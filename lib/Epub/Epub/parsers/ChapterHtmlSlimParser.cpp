#include "ChapterHtmlSlimParser.h"

#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Utf8.h>
#include <XmlParserUtils.h>
#include <expat.h>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <new>

#include "Epub.h"
#include "Epub/Page.h"
#include "Epub/converters/ImageDecoderFactory.h"
#include "Epub/converters/ImageDimsProbe.h"
#include "Epub/converters/ImageToFramebufferDecoder.h"
#include "Epub/converters/PngToFramebufferConverter.h"
#include "Epub/htmlEntities.h"

// Minimum file size (in bytes) to show indexing popup - smaller chapters don't benefit from it
constexpr size_t MIN_SIZE_FOR_POPUP = 10 * 1024;  // 10KB
constexpr size_t PARSE_BUFFER_SIZE = 1024;

// This number comes from PR #73
// If we have > 750 words buffered up, perform the layout and consume out all but the last line
// There should be enough here to build out 1-2 full pages and doing this will free up a lot of
// memory.
// Spotted when reading Intermezzo, there are some really long text blocks in there.
constexpr size_t TEXT_BLOCK_SOFT_FLUSH_WORDS = 750;

// When CSS is enabled, flush earlier to save RAM. 320 is still more than enough to build a CJK
// page at font size 14
constexpr size_t TEXT_BLOCK_SOFT_FLUSH_WORDS_WITH_CSS = 320;

// Hard cap on the number of anchor IDs recorded per chapter. Legitimate navigation
// anchors (TOC entries, footnotes, cross-references) rarely exceed a few hundred per
// chapter. A runaway count usually means a converter injected machine-generated IDs on
// every text fragment (e.g. Kobo KePub spans). The cap prevents unbounded heap growth
// on resource-constrained devices (~380KB heap). TOC anchors bypass this cap.
constexpr size_t MAX_ANCHORS_PER_CHAPTER = 1024;

constexpr const char* HEADER_TAGS[] = {"h1", "h2", "h3", "h4", "h5", "h6"};
constexpr const char* BLOCK_TAGS[] = {"p", "li", "div", "br", "blockquote"};
constexpr const char* BOLD_TAGS[] = {"b", "strong"};
constexpr const char* ITALIC_TAGS[] = {"i", "em"};
constexpr const char* UNDERLINE_TAGS[] = {"u", "ins"};
constexpr const char* LINETHROUGH_TAGS[] = {"del", "s", "strike"};
constexpr const char* IMAGE_TAGS[] = {"img", "image"};
constexpr const char* SKIP_TAGS[] = {"head"};

bool isWhitespace(const char c) { return c == ' ' || c == '\r' || c == '\n' || c == '\t'; }

bool matches(const char* tag_name, const char* const* possible_tags, size_t count) {
  for (size_t i = 0; i < count; i++) {
    if (strcmp(tag_name, possible_tags[i]) == 0) {
      return true;
    }
  }
  return false;
}

const char* getAttribute(const XML_Char** atts, const char* attrName) {
  if (!atts) return nullptr;
  for (int i = 0; atts[i]; i += 2) {
    if (strcmp(atts[i], attrName) == 0) return atts[i + 1];
  }
  return nullptr;
}

// Returns true if the HTML element is a purely inline, non-navigable wrapper.
// IDs on these elements are never meaningful navigation targets in epub content.
// Reading-system converters (Kobo KePub, Calibre, etc.) frequently inject thousands
// of such IDs for progress tracking or internal bookkeeping, and recording each one
// as a navigation anchor exhausts the heap on memory-constrained devices.
// Block-level, sectioning, and structural elements are always considered navigable.
bool isNonNavigableInlineElement(const char* name) { return strcmp(name, "span") == 0; }

bool isInternalEpubLink(const char* href) {
  if (!href || href[0] == '\0') return false;
  if (strncmp(href, "http://", 7) == 0 || strncmp(href, "https://", 8) == 0) return false;
  if (strncmp(href, "mailto:", 7) == 0) return false;
  if (strncmp(href, "ftp://", 6) == 0) return false;
  if (strncmp(href, "tel:", 4) == 0) return false;
  if (strncmp(href, "javascript:", 11) == 0) return false;
  return true;
}

bool isHeaderOrBlock(const char* name) {
  return matches(name, HEADER_TAGS, std::size(HEADER_TAGS)) || matches(name, BLOCK_TAGS, std::size(BLOCK_TAGS));
}

bool isTableStructuralTag(const char* name) {
  return strcmp(name, "table") == 0 || strcmp(name, "tr") == 0 || strcmp(name, "td") == 0 || strcmp(name, "th") == 0;
}

void ChapterHtmlSlimParser::applyDirectionToEntry(StyleStackEntry& entry, const CssStyle& css) {
  if (css.hasDirection()) {
    entry.hasDirection = true;
    entry.direction = css.direction;
  }
}

EpdFontFamily::Style ChapterHtmlSlimParser::fontStyleForTextDecoration(const CssTextDecoration decoration) {
  EpdFontFamily::Style style = EpdFontFamily::REGULAR;
  if ((decoration & CssTextDecoration::Underline) != CssTextDecoration::None) {
    style = static_cast<EpdFontFamily::Style>(style | EpdFontFamily::UNDERLINE);
  }
  if ((decoration & CssTextDecoration::LineThrough) != CssTextDecoration::None) {
    style = static_cast<EpdFontFamily::Style>(style | EpdFontFamily::STRIKETHROUGH);
  }
  return style;
}

void ChapterHtmlSlimParser::applyTextDecorationToEntry(StyleStackEntry& entry, const CssStyle& css) {
  if (css.hasTextDecoration()) {
    entry.hasTextDecoration = true;
    entry.textDecoration = css.textDecoration;
  }
}

void ChapterHtmlSlimParser::pushDecorationStyleEntry(const CssTextDecoration defaultDecoration,
                                                     const CssStyle& cssStyle) {
  StyleStackEntry entry;
  entry.depth = depth;
  entry.hasTextDecoration = true;
  entry.textDecoration = cssStyle.hasTextDecoration() ? cssStyle.textDecoration : defaultDecoration;
  if (cssStyle.hasFontWeight()) {
    entry.hasBold = true;
    entry.bold = cssStyle.fontWeight == CssFontWeight::Bold;
  }
  if (cssStyle.hasFontStyle()) {
    entry.hasItalic = true;
    entry.italic = cssStyle.fontStyle == CssFontStyle::Italic;
  }
  applyDirectionToEntry(entry, cssStyle);
  inlineStyleStack.push_back(entry);
  updateEffectiveInlineStyle();
}

// Update effective bold/italic/decorations based on block style and inline style stack
void ChapterHtmlSlimParser::updateEffectiveInlineStyle() {
  // Start with block-level styles
  effectiveBold = currentCssStyle.hasFontWeight() && currentCssStyle.fontWeight == CssFontWeight::Bold;
  effectiveItalic = currentCssStyle.hasFontStyle() && currentCssStyle.fontStyle == CssFontStyle::Italic;
  effectiveTextDecoration =
      currentCssStyle.hasTextDecoration() ? currentCssStyle.textDecoration : CssTextDecoration::None;
  effectiveDirectionDefined = currentCssStyle.hasDirection();
  effectiveDirection = currentCssStyle.direction;
  effectiveSup = false;
  effectiveSub = false;

  // Apply inline style stack in order
  for (const auto& entry : inlineStyleStack) {
    if (entry.hasBold) {
      effectiveBold = entry.bold;
    }
    if (entry.hasItalic) {
      effectiveItalic = entry.italic;
    }
    // CSS line decorations propagate through descendants; child entries add
    // their own lines but cannot cancel an ancestor's already active line.
    if (entry.hasTextDecoration) {
      effectiveTextDecoration = effectiveTextDecoration | entry.textDecoration;
    }
    if (entry.hasDirection) {
      effectiveDirectionDefined = true;
      effectiveDirection = entry.direction;
    }
    if (entry.hasSup) {
      effectiveSup = entry.sup;
      if (entry.sup) effectiveSub = false;
    }
    if (entry.hasSub) {
      effectiveSub = entry.sub;
      if (entry.sub) effectiveSup = false;
    }
  }

  // Keep inherited direction in the active empty text block so upcoming block starts
  // can inherit from non-block ancestors such as <html dir="rtl"> / <body dir="rtl">.
  if (currentTextBlock && currentTextBlock->isEmpty()) {
    auto& style = currentTextBlock->getBlockStyle();
    if (effectiveDirectionDefined) {
      style.directionDefined = true;
      style.isRtl = (effectiveDirection == CssTextDirection::Rtl);
    } else {
      style.directionDefined = false;
      style.isRtl = false;
    }
  }
}

void ChapterHtmlSlimParser::flushPendingAnchor() {
  if (pendingAnchorId.empty()) return;

  // If the pending anchor is a TOC chapter boundary, force a page break after the previous
  // block is flushed so the chapter starts on a fresh page.
  if (std::find(tocAnchors.begin(), tocAnchors.end(), pendingAnchorId) != tocAnchors.end()) {
    if (currentPage && !currentPage->elements.empty()) {
      completePageFn(std::move(currentPage), xpathParagraphIndex, xpathListItemIndex);
      completedPageCount++;
      currentPage.reset(new Page());
      currentPageNextY = 0;
    }
  }

  // Record deferred anchor after previous block is flushed (and any TOC page break)
  anchorData.push_back({std::move(pendingAnchorId), static_cast<uint16_t>(completedPageCount)});
  pendingAnchorId.clear();
}

// flush the contents of partWordBuffer to currentTextBlock
void ChapterHtmlSlimParser::flushPartWordBuffer() {
  // Determine font style from depth-based tracking and CSS effective style
  const bool isBold = boldUntilDepth < depth || effectiveBold;
  const bool isItalic = italicUntilDepth < depth || effectiveItalic;

  // Combine style flags using bitwise OR
  EpdFontFamily::Style fontStyle = EpdFontFamily::REGULAR;
  if (isBold) {
    fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::BOLD);
  }
  if (isItalic) {
    fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::ITALIC);
  }
  fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | fontStyleForTextDecoration(effectiveTextDecoration));
  if (effectiveSup) {
    fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::SUP);
  } else if (effectiveSub) {
    fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::SUB);
  }

  // flush the buffer
  partWordBuffer[partWordBufferIndex] = '\0';
  currentTextBlock->addWord(partWordBuffer, fontStyle, false, nextWordContinues);
  partWordBufferIndex = 0;
  nextWordContinues = false;
  listItemBulletOnly = false;
}

// start a new text block if needed
void ChapterHtmlSlimParser::startNewTextBlock(const BlockStyle& blockStyle) {
  nextWordContinues = false;  // New block = new paragraph, no continuation
  if (currentTextBlock) {
    // already have a text block running and it is empty - just reuse it
    if (currentTextBlock->isEmpty()) {
      // The stack accumulates horizontal margins and text properties from ancestors.
      // Vertical margins are per-element and not inherited through the stack, but
      // container elements deposit their vertical margins on the empty block when they
      // open. Merge those into the new style so the first child in a container inherits
      // the container's vertical spacing.
      const auto style = currentTextBlock->getBlockStyle();
      BlockStyle incoming = blockStyle;
      if (style.fromBrElement) {
        // The empty block was created by a <br> section separator. Inject a full line of
        // blank space before the following paragraph so the scene/section break is visible.
        // This only fires when the <br> block stayed empty (i.e. no inline text was added).
        const int16_t lineHeight = static_cast<int16_t>(lineAdvancePx(incoming));
        incoming.marginTop = static_cast<int16_t>(incoming.marginTop + lineHeight);
      }

      currentTextBlock->setBlockStyle(style.getCombinedBlockStyle(incoming, BlockStyle::CombineAxis::Vertical));

      flushPendingAnchor();
      // Reused empty block is still the "next" paragraph for drop-cap arming.
      if (armDropCapOnNextTextBlock_) {
        pendingDropCap_ = true;
        armDropCapOnNextTextBlock_ = false;
        LOG_DBG("RLC", "Drop-cap pending on reused empty text block");
      }
      return;
    }

    // <li> added a bullet as the first word, making the block non-empty. When a nested
    // block-level child (<p>, <div>, etc.) opens, reuse the block instead of flushing
    // the bullet to its own line. The bullet stays inline with the child's text.
    if (listItemBulletOnly) {
      const auto style = currentTextBlock->getBlockStyle();
      currentTextBlock->setBlockStyle(style.getCombinedBlockStyle(blockStyle, BlockStyle::CombineAxis::Vertical));
      listItemBulletOnly = false;
      flushPendingAnchor();
      return;
    }

    // Flush previous paragraph BEFORE arming drop-cap on the new block.
    makePages();
  }
  // If the pending anchor is a TOC chapter boundary, force a page break after the previous
  // block is flushed so the chapter starts on a fresh page.
  flushPendingAnchor();
  currentTextBlock.reset(
      new ParsedText(extraParagraphSpacing, hyphenationEnabled, focusReadingEnabled, guideReadingEnabled, blockStyle));
  wordsExtractedInBlock = 0;
  listItemBulletOnly = false;
  if (armDropCapOnNextTextBlock_) {
    pendingDropCap_ = true;
    armDropCapOnNextTextBlock_ = false;
    LOG_DBG("RLC", "Drop-cap pending on new text block");
  }
}

void ChapterHtmlSlimParser::emitHorizontalRule(const BlockStyle& blockStyle) {
  if (partWordBufferIndex > 0) {
    flushPartWordBuffer();
  }

  if (currentTextBlock) {
    const BlockStyle parentBlockStyle = currentTextBlock->getBlockStyle();
    startNewTextBlock(parentBlockStyle);
  }

  if (!currentPage) {
    currentPage.reset(new (std::nothrow) Page());
    if (!currentPage) {
      LOG_ERR("EHP", "Failed to create page for horizontal rule");
      return;
    }
    currentPageNextY = 0;
  }

  const int16_t lineHeight = static_cast<int16_t>(lineAdvancePx(blockStyle));
  const int16_t defaultVerticalSpacing = static_cast<int16_t>(lineHeight / 2);
  const int16_t topSpacing =
      static_cast<int16_t>((blockStyle.marginTop > 0 ? blockStyle.marginTop : defaultVerticalSpacing) +
                           (blockStyle.paddingTop > 0 ? blockStyle.paddingTop : 0));
  const int16_t bottomSpacing =
      static_cast<int16_t>((blockStyle.marginBottom > 0 ? blockStyle.marginBottom : defaultVerticalSpacing) +
                           (blockStyle.paddingBottom > 0 ? blockStyle.paddingBottom : 0));
  constexpr uint8_t ruleThickness = 2;
  const int16_t availableWidth =
      std::max<int16_t>(1, static_cast<int16_t>(viewportWidth - blockStyle.totalHorizontalInset()));
  const int16_t width = std::max<int16_t>(1, static_cast<int16_t>(availableWidth / 4));
  const int16_t xPos = static_cast<int16_t>(blockStyle.leftInset() + ((availableWidth - width) / 2));
  const int16_t totalHeight = static_cast<int16_t>(topSpacing + ruleThickness + bottomSpacing);

  if (!currentPage->elements.empty() && currentPageNextY + totalHeight > viewportHeight) {
    completePageFn(std::move(currentPage), xpathParagraphIndex, xpathListItemIndex);
    completedPageCount++;
    currentPage.reset(new (std::nothrow) Page());
    if (!currentPage) {
      LOG_ERR("EHP", "Failed to create page after horizontal-rule page break");
      return;
    }
    currentPageNextY = 0;
  }

  currentPageNextY += topSpacing;

  auto pageRule = std::shared_ptr<PageHorizontalRule>(
      new (std::nothrow) PageHorizontalRule(width, ruleThickness, xPos, currentPageNextY));
  if (!pageRule) {
    LOG_ERR("EHP", "Failed to create PageHorizontalRule");
    return;
  }
  currentPage->elements.push_back(pageRule);
  currentPageNextY = static_cast<int16_t>(currentPageNextY + ruleThickness + bottomSpacing);

  if (!pendingAnchorId.empty()) {
    anchorData.push_back({std::move(pendingAnchorId), static_cast<uint16_t>(completedPageCount)});
    pendingAnchorId.clear();
  }
}

void XMLCALL ChapterHtmlSlimParser::startElement(void* userData, const XML_Char* name, const XML_Char** atts) {
  auto* self = static_cast<ChapterHtmlSlimParser*>(userData);

  // Middle of skip
  if (self->skipUntilDepth < self->depth) {
    self->depth += 1;
    return;
  }

  if (strcmp(name, "p") == 0) {
    self->xpathParagraphIndex++;
  }
  if (strcmp(name, "li") == 0) {
    self->xpathListItemIndex++;
  }

  // Extract class, style, id, dir, and HTML align attributes for CSS/RTL processing
  std::string classAttr;
  std::string styleAttr;
  std::string dirAttr;
  std::string alignAttr;
  if (atts != nullptr) {
    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "class") == 0) {
        classAttr = atts[i + 1];
      } else if (strcmp(atts[i], "style") == 0) {
        styleAttr = atts[i + 1];
      } else if (strcmp(atts[i], "id") == 0) {
        // Defer both anchor recording and TOC page breaks until startNewTextBlock,
        // after the previous block is flushed to pages via makePages().
        //
        // Skip IDs on non-navigable inline elements (e.g. <span>): these are never
        // link targets in epub content, but reading-system converters can inject tens
        // of thousands of them per chapter, exhausting the heap. TOC anchors are
        // always recorded regardless of element type, since they drive page breaks.
        const char* idValue = atts[i + 1];
        const bool isTocAnchor =
            std::find(self->tocAnchors.begin(), self->tocAnchors.end(), idValue) != self->tocAnchors.end();
        if (isTocAnchor || (!isNonNavigableInlineElement(name) && self->anchorData.size() < MAX_ANCHORS_PER_CHAPTER)) {
          // Flush a displaced anchor before overwriting. Consecutive non-block elements
          // (e.g. <aside id="fn1">text</aside><aside id="fn2">) with no intervening block
          // never trigger startNewTextBlock, so fn1 gets silently overwritten. That leaves
          // fn1 missing from the anchor map -> getPageForAnchor returns nullopt -> reader
          // lands at page 0 (section start) instead of the footnote.
          if (!self->pendingAnchorId.empty()) {
            self->flushPendingAnchor();
          }
          self->pendingAnchorId = idValue;
        }
      } else if (strcmp(atts[i], "dir") == 0) {
        dirAttr = atts[i + 1];
      } else if (strcmp(atts[i], "align") == 0) {
        // Legacy HTML align="center|left|right|justify" on p/h1/div/td.
        alignAttr = atts[i + 1];
      }
    }
  }

  auto centeredBlockStyle = BlockStyle();
  centeredBlockStyle.textAlignDefined = true;
  centeredBlockStyle.alignment = CssTextAlign::Center;

  // Compute CSS style for this element early so display:none can short-circuit
  // before tag-specific branches emit any content or metadata.
  //
  // Full stylesheets load only when embeddedStyle is on. Inline style="" and
  // HTML align= still apply always — many EPUBs center chapter titles that way
  // without a class rule, and Casper defaults embedded style off for speed.
  CssStyle cssStyle;
  if (self->cssParser) {
    cssStyle = self->cssParser->resolveStyle(name, classAttr);
  }
  if (!styleAttr.empty()) {
    CssStyle inlineStyle = CssParser::parseInlineStyle(styleAttr);
    cssStyle.applyOver(inlineStyle);
  }
  if (!alignAttr.empty() && !cssStyle.hasTextAlign()) {
    if (strcasecmp(alignAttr.c_str(), "center") == 0) {
      cssStyle.textAlign = CssTextAlign::Center;
      cssStyle.defined.textAlign = 1;
    } else if (strcasecmp(alignAttr.c_str(), "right") == 0) {
      cssStyle.textAlign = CssTextAlign::Right;
      cssStyle.defined.textAlign = 1;
    } else if (strcasecmp(alignAttr.c_str(), "left") == 0) {
      cssStyle.textAlign = CssTextAlign::Left;
      cssStyle.defined.textAlign = 1;
    } else if (strcasecmp(alignAttr.c_str(), "justify") == 0) {
      cssStyle.textAlign = CssTextAlign::Justify;
      cssStyle.defined.textAlign = 1;
    }
  }

  // Class / TOC title centering when CSS text-align never resolved.
  // Our stylesheet engine only keeps simple tag/.class/tag.class rules — it
  // drops descendant selectors ("div.chapter h1", ".body .title"). Commercial
  // books (e.g. Empire of the Vampire) then fall back to Justify, so short
  // chapter heads look left-aligned with Book's Style + Embedded on.
  if (!cssStyle.hasTextAlign()) {
    auto classSuggestsCenter = [](const std::string& classAttr) -> bool {
      if (classAttr.empty()) return false;
      // Normalize to lower + separators → space so tokens match cleanly.
      std::string cls;
      cls.reserve(classAttr.size());
      for (unsigned char c : classAttr) {
        if (c >= 'A' && c <= 'Z') c = static_cast<unsigned char>(c - 'A' + 'a');
        if (c == '_' || c == '-' || c == '.') c = ' ';
        cls.push_back(static_cast<char>(c));
      }
      // Whole-string cues first (hyphenated names become multi-token after normalize).
      if (cls.find("chapter title") != std::string::npos || cls.find("chaptertitle") != std::string::npos ||
          cls.find("chap title") != std::string::npos || cls.find("chaptitle") != std::string::npos ||
          cls.find("book title") != std::string::npos || cls.find("booktitle") != std::string::npos ||
          cls.find("chapter number") != std::string::npos || cls.find("chapternumber") != std::string::npos ||
          cls.find("chapter name") != std::string::npos || cls.find("chaptername") != std::string::npos) {
        return true;
      }
      size_t i = 0;
      while (i < cls.size()) {
        while (i < cls.size() && cls[i] == ' ') ++i;
        const size_t start = i;
        while (i < cls.size() && cls[i] != ' ') ++i;
        if (start >= i) break;
        const std::string_view tok(cls.data() + start, i - start);
        if (tok == "center" || tok == "centred" || tok == "centered" || tok == "title" || tok == "subtitle" ||
            tok == "heading" || tok == "header" || tok == "caption" ||
            // Calibre often uses title1 / title2 for chapter heads
            (tok.size() >= 5 && tok.compare(0, 5, "title") == 0)) {
          return true;
        }
      }
      return false;
    };
    bool center = classSuggestsCenter(classAttr);
    // TOC chapter targets are almost always the displayed chapter title.
    if (!center && !self->pendingAnchorId.empty() &&
        std::find(self->tocAnchors.begin(), self->tocAnchors.end(), self->pendingAnchorId) != self->tocAnchors.end()) {
      // Only treat as title when this element is a natural title host — not a
      // whole-chapter wrapper with tons of nested body text (span/div catch-all).
      if (matches(name, HEADER_TAGS, std::size(HEADER_TAGS)) || strcmp(name, "p") == 0) {
        center = true;
      }
    }
    if (center) {
      cssStyle.textAlign = CssTextAlign::Center;
      cssStyle.defined.textAlign = 1;
    }
  }

  // HTML dir attribute overrides CSS direction (case-insensitive per HTML spec)
  if (!dirAttr.empty()) {
    if (strcasecmp(dirAttr.c_str(), "rtl") == 0) {
      cssStyle.direction = CssTextDirection::Rtl;
      cssStyle.defined.direction = 1;
    } else if (strcasecmp(dirAttr.c_str(), "ltr") == 0) {
      cssStyle.direction = CssTextDirection::Ltr;
      cssStyle.defined.direction = 1;
    }
  }

  // Direction is inherited in HTML/CSS. If this element does not define one, carry
  // the currently active inherited direction into its computed style.
  if (!cssStyle.hasDirection() && self->effectiveDirectionDefined) {
    cssStyle.direction = self->effectiveDirection;
    cssStyle.defined.direction = 1;
  }

  // Skip elements with display:none before all fast paths (tables, links, etc.).
  if (cssStyle.hasDisplay() && cssStyle.display == CssDisplay::None) {
    self->skipUntilDepth = self->depth;
    self->depth += 1;
    return;
  }

  // True-to-book floats: CSS float is often on a wrapper div (.figleft/.figright in
  // Project Gutenberg), not on the <img>. Record open float containers for children.
  // Class-name fallback: if the stylesheet failed to apply float (stale cache,
  // selector limits), still honor Gutenberg/Calibre class conventions.
  CssFloat wrapperFloat = CssFloat::None;
  if (self->embeddedStyle) {
    if (cssStyle.hasFloatSide() && (cssStyle.floatSide == CssFloat::Left || cssStyle.floatSide == CssFloat::Right)) {
      wrapperFloat = cssStyle.floatSide;
    } else if (!classAttr.empty()) {
      // Case-insensitive token search for common float figure classes.
      std::string cls = classAttr;
      for (char& c : cls) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
      }
      if (cls.find("figleft") != std::string::npos || cls.find("floatleft") != std::string::npos ||
          cls.find("float-left") != std::string::npos || cls.find("alignleft") != std::string::npos) {
        wrapperFloat = CssFloat::Left;
      } else if (cls.find("figright") != std::string::npos || cls.find("floatright") != std::string::npos ||
                 cls.find("float-right") != std::string::npos || cls.find("alignright") != std::string::npos) {
        wrapperFloat = CssFloat::Right;
      }
    }
  }
  if (wrapperFloat != CssFloat::None && self->floatInheritCount_ < kMaxFloatInherit) {
    FloatInherit& inh = self->floatInherit_[self->floatInheritCount_++];
    inh.depth = self->depth;
    inh.side = wrapperFloat;
    inh.cssWidthPx = 0;
    if (cssStyle.hasImageWidth()) {
      const float em = static_cast<float>(self->renderer.getFontAscenderSize(self->fontId));
      const int w = static_cast<int>(cssStyle.imageWidth.toPixels(em, static_cast<float>(self->viewportWidth)) + 0.5f);
      if (w > 0) inh.cssWidthPx = static_cast<int16_t>(std::min(w, static_cast<int>(self->viewportWidth)));
    }
    LOG_DBG("RLC", "Float inherit push %s depth=%d cssW=%d", inh.side == CssFloat::Left ? "left" : "right", inh.depth,
            static_cast<int>(inh.cssWidthPx));
  }

  // Push CSS width for non-img blocks so nested % / width:100% on <img> resolve against
  // the book wrapper (DCC .class_sb width:11.2em > .class_saw 28.1% > img 100%).
  if (strcmp(name, "img") != 0 && cssStyle.hasImageWidth() && self->widthContainCount_ < kMaxWidthContain) {
    const float em = static_cast<float>(self->renderer.getFontAscenderSize(self->fontId));
    int parentW = self->viewportWidth;
    if (self->widthContainCount_ > 0 && self->widthContain_[self->widthContainCount_ - 1].cssWidthPx > 0) {
      parentW = self->widthContain_[self->widthContainCount_ - 1].cssWidthPx;
    }
    const int w = static_cast<int>(cssStyle.imageWidth.toPixels(em, static_cast<float>(parentW)) + 0.5f);
    if (w > 0) {
      WidthContain& wc = self->widthContain_[self->widthContainCount_++];
      wc.depth = self->depth;
      wc.cssWidthPx = static_cast<int16_t>(std::min(w, static_cast<int>(self->viewportWidth)));
      LOG_DBG("RLC", "Width contain push depth=%d cssW=%d (parent=%d)", wc.depth, static_cast<int>(wc.cssWidthPx),
              parentW);
    }
  }

  // Tables are layout vehicles in many EPUBs (Alice rabbithole, caption|image pairs).
  // Stream cell content as normal blocks — never inject "Tab Row N, Cell M:" chrome.
  if (strcmp(name, "table") == 0) {
    // skip nested tables
    if (self->tableDepth > 0) {
      self->tableDepth += 1;
      return;
    }

    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
    }
    // True-to-book: CSS background-image on the table (Alice rabbit-hole art).
    if (self->embeddedStyle && self->cssParser) {
      const std::string bgSrc = self->cssParser->resolveBackgroundImage(name, classAttr);
      if (!bgSrc.empty()) {
        self->placeEpubImageSrc(bgSrc, cssStyle, /*forceFloat=*/CssFloat::None);
      }
    }
    self->tableDepth += 1;
    self->tableRowIndex = 0;
    self->tableColIndex = 0;
    self->depth += 1;
    return;
  }

  if (self->tableDepth == 1 && strcmp(name, "tr") == 0) {
    self->tableRowIndex += 1;
    self->tableColIndex = 0;
    self->depth += 1;
    return;
  }

  if (self->tableDepth == 1 && (strcmp(name, "td") == 0 || strcmp(name, "th") == 0)) {
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
    }
    self->tableColIndex += 1;

    // Apply cell CSS (text-align, etc.) when Embedded Style is on; otherwise user align.
    const float emSize = static_cast<float>(self->renderer.getFontAscenderSize(self->fontId));
    auto tableCellBlockStyle = BlockStyle::fromCssStyle(
        cssStyle, emSize, static_cast<CssTextAlign>(self->paragraphAlignment), self->viewportWidth);
    if (!tableCellBlockStyle.textAlignDefined) {
      tableCellBlockStyle.textAlignDefined = true;
      tableCellBlockStyle.alignment = (self->paragraphAlignment == static_cast<uint8_t>(CssTextAlign::None))
                                          ? CssTextAlign::Justify
                                          : static_cast<CssTextAlign>(self->paragraphAlignment);
    }
    self->startNewTextBlock(tableCellBlockStyle);

    self->depth += 1;
    return;
  }

  if (self->tableDepth == 1 && strcmp(name, "hr") == 0) {
    self->depth += 1;
    return;
  }

  if (matches(name, IMAGE_TAGS, std::size(IMAGE_TAGS))) {
    std::string src;
    std::string alt;
    if (atts != nullptr) {
      for (int i = 0; atts[i]; i += 2) {
        if (strcmp(atts[i], "src") == 0) {
          src = atts[i + 1];
        } else if (src.empty() && (strcmp(atts[i], "href") == 0 || strcmp(atts[i], "xlink:href") == 0)) {
          src = atts[i + 1];
        } else if (strcmp(atts[i], "alt") == 0) {
          alt = atts[i + 1];
        }
      }

      const size_t fragmentPos = src.find('#');
      if (fragmentPos != std::string::npos) {
        src.resize(fragmentPos);
      }

      // imageRendering: 0=display, 1=placeholder (alt text only), 2=suppress entirely
      if (self->imageRendering == 2) {
        self->skipUntilDepth = self->depth;
        self->depth += 1;
        return;
      }

      if (!src.empty() && self->imageRendering != 1) {
        LOG_DBG("EHP", "Found image: src=%s", src.c_str());

        {
          // Resolve the image path relative to the HTML file
          std::string resolvedPath = FsHelpers::normalisePath(FsHelpers::decodeUriEscapes(self->contentBase + src));

          if (ImageDecoderFactory::isFormatSupported(resolvedPath)) {
            // Create a unique filename for the cached image
            std::string ext;
            size_t extPos = resolvedPath.rfind('.');
            if (extPos != std::string::npos) {
              ext = resolvedPath.substr(extPos);
            }
            std::string cachedImagePath = self->imageBasePath + std::to_string(self->imageCounter++) + ext;

            {
              // Probe the dimensions from the entry's first bytes (early-aborted
              // inflate, a few KB) instead of extracting the whole image now —
              // extraction is deferred to the first render of the page (see
              // ImageBlock's lazy extractor). This is what keeps first-open of an
              // image-heavy chapter from stalling for seconds per image.
              ImageDimensions dims = {0, 0};
              // Streaming ZIP inflate needs ~11KB tinfl state + 32KB window when
              // BuildScratch is not lent. After a chapter jump maxAlloc often sits
              // ~20KB under font-cache fragmentation → "Failed to init inflate" and
              // an empty full-page plate. Reclaim once before the probe.
              auto reclaimForZipInflate = [&]() {
                constexpr size_t kZipStreamNeed = 48 * 1024;
                if (ESP.getMaxAllocHeap() >= kZipStreamNeed) return;
                PngToFramebufferConverter::releaseWarmIfHeapTight(kZipStreamNeed);
                if (FontCacheManager* fcm = self->renderer.getFontCacheManager()) {
                  if (!fcm->isScanning()) fcm->clearCache();
                }
                LOG_DBG("EHP", "reclaim for ZIP inflate free=%u maxAlloc=%u", static_cast<unsigned>(ESP.getFreeHeap()),
                        static_cast<unsigned>(ESP.getMaxAllocHeap()));
              };
              reclaimForZipInflate();

              ImageDimsProbe headerProbe;
              self->epub->readItemContentsToStream(resolvedPath, headerProbe, 1024, /*allowEarlyStop=*/true);
              bool gotDimensions = headerProbe.getDimensions(dims);

              if (!gotDimensions) {
                // No header within the stream (rare) — fall back to extracting the
                // whole image and probing the file. That can take seconds, so
                // surface the indexing popup first (single-shot per parser).
                if (self->popupFn && !self->imagePopupFired) {
                  self->imagePopupFired = true;
                  self->popupFn();
                }
                reclaimForZipInflate();
                HalFile cachedImageFile;
                bool extractSuccess = false;
                if (Storage.openFileForWrite("EHP", cachedImagePath, cachedImageFile)) {
                  extractSuccess = self->epub->readItemContentsToStream(resolvedPath, cachedImageFile, 4096);
                  cachedImageFile.flush();
                  cachedImageFile.close();
                }
                if (extractSuccess) {
                  // Retry to absorb SD-card sync latency on slow cards, and to close
                  // the silent-drop bug where a single getDimensions failure was fatal.
                  ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(cachedImagePath);
                  for (int attempt = 0; attempt < 3 && !gotDimensions; attempt++) {
                    if (attempt > 0) {
                      delay(50);  // Give a slow SD card time to finish syncing before retrying
                    }
                    gotDimensions = decoder && decoder->getDimensions(cachedImagePath, dims);
                  }
                } else {
                  LOG_ERR("EHP", "Failed to extract image free=%u maxAlloc=%u",
                          static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
                }
              }

              if (gotDimensions) {
                LOG_DBG("EHP", "Image dimensions: %dx%d", dims.width, dims.height);

                int displayWidth = 0;
                int displayHeight = 0;
                const float emSize = static_cast<float>(self->renderer.getFontAscenderSize(self->fontId));
                const CssStyle& imgStyle = cssStyle;
                // Parent float wrappers (.figleft/.figright) often carry width, not <img>.
                const int16_t inheritWidthPx =
                    (self->floatInheritCount_ > 0) ? self->floatInherit_[self->floatInheritCount_ - 1].cssWidthPx : 0;
                const bool hasCssHeight = imgStyle.hasImageHeight();
                const bool hasCssWidth = imgStyle.hasImageWidth() || inheritWidthPx > 0;

                // Compute effective container width for percentage-based image sizes.
                // If the image is inside a block with horizontal margins/padding (e.g.
                // <div style="margin: 1em 40%">), percentage widths like width:100%
                // should resolve against the container width, not the full viewport.
                // Nested width:11.2em wrappers (DCC scene rules) further constrain.
                int containerWidth = self->viewportWidth;
                if (self->currentTextBlock) {
                  const int inset = self->currentTextBlock->getBlockStyle().totalHorizontalInset();
                  if (inset > 0 && inset < self->viewportWidth) {
                    containerWidth = self->viewportWidth - inset;
                  }
                }
                if (self->widthContainCount_ > 0) {
                  const int16_t cw = self->widthContain_[self->widthContainCount_ - 1].cssWidthPx;
                  if (cw > 0 && cw < containerWidth) {
                    containerWidth = cw;
                  }
                }
                // Floated figures: CSS float on <img>, or inherited from wrapper
                // (.figleft/.figright). Prefer natural/CSS width — do NOT force
                // half-page scaling for small letter images (71px drop-caps).
                CssFloat pendingFloat = CssFloat::None;
                if (self->embeddedStyle) {
                  if (cssStyle.hasFloatSide() &&
                      (cssStyle.floatSide == CssFloat::Left || cssStyle.floatSide == CssFloat::Right)) {
                    pendingFloat = cssStyle.floatSide;
                  } else if (self->floatInheritCount_ > 0) {
                    pendingFloat = self->floatInherit_[self->floatInheritCount_ - 1].side;
                  }
                }
                // Only cap container for % widths / fit-scale — explicit px widths
                // (figright style="width:183px") must keep their book size.
                const bool explicitFloatWidth = (inheritWidthPx > 0) || imgStyle.hasImageWidth();
                if (pendingFloat != CssFloat::None && !explicitFloatWidth) {
                  const int floatMax = std::max(48, static_cast<int>(self->viewportWidth) * 48 / 100);
                  if (containerWidth > floatMax) containerWidth = floatMax;
                }

                if (hasCssHeight && hasCssWidth && dims.width > 0 && dims.height > 0) {
                  // Both CSS height and width set: resolve both, then clamp to viewport preserving requested ratio
                  displayHeight = static_cast<int>(
                      imgStyle.imageHeight.toPixels(emSize, static_cast<float>(self->viewportHeight)) + 0.5f);
                  if (imgStyle.hasImageWidth()) {
                    displayWidth = static_cast<int>(
                        imgStyle.imageWidth.toPixels(emSize, static_cast<float>(containerWidth)) + 0.5f);
                  } else {
                    displayWidth = static_cast<int>(inheritWidthPx);
                  }
                  if (displayHeight < 1) displayHeight = 1;
                  if (displayWidth < 1) displayWidth = 1;
                  if (displayWidth > containerWidth || displayHeight > self->viewportHeight) {
                    float scaleX =
                        (displayWidth > containerWidth) ? static_cast<float>(containerWidth) / displayWidth : 1.0f;
                    float scaleY = (displayHeight > self->viewportHeight)
                                       ? static_cast<float>(self->viewportHeight) / displayHeight
                                       : 1.0f;
                    float scale = (scaleX < scaleY) ? scaleX : scaleY;
                    displayWidth = static_cast<int>(displayWidth * scale + 0.5f);
                    displayHeight = static_cast<int>(displayHeight * scale + 0.5f);
                    if (displayWidth < 1) displayWidth = 1;
                    if (displayHeight < 1) displayHeight = 1;
                  }
                  LOG_DBG("EHP", "Display size from CSS height+width: %dx%d", displayWidth, displayHeight);
                } else if (hasCssHeight && !hasCssWidth && dims.width > 0 && dims.height > 0) {
                  // Use CSS height (resolve % against viewport height) and derive width from aspect ratio
                  displayHeight = static_cast<int>(
                      imgStyle.imageHeight.toPixels(emSize, static_cast<float>(self->viewportHeight)) + 0.5f);
                  if (displayHeight < 1) displayHeight = 1;
                  displayWidth =
                      static_cast<int>(displayHeight * (static_cast<float>(dims.width) / dims.height) + 0.5f);
                  if (displayHeight > self->viewportHeight) {
                    displayHeight = self->viewportHeight;
                    // Rescale width to preserve aspect ratio when height is clamped
                    displayWidth =
                        static_cast<int>(displayHeight * (static_cast<float>(dims.width) / dims.height) + 0.5f);
                    if (displayWidth < 1) displayWidth = 1;
                  }
                  if (displayWidth > containerWidth) {
                    displayWidth = containerWidth;
                    // Rescale height to preserve aspect ratio when width is clamped
                    displayHeight =
                        static_cast<int>(displayWidth * (static_cast<float>(dims.height) / dims.width) + 0.5f);
                    if (displayHeight < 1) displayHeight = 1;
                  }
                  if (displayWidth < 1) displayWidth = 1;
                  LOG_DBG("EHP", "Display size from CSS height: %dx%d", displayWidth, displayHeight);
                } else if (hasCssWidth && !hasCssHeight && dims.width > 0 && dims.height > 0) {
                  // Use CSS width (img or inherited wrapper) and derive height from aspect ratio
                  if (imgStyle.hasImageWidth()) {
                    displayWidth = static_cast<int>(
                        imgStyle.imageWidth.toPixels(emSize, static_cast<float>(containerWidth)) + 0.5f);
                  } else {
                    displayWidth = static_cast<int>(inheritWidthPx);
                  }
                  if (displayWidth > containerWidth) displayWidth = containerWidth;
                  if (displayWidth < 1) displayWidth = 1;
                  displayHeight =
                      static_cast<int>(displayWidth * (static_cast<float>(dims.height) / dims.width) + 0.5f);
                  if (displayHeight > self->viewportHeight) {
                    displayHeight = self->viewportHeight;
                    // Rescale width to preserve aspect ratio when height is clamped
                    displayWidth =
                        static_cast<int>(displayHeight * (static_cast<float>(dims.width) / dims.height) + 0.5f);
                    if (displayWidth < 1) displayWidth = 1;
                  }
                  if (displayHeight < 1) displayHeight = 1;
                  LOG_DBG("EHP", "Display size from CSS width: %dx%d", displayWidth, displayHeight);
                } else {
                  // Scale to fit container while maintaining aspect ratio.
                  // Non-float plates: leave ~2 body lines so captions / following
                  // prose can share the page instead of a full empty lower half.
                  int maxWidth = containerWidth;
                  int maxHeight = self->viewportHeight;
                  if (pendingFloat == CssFloat::None) {
                    const int keep = std::max(40, self->renderer.getFontAscenderSize(self->fontId) * 2 + 8);
                    if (maxHeight > keep + 100) maxHeight -= keep;
                  }
                  float scaleX = (dims.width > maxWidth) ? (float)maxWidth / dims.width : 1.0f;
                  float scaleY = (dims.height > maxHeight) ? (float)maxHeight / dims.height : 1.0f;
                  float scale = (scaleX < scaleY) ? scaleX : scaleY;
                  if (scale > 1.0f) scale = 1.0f;

                  displayWidth = (int)(dims.width * scale);
                  displayHeight = (int)(dims.height * scale);
                  LOG_DBG("EHP", "Display size: %dx%d (scale %.2f)", displayWidth, displayHeight, scale);
                }

                // Scene dividers (DCC image_rsrc8GP etc.): ultra-wide canvas with a thin
                // ink stripe and huge white padding. Sizing height from full aspect leaves
                // massive empty bands above/below the line. Cap non-float landscape rules
                // to ~1.5 body lines so they read as a small rule, not a grey slab.
                if (pendingFloat == CssFloat::None && dims.width > 0 && dims.height > 0 &&
                    dims.width >= dims.height * 2 && displayHeight > 0) {
                  const int bodyEm = std::max(8, self->renderer.getFontAscenderSize(self->fontId));
                  const int maxRuleH = std::max(8, bodyEm + bodyEm / 2);
                  if (displayHeight > maxRuleH) {
                    LOG_DBG("EHP", "Rule-like image height %d -> %d (src %dx%d w=%d)", displayHeight, maxRuleH,
                            dims.width, dims.height, displayWidth);
                    displayHeight = maxRuleH;
                  }
                }

                // Flush any pending text block so it appears before the image
                if (self->partWordBufferIndex > 0) {
                  self->flushPartWordBuffer();
                }
                if (self->currentTextBlock && !self->currentTextBlock->isEmpty()) {
                  const BlockStyle parentBlockStyle = self->currentTextBlock->getBlockStyle();
                  self->startNewTextBlock(parentBlockStyle);
                }

                // Apply vertical margins from the container to the image.
                // Top margin lives on the empty text block (deposited via vertical merge
                // in startNewTextBlock). Bottom margin was stripped by withoutBottom() for
                // deferred application at element close, so read it from the stack.
                int16_t imageMarginTop = 0;
                int16_t imageMarginBottom = 0;
                if (self->currentTextBlock && self->currentTextBlock->isEmpty()) {
                  const auto& bs = self->currentTextBlock->getBlockStyle();
                  imageMarginTop = bs.topInset();
                  if (self->blockStyleStack.size() > 1) {
                    imageMarginBottom = self->blockStyleStack.back().bottomInset();
                  }
                }

                // Create page for image - only break if image won't fit remaining space
                if (self->currentPage && !self->currentPage->elements.empty() &&
                    (self->currentPageNextY + imageMarginTop + displayHeight + imageMarginBottom >
                     self->viewportHeight)) {
                  self->completePageFn(std::move(self->currentPage), self->xpathParagraphIndex,
                                       self->xpathListItemIndex);
                  self->completedPageCount++;
                  self->currentPage.reset(new Page());
                  if (!self->currentPage) {
                    LOG_ERR("EHP", "Failed to create new page");
                    return;
                  }
                  self->currentPageNextY = 0;
                } else if (!self->currentPage) {
                  self->currentPage.reset(new Page());
                  if (!self->currentPage) {
                    LOG_ERR("EHP", "Failed to create initial page");
                    return;
                  }
                  self->currentPageNextY = 0;
                }

                // Apply top margin from container block
                self->currentPageNextY += imageMarginTop;

                // Create ImageBlock and add to page
                // nothrow: make_shared uses bare new, which aborts on OOM under
                // -fno-exceptions; images arrive mid-parse when the heap is at its
                // most loaded, so this must fail soft into the null-check below.
                // Eager-extract while layout still has heap. Always keep
                // resolvedPath as srcPath so first paint can re-extract if the
                // file is missing/empty (was the root of Alice empty boxes).
                if (!Storage.exists(cachedImagePath.c_str())) {
                  HalFile outFile;
                  if (Storage.openFileForWrite("EHP", cachedImagePath, outFile)) {
                    const bool ok = self->epub->readItemContentsToStream(resolvedPath, outFile, 4096);
                    outFile.flush();
                    outFile.close();
                    if (!ok) Storage.remove(cachedImagePath.c_str());
                  }
                }
                // Warm PNGdec while section-parse heap is still contiguous
                // (maxAlloc often ~80KB here; paint-time after fonts ~40KB fails).
                if (ext.size() >= 4) {
                  char e0 = ext[1], e1 = ext[2], e2 = ext[3];
                  if (e0 >= 'A' && e0 <= 'Z') e0 = static_cast<char>(e0 - 'A' + 'a');
                  if (e1 >= 'A' && e1 <= 'Z') e1 = static_cast<char>(e1 - 'A' + 'a');
                  if (e2 >= 'A' && e2 <= 'Z') e2 = static_cast<char>(e2 - 'A' + 'a');
                  if (e0 == 'p' && e1 == 'n' && e2 == 'g') {
                    (void)PngToFramebufferConverter::warmSharedDecoder();
                  }
                }
                auto imageBlock = std::shared_ptr<ImageBlock>(
                    new (std::nothrow) ImageBlock(cachedImagePath, resolvedPath, displayWidth, displayHeight));
                if (!imageBlock) {
                  LOG_ERR("EHP", "Failed to create ImageBlock");
                  return;
                }

                // Rivulet PR4: float:left/right on <img> or inherited from wrapper
                // (.figleft/.figright — Project Gutenberg / Alice pattern).
                // imageMarginTop already applied to currentPageNextY above.
                const CssFloat imgFloat = pendingFloat;
                if (imgFloat == CssFloat::Left || imgFloat == CssFloat::Right) {
                  // placeFloatImage may shrink drop-caps to a 2-line box, then
                  // tryPrecache at the final size. Do NOT precache at CSS size
                  // first — that double-decoded Alice letters (75→62) under a
                  // deep SAX stack and crashed free() after TextSettings reflow.
                  self->placeFloatImage(imageBlock, displayWidth, displayHeight, imgFloat, /*marginTop=*/0,
                                        imageMarginBottom);
                } else {
                  // Non-float: warm .pxc once at final display size while layout
                  // heap is still the best of the session.
                  (void)imageBlock->tryPrecache(self->renderer);
                  // Non-float: honor text-align / figcenter (default center for figures).
                  int xPos = 0;
                  CssTextAlign align = CssTextAlign::Center;
                  if (cssStyle.hasTextAlign()) {
                    align = cssStyle.textAlign;
                  } else if (self->currentTextBlock) {
                    align = self->currentTextBlock->getBlockStyle().alignment;
                  }
                  // Class fallback for figcenter when CSS text-align missed.
                  if (!classAttr.empty()) {
                    std::string cls = classAttr;
                    for (char& c : cls) {
                      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
                    }
                    if (cls.find("figcenter") != std::string::npos || cls.find("center") != std::string::npos) {
                      align = CssTextAlign::Center;
                    }
                  }
                  const int leftPad = self->currentTextBlock ? self->currentTextBlock->getBlockStyle().leftInset() : 0;
                  const int rightPad =
                      self->currentTextBlock ? self->currentTextBlock->getBlockStyle().rightInset() : 0;
                  const int contentW = std::max(1, static_cast<int>(self->viewportWidth) - leftPad - rightPad);
                  if (align == CssTextAlign::Right) {
                    xPos = leftPad + std::max(0, contentW - displayWidth);
                  } else if (align == CssTextAlign::Left) {
                    xPos = leftPad;
                  } else {
                    xPos = leftPad + std::max(0, (contentW - displayWidth) / 2);
                  }
                  auto pageImage = std::shared_ptr<PageImage>(
                      new (std::nothrow) PageImage(imageBlock, static_cast<int16_t>(xPos), self->currentPageNextY));
                  if (!pageImage) {
                    LOG_ERR("EHP", "Failed to create PageImage");
                    return;
                  }
                  self->currentPage->elements.push_back(pageImage);
                  self->currentPageNextY += displayHeight + imageMarginBottom;
                }

                // The image consumed the empty block's accumulated vertical spacing.
                // Reset the block so the Vertical merge in startNewTextBlock doesn't
                // re-apply the same margins to the next text paragraph.
                if (self->currentTextBlock && self->currentTextBlock->isEmpty()) {
                  BlockStyle resetStyle;
                  resetStyle.alignment = (self->paragraphAlignment == static_cast<uint8_t>(CssTextAlign::None))
                                             ? CssTextAlign::Justify
                                             : static_cast<CssTextAlign>(self->paragraphAlignment);
                  self->currentTextBlock->setBlockStyle(resetStyle);
                }

                self->depth += 1;
                return;
              } else {
                LOG_ERR("EHP", "Failed to get image dimensions");
                Storage.remove(cachedImagePath.c_str());
              }
            }
          }  // isFormatSupported
        }
      }

      // Fallback to alt text if image processing fails
      if (!alt.empty()) {
        alt = "[Image: " + alt + "]";
        self->startNewTextBlock(self->blockStyleStack.back()
                                    .getCombinedBlockStyle(centeredBlockStyle, BlockStyle::CombineAxis::Horizontal)
                                    .withoutBottom());
        self->italicUntilDepth = std::min(self->italicUntilDepth, self->depth);
        self->depth += 1;
        self->characterData(userData, alt.c_str(), alt.length());
        // Skip any child content (skip until parent as we pre-advanced depth above)
        self->skipUntilDepth = self->depth - 1;
        return;
      }

      // No alt text, skip
      self->skipUntilDepth = self->depth;
      self->depth += 1;
      return;
    }
  }

  if (matches(name, SKIP_TAGS, std::size(SKIP_TAGS))) {
    // start skip
    self->skipUntilDepth = self->depth;
    self->depth += 1;
    return;
  }

  // Skip blocks with role="doc-pagebreak" and epub:type="pagebreak"
  if (atts != nullptr) {
    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "role") == 0 && strcmp(atts[i + 1], "doc-pagebreak") == 0 ||
          strcmp(atts[i], "epub:type") == 0 && strcmp(atts[i + 1], "pagebreak") == 0) {
        self->skipUntilDepth = self->depth;
        self->depth += 1;
        return;
      }
    }
  }

  // Detect internal <a href="..."> links (footnotes, cross-references)
  // Note: <aside epub:type="footnote"> elements are rendered as normal content
  // without special handling. Links pointing to them are collected as footnotes.
  if (strcmp(name, "a") == 0) {
    const char* href = getAttribute(atts, "href");

    bool isInternalLink = isInternalEpubLink(href);

    // Special case: javascript:void(0) links with data attributes
    // Example: <a href="javascript:void(0)"
    // data-xyz="{&quot;name&quot;:&quot;OPS/ch2.xhtml&quot;,&quot;frag&quot;:&quot;id46&quot;}">
    if (href && strncmp(href, "javascript:", 11) == 0) {
      isInternalLink = false;
      // TODO: Parse data-* attributes to extract actual href
    }

    if (isInternalLink) {
      // Flush buffer before style change
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
        self->nextWordContinues = true;
      }
      self->insideFootnoteLink = true;
      self->footnoteLinkDepth = self->depth;
      strncpy(self->currentFootnote.href, href, sizeof(self->currentFootnote.href) - 1);
      self->currentFootnote.href[sizeof(self->currentFootnote.href) - 1] = '\0';
      self->currentFootnote.number[0] = '\0';
      self->currentFootnoteLinkTextLen = 0;

      // Apply underline style to visually indicate the link.
      StyleStackEntry entry;
      entry.depth = self->depth;
      entry.hasTextDecoration = true;
      entry.textDecoration = CssTextDecoration::Underline;
      applyDirectionToEntry(entry, cssStyle);
      self->inlineStyleStack.push_back(entry);
      self->updateEffectiveInlineStyle();

      // Skip CSS resolution — we already handled styling for this <a> tag
      self->depth += 1;
      return;
    }
  }

  const float emSize = static_cast<float>(self->renderer.getFontAscenderSize(self->fontId));
  auto userAlignmentBlockStyle = BlockStyle::fromCssStyle(
      cssStyle, emSize, static_cast<CssTextAlign>(self->paragraphAlignment), self->viewportWidth);
  applyRivuletBlockMetrics(userAlignmentBlockStyle, cssStyle, self->styleResolve_, self->renderer, 0);

  if (strcmp(name, "hr") == 0) {
    auto hrBlockStyle = BlockStyle::fromCssStyle(cssStyle, emSize, CssTextAlign::Left, self->viewportWidth);
    applyRivuletBlockMetrics(hrBlockStyle, cssStyle, self->styleResolve_, self->renderer, 0);
    if (!self->embeddedStyle) {
      hrBlockStyle.marginLeft = 0;
      hrBlockStyle.marginRight = 0;
      hrBlockStyle.marginTop = 0;
      hrBlockStyle.marginBottom = 0;
      hrBlockStyle.paddingLeft = 0;
      hrBlockStyle.paddingRight = 0;
      hrBlockStyle.paddingTop = 0;
      hrBlockStyle.paddingBottom = 0;
      hrBlockStyle.textIndentDefined = false;
      hrBlockStyle.textIndent = 0;
    }
    self->emitHorizontalRule(hrBlockStyle);
    self->depth += 1;
    return;
  }

  // Track .ct1 / drop-cap host open for first-letter simulation (God Emperor pattern).
  if (strcmp(name, "blockquote") == 0 || strcmp(name, "div") == 0) {
    self->openBlockquoteIsCt1_ = false;
    if (!classAttr.empty()) {
      // Token-ish match: ct1, dropcap, drop-cap, firstletter
      const char* c = classAttr.c_str();
      if (strstr(c, "ct1") || strstr(c, "dropcap") || strstr(c, "drop-cap") || strstr(c, "drop_cap") ||
          strstr(c, "firstletter") || strstr(c, "first-letter")) {
        self->openBlockquoteIsCt1_ = true;
      }
    }
  }

  // Body paragraph after an explicit drop-cap host (.ct1 / dropcap / …) gets a synthetic
  // first-letter float (no ::first-letter engine). Skip paragraphs *inside* the host
  // blockquote (epigraph); only the first <p> after it. Bare <h1> does not arm — DCC
  // chapters are "<h1>[ 70 ]</h1><p>Colored lights…" with no book drop-cap.
  // Do NOT set pendingDropCap_ here — startNewTextBlock may still flush the previous
  // paragraph (e.g. "— THE STOLEN JOURNALS") via makePages; arm the *next* text block only.
  if (strcmp(name, "p") == 0 && self->nextParagraphGetsDropCap_ && self->embeddedStyle && !self->openBlockquoteIsCt1_) {
    self->armDropCapOnNextTextBlock_ = true;
    self->nextParagraphGetsDropCap_ = false;
    LOG_DBG("RLC", "Drop-cap armed for next text block");
  }

  // CSS clear: push past active floats before this block
  if (self->embeddedStyle && cssStyle.hasClear() && cssStyle.clear != CssClear::None) {
    self->floatClearPast();
  }

  if (matches(name, HEADER_TAGS, std::size(HEADER_TAGS))) {
    self->currentCssStyle = cssStyle;
    // h1–h6 default to centered (e-reader convention). Explicit book CSS/inline
    // left/right/justify still wins so intentional author layout is preserved.
    auto headerBlockStyle = BlockStyle::fromCssStyle(cssStyle, emSize, CssTextAlign::Center, self->viewportWidth);
    int headingLevel = 0;
    if (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && name[2] == '\0') {
      headingLevel = name[1] - '0';
    }
    applyRivuletBlockMetrics(headerBlockStyle, cssStyle, self->styleResolve_, self->renderer, headingLevel);
    headerBlockStyle.textAlignDefined = true;
    if (cssStyle.hasTextAlign()) {
      headerBlockStyle.alignment = cssStyle.textAlign;
    } else {
      headerBlockStyle.alignment = CssTextAlign::Center;
    }
    const auto accumulated =
        self->blockStyleStack.back().getCombinedBlockStyle(headerBlockStyle, BlockStyle::CombineAxis::Horizontal);
    self->blockStyleStack.push_back(accumulated);
    self->startNewTextBlock(accumulated.withoutBottom());
    self->boldUntilDepth = std::min(self->boldUntilDepth, self->depth);
    self->updateEffectiveInlineStyle();
  } else if (matches(name, BLOCK_TAGS, std::size(BLOCK_TAGS))) {
    if (strcmp(name, "br") == 0) {
      if (self->partWordBufferIndex > 0) {
        // flush word preceding <br/> to currentTextBlock before calling startNewTextBlock
        self->flushPartWordBuffer();
      }
      // Tag the new block so startNewTextBlock can inject a full line-height gap if
      // the block remains empty (i.e. <br> is a section separator between paragraphs).
      // If the block gets text added before the next block opens it becomes non-empty,
      // goes through makePages() normally, and the flag has no effect (inline <br> case).
      // WH-aligned <br>: neutral line box. Keep container inset (e.g. .poem
      // margin-left 30%) so bare lines like "Fury said to" / "nothing" stay in
      // the poem column. Clear textIndent so the previous span's progressive
      // indent does not leak onto the next stanza line.
      BlockStyle brStyle;
      if (!self->blockStyleStack.empty()) {
        brStyle.marginLeft = self->blockStyleStack.back().marginLeft;
        brStyle.paddingLeft = self->blockStyleStack.back().paddingLeft;
        brStyle.alignment = self->blockStyleStack.back().alignment;
        brStyle.textAlignDefined = self->blockStyleStack.back().textAlignDefined;
      } else if (self->currentTextBlock) {
        const auto& cur = self->currentTextBlock->getBlockStyle();
        brStyle.marginLeft = cur.marginLeft;
        brStyle.paddingLeft = cur.paddingLeft;
        brStyle.alignment = cur.alignment;
        brStyle.textAlignDefined = cur.textAlignDefined;
      }
      brStyle.fromBrElement = true;
      brStyle.textIndent = 0;
      brStyle.textIndentDefined = true;
      self->startNewTextBlock(brStyle);
    } else {
      self->currentCssStyle = cssStyle;
      const auto accumulated = self->blockStyleStack.back().getCombinedBlockStyle(userAlignmentBlockStyle,
                                                                                  BlockStyle::CombineAxis::Horizontal);
      self->blockStyleStack.push_back(accumulated);
      self->startNewTextBlock(accumulated.withoutBottom());
      self->updateEffectiveInlineStyle();

      if (strcmp(name, "li") == 0) {
        self->currentTextBlock->addWord("\xe2\x80\xa2", EpdFontFamily::REGULAR);
        self->listItemBulletOnly = true;
      }
    }
  } else if (matches(name, UNDERLINE_TAGS, std::size(UNDERLINE_TAGS))) {
    // Flush buffer before style change so preceding text gets current style
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      self->nextWordContinues = true;
    }
    self->pushDecorationStyleEntry(CssTextDecoration::Underline, cssStyle);
  } else if (matches(name, LINETHROUGH_TAGS, std::size(LINETHROUGH_TAGS))) {
    // Flush buffer before style change so preceding text gets current style
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      self->nextWordContinues = true;
    }
    self->pushDecorationStyleEntry(CssTextDecoration::LineThrough, cssStyle);
  } else if (matches(name, BOLD_TAGS, std::size(BOLD_TAGS))) {
    // Flush buffer before style change so preceding text gets current style
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      self->nextWordContinues = true;
    }
    self->boldUntilDepth = std::min(self->boldUntilDepth, self->depth);
    // Push inline style entry for bold tag
    StyleStackEntry entry;
    entry.depth = self->depth;  // Track depth for matching pop
    entry.hasBold = true;
    entry.bold = true;
    if (cssStyle.hasFontStyle()) {
      entry.hasItalic = true;
      entry.italic = cssStyle.fontStyle == CssFontStyle::Italic;
    }
    applyTextDecorationToEntry(entry, cssStyle);
    applyDirectionToEntry(entry, cssStyle);
    self->inlineStyleStack.push_back(entry);
    self->updateEffectiveInlineStyle();
  } else if (matches(name, ITALIC_TAGS, std::size(ITALIC_TAGS))) {
    // Flush buffer before style change so preceding text gets current style
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      self->nextWordContinues = true;
    }
    self->italicUntilDepth = std::min(self->italicUntilDepth, self->depth);
    // Push inline style entry for italic tag
    StyleStackEntry entry;
    entry.depth = self->depth;  // Track depth for matching pop
    entry.hasItalic = true;
    entry.italic = true;
    if (cssStyle.hasFontWeight()) {
      entry.hasBold = true;
      entry.bold = cssStyle.fontWeight == CssFontWeight::Bold;
    }
    applyTextDecorationToEntry(entry, cssStyle);
    applyDirectionToEntry(entry, cssStyle);
    self->inlineStyleStack.push_back(entry);
    self->updateEffectiveInlineStyle();
  } else if (strcmp(name, "sup") == 0 || strcmp(name, "sub") == 0) {
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      self->nextWordContinues = true;
    }
    StyleStackEntry entry;
    entry.depth = self->depth;
    if (strcmp(name, "sup") == 0) {
      entry.hasSup = true;
      entry.sup = true;
    } else {
      entry.hasSub = true;
      entry.sub = true;
    }
    self->inlineStyleStack.push_back(entry);
    self->updateEffectiveInlineStyle();
  } else if (strcmp(name, "span") == 0 || !isHeaderOrBlock(name)) {
    // Alice mouse-tail: <span style="margin-left:Nem"><span class="taleN">…</span></span><br/>
    // Progressive indent shifts the whole line box (marginLeft), not textIndent
    // (textIndent only first visual line → wrap zigzag).
    //
    // .poem { margin-left:30% } + span up to ~10em overflows a ~500px column:
    // hard-capping totalLeft makes 6–10em lines identical → right-hand stack
    // (field: "he met…" through "must have" all in one column). Scale the
    // progressive *wave* into remaining width so relative steps stay visible.
    // IMPORTANT: Do NOT treat bare font-size on span/small as a block split.
    //
    // Also skip tiny/super-sub margins: DCC ordinals use
    //   <span style="margin-left:0.05em; vertical-align:super">th</span>
    // Treating that as progressive indent forced a new text block so "th" dropped
    // to the next line next to "25" (Butcher's Masquerade ch.64 note).
    if (self->embeddedStyle && cssStyle.hasMarginLeft() &&
        !(cssStyle.hasVerticalAlign() &&
          (cssStyle.verticalAlign == CssVerticalAlign::Super || cssStyle.verticalAlign == CssVerticalAlign::Sub))) {
      const float emSize = static_cast<float>(self->renderer.getFontAscenderSize(self->fontId));
      const float vw = static_cast<float>(self->viewportWidth);
      int spanLeft = static_cast<int>(std::lround(cssStyle.marginLeft.toPixels(emSize, vw)));
      if (spanLeft < 0) spanLeft = 0;
      // Alice wave steps are multi-em; ignore sub-0.5em nudges (0.05em ordinals).
      const int minProgressivePx = std::max(4, static_cast<int>(std::lround(emSize * 0.5f)));
      if (spanLeft >= minProgressivePx) {
        if (self->partWordBufferIndex > 0) {
          self->flushPartWordBuffer();
        }
        BlockStyle lineStyle = self->currentTextBlock
                                   ? self->currentTextBlock->getBlockStyle()
                                   : (self->blockStyleStack.empty() ? BlockStyle{} : self->blockStyleStack.back());
        if (self->currentTextBlock && !self->currentTextBlock->isEmpty()) {
          self->startNewTextBlock(lineStyle);
          lineStyle = self->currentTextBlock->getBlockStyle();
        }
        const int containerLeft =
            self->blockStyleStack.empty() ? 0 : static_cast<int>(self->blockStyleStack.back().marginLeft);
        const int rightPad =
            self->blockStyleStack.empty() ? 0 : static_cast<int>(self->blockStyleStack.back().marginRight);
        // Short phrases ("will prose-", "take no de-") need ~120px; leave a bit more.
        const int minText = std::max(120, static_cast<int>(vw * 0.22f));
        const int roomForWave = std::max(0, static_cast<int>(self->viewportWidth) - containerLeft - rightPad - minText);
        // Alice peak is ~10em; map every span step into [0, roomForWave] proportionally.
        const int designPeak = std::max(1, static_cast<int>(std::lround(emSize * 10.0f)));
        int wave = spanLeft;
        if (designPeak > roomForWave) {
          wave = static_cast<int>((static_cast<int64_t>(spanLeft) * roomForWave) / designPeak);
        } else if (spanLeft > roomForWave) {
          wave = roomForWave;
        }
        lineStyle.marginLeft = static_cast<int16_t>(containerLeft + wave);
        lineStyle.paddingLeft = 0;
        lineStyle.marginRight = static_cast<int16_t>(rightPad);
        // No textIndent — whole-box indent only (avoids wrap zigzag).
        lineStyle.textIndent = 0;
        lineStyle.textIndentDefined = true;
        lineStyle.alignment = CssTextAlign::Left;
        lineStyle.textAlignDefined = true;
        lineStyle.marginTop = 0;
        lineStyle.marginBottom = 0;
        if (cssStyle.hasFontSize()) {
          applyRivuletBlockMetrics(lineStyle, cssStyle, self->styleResolve_, self->renderer, /*headingLevel=*/0);
        }
        if (self->currentTextBlock) {
          self->currentTextBlock->setBlockStyle(lineStyle);
        } else {
          self->startNewTextBlock(lineStyle);
        }
      }
    } else if (self->embeddedStyle && cssStyle.hasFontSize() && self->currentTextBlock &&
               self->currentTextBlock->isEmpty()) {
      // Empty post-<br> line with only a size class (e.g. .taleN) — restyle in place.
      BlockStyle lineStyle = self->currentTextBlock->getBlockStyle();
      applyRivuletBlockMetrics(lineStyle, cssStyle, self->styleResolve_, self->renderer, /*headingLevel=*/0);
      self->currentTextBlock->setBlockStyle(lineStyle);
    }

    // Handle span and other inline elements for CSS styling
    if (cssStyle.hasFontWeight() || cssStyle.hasFontStyle() || cssStyle.hasTextDecoration() ||
        cssStyle.hasDirection() || cssStyle.hasVerticalAlign()) {
      // Flush buffer before style change so preceding text gets current style
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
        self->nextWordContinues = true;
      }
      StyleStackEntry entry;
      entry.depth = self->depth;  // Track depth for matching pop
      if (cssStyle.hasFontWeight()) {
        entry.hasBold = true;
        entry.bold = cssStyle.fontWeight == CssFontWeight::Bold;
      }
      if (cssStyle.hasFontStyle()) {
        entry.hasItalic = true;
        entry.italic = cssStyle.fontStyle == CssFontStyle::Italic;
      }
      applyTextDecorationToEntry(entry, cssStyle);
      applyDirectionToEntry(entry, cssStyle);
      if (cssStyle.hasVerticalAlign()) {
        if (cssStyle.verticalAlign == CssVerticalAlign::Super) {
          entry.hasSup = true;
          entry.sup = true;
        } else if (cssStyle.verticalAlign == CssVerticalAlign::Sub) {
          entry.hasSub = true;
          entry.sub = true;
        }
      }
      self->inlineStyleStack.push_back(entry);
      self->updateEffectiveInlineStyle();
    }
  }

  // Unprocessed tag, just increasing depth and continue forward
  self->depth += 1;
}

void XMLCALL ChapterHtmlSlimParser::characterData(void* userData, const XML_Char* s, const int len) {
  auto* self = static_cast<ChapterHtmlSlimParser*>(userData);

  // Skip content of nested table
  if (self->tableDepth > 1) {
    return;
  }

  // Middle of skip
  if (self->skipUntilDepth < self->depth) {
    return;
  }

  // Collect footnote link display text (for the number label)
  // Skip whitespace and brackets to normalize noterefs like "[1]" → "1"
  if (self->insideFootnoteLink) {
    int start = 0;
    int end = len - 1;

    // Example input and output texts:
    // "     [  12  ]   " => "12"
    // "   turn to 256  " => "turn to 256"

    // Ignore leading whitespaces and left square brackets
    while (start < len && (isWhitespace(s[start]) || (s[start] == '['))) {
      ++start;
    }

    // Ignore trailing whitespaces and right square brackets
    while (end >= start && (isWhitespace(s[end]) || (s[end] == ']'))) {
      --end;
    }

    // Extract footnote link text
    for (int i = start; (self->currentFootnoteLinkTextLen < sizeof(self->currentFootnote.number) - 1) && (i <= end);
         ++i) {
      self->currentFootnote.number[self->currentFootnoteLinkTextLen++] = s[i];
    }
    self->currentFootnote.number[self->currentFootnoteLinkTextLen] = '\0';
  }

  for (int i = 0; i < len; i++) {
    if (isWhitespace(s[i])) {
      // Currently looking at whitespace, if there's anything in the partWordBuffer, flush it
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
      }
      // Whitespace is a real word boundary — reset continuation state
      self->nextWordContinues = false;
      // Skip the whitespace char
      continue;
    }

    // Detect U+00A0 (non-breaking space, UTF-8: 0xC2 0xA0) or
    //        U+202F (narrow no-break space, UTF-8: 0xE2 0x80 0xAF).
    //
    // Both are rendered as a visible space but must never allow a line break around them.
    // We split the no-break space into its own word token and link the surrounding words
    // with continuation flags so the layout engine treats them as an indivisible group.
    //
    // Example: "200&#xA0;Quadratkilometer" or "200&#x202F;Quadratkilometer"
    //   Input bytes:  "200\xC2\xA0Quadratkilometer"  (or 0xE2 0x80 0xAF for U+202F)
    //   Tokens produced:
    //     [0] "200"               continues=false
    //     [1] " "                 continues=true   (attaches to "200", no gap)
    //     [2] "Quadratkilometer"  continues=true   (attaches to " ", no gap)
    //
    //   The continuation flags prevent the line-breaker from inserting a line break
    //   between "200" and "Quadratkilometer". However, "Quadratkilometer" is now a
    //   standalone word for hyphenation purposes, so Liang patterns can produce
    //   "200 Quadrat-" / "kilometer" instead of the unusable "200" / "Quadratkilometer".
    if (static_cast<uint8_t>(s[i]) == 0xC2 && i + 1 < len && static_cast<uint8_t>(s[i + 1]) == 0xA0) {
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
      }

      self->partWordBuffer[0] = ' ';
      self->partWordBuffer[1] = '\0';
      self->partWordBufferIndex = 1;
      self->nextWordContinues = true;  // Attach space to previous word (no break).
      self->flushPartWordBuffer();

      self->nextWordContinues = true;  // Next real word attaches to this space (no break).

      i++;  // Skip the second byte (0xA0)
      continue;
    }

    // U+202F (narrow no-break space) — identical logic to U+00A0 above.
    if (static_cast<uint8_t>(s[i]) == 0xE2 && i + 2 < len && static_cast<uint8_t>(s[i + 1]) == 0x80 &&
        static_cast<uint8_t>(s[i + 2]) == 0xAF) {
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
      }

      self->partWordBuffer[0] = ' ';
      self->partWordBuffer[1] = '\0';
      self->partWordBufferIndex = 1;
      self->nextWordContinues = true;
      self->flushPartWordBuffer();

      self->nextWordContinues = true;

      i += 2;  // Skip the remaining two bytes (0x80 0xAF)
      continue;
    }

    // Skip Zero Width No-Break Space / BOM (U+FEFF) = 0xEF 0xBB 0xBF
    const XML_Char FEFF_BYTE_1 = static_cast<XML_Char>(0xEF);
    const XML_Char FEFF_BYTE_2 = static_cast<XML_Char>(0xBB);
    const XML_Char FEFF_BYTE_3 = static_cast<XML_Char>(0xBF);

    if (s[i] == FEFF_BYTE_1) {
      // Check if the next two bytes complete the 3-byte sequence
      if ((i + 2 < len) && (s[i + 1] == FEFF_BYTE_2) && (s[i + 2] == FEFF_BYTE_3)) {
        // Sequence 0xEF 0xBB 0xBF found!
        i += 2;    // Skip the next two bytes
        continue;  // Move to the next iteration
      }
    }

    // If we're about to run out of space, then cut the word off and start a new one.
    // For CJK text (no spaces), this is the primary word-breaking mechanism.
    // We must avoid splitting multi-byte UTF-8 sequences across word boundaries,
    // otherwise the trailing bytes become orphaned continuation bytes that the
    // decoder can't interpret.
    if (self->partWordBufferIndex >= MAX_WORD_SIZE) {
      int safeLen = utf8SafeTruncateBuffer(self->partWordBuffer, self->partWordBufferIndex);

      if (safeLen < self->partWordBufferIndex && safeLen > 0) {
        // Incomplete UTF-8 sequence at the end — save it before flushing
        int overflow = self->partWordBufferIndex - safeLen;
        char saved[4];
        for (int j = 0; j < overflow; j++) {
          saved[j] = self->partWordBuffer[safeLen + j];
        }
        self->partWordBufferIndex = safeLen;
        self->flushPartWordBuffer();
        self->nextWordContinues = true;
        for (int j = 0; j < overflow; j++) {
          self->partWordBuffer[j] = saved[j];
        }
        self->partWordBufferIndex = overflow;
      } else {
        self->flushPartWordBuffer();
        self->nextWordContinues = true;
      }
    }

    self->partWordBuffer[self->partWordBufferIndex++] = s[i];
  }

  // Keep token growth bounded: CSS-heavy spans can fragment text into many tiny
  // words, so flush earlier when embedded CSS is active. We still keep the
  // "exclude last line" behavior to preserve paragraph flow across chunks.
  const size_t blockWordCount = self->currentTextBlock->size();
  const size_t softFlushThreshold =
      self->embeddedStyle ? TEXT_BLOCK_SOFT_FLUSH_WORDS_WITH_CSS : TEXT_BLOCK_SOFT_FLUSH_WORDS;
  if (blockWordCount > softFlushThreshold) {
    LOG_DBG("EHP", "Text block soft flush (%u words)", static_cast<unsigned>(blockWordCount));
    // Drop-cap must peel before the first soft-flush consumes the lead letter.
    self->emitDropCapIfPending();
    BlockStyle& bs = self->currentTextBlock->getBlockStyle();
    self->injectPageFloatIntoBlock(bs);
    if (bs.floatZoneCount > 0 || (self->pageFloatActive() && !self->pageFloatIsRight_)) {
      bs.textIndent = 0;
      bs.textIndentDefined = true;
    }
    const int horizontalInset = bs.totalHorizontalInset();
    int usable = static_cast<int>(self->viewportWidth) - horizontalInset;
    if (usable < 16) usable = 16;
    const int measureFontId = self->blockFontId(bs);
    const int lh = self->lineAdvancePx(bs);
    const int floatLineH = (bs.floatZoneCount > 0) ? lh : 0;
    self->currentTextBlock->layoutAndExtractLines(
        self->renderer, measureFontId, static_cast<uint16_t>(usable),
        [self](const std::shared_ptr<TextBlock>& textBlock) { self->addLineToPage(textBlock); }, false, 0,
        self->currentPageNextY, floatLineH);
  }
}

void XMLCALL ChapterHtmlSlimParser::defaultHandlerExpand(void* userData, const XML_Char* s, const int len) {
  // Check if this looks like an entity reference (&...;)
  if (len >= 3 && s[0] == '&' && s[len - 1] == ';') {
    const char* utf8Value = lookupHtmlEntity(s, static_cast<size_t>(len));
    if (utf8Value != nullptr) {
      // Known entity: expand to its UTF-8 value
      characterData(userData, utf8Value, strlen(utf8Value));
      return;
    }
    // Unknown entity: preserve original &...; sequence
    characterData(userData, s, len);
    return;
  }
  // Not an entity we recognize - skip it
}

void XMLCALL ChapterHtmlSlimParser::endElement(void* userData, const XML_Char* name) {
  auto* self = static_cast<ChapterHtmlSlimParser*>(userData);

  // Pop float-inherit / width-contain entries for the element being closed.
  // startElement records inherit.depth = depth *before* depth++, so when we close
  // that element the current depth is inherit.depth + 1 (children already closed).
  while (self->floatInheritCount_ > 0 && self->floatInherit_[self->floatInheritCount_ - 1].depth == self->depth - 1) {
    LOG_DBG("RLC", "Float inherit pop depth=%d", self->depth - 1);
    --self->floatInheritCount_;
  }
  while (self->widthContainCount_ > 0 && self->widthContain_[self->widthContainCount_ - 1].depth == self->depth - 1) {
    LOG_DBG("RLC", "Width contain pop depth=%d", self->depth - 1);
    --self->widthContainCount_;
  }

  // Rivulet drop-cap host: after </blockquote.ct1> / .dropcap / .first-letter host only.
  // (Not after bare </h1> — that falsely drop-capped every DCC chapter open.)
  if (self->embeddedStyle) {
    if ((strcmp(name, "blockquote") == 0 || strcmp(name, "div") == 0) && self->openBlockquoteIsCt1_) {
      self->nextParagraphGetsDropCap_ = true;
      self->openBlockquoteIsCt1_ = false;
    }
  }

  // Check if any style state will change after we decrement depth
  // If so, we MUST flush the partWordBuffer with the CURRENT style first
  // Note: depth hasn't been decremented yet, so we check against (depth - 1)
  const bool willPopStyleStack =
      !self->inlineStyleStack.empty() && self->inlineStyleStack.back().depth == self->depth - 1;
  const bool willClearBold = self->boldUntilDepth == self->depth - 1;
  const bool willClearItalic = self->italicUntilDepth == self->depth - 1;

  const bool styleWillChange = willPopStyleStack || willClearBold || willClearItalic;
  const bool headerOrBlockTag = isHeaderOrBlock(name);
  const bool tableStructuralTag = isTableStructuralTag(name);

  if (self->tableDepth > 1 && strcmp(name, "table") == 0) {
    // get rid of all text inside the nested table
    self->partWordBufferIndex = 0;
    self->tableDepth -= 1;
    LOG_DBG("EHP", "nested table detected, get rid of its content");
    return;
  }

  // Flush buffer with current style BEFORE any style changes
  if (self->partWordBufferIndex > 0) {
    // Flush if style will change OR if we're closing a block/structural element
    const bool isInlineTag = !headerOrBlockTag && !tableStructuralTag &&
                             !matches(name, IMAGE_TAGS, std::size(IMAGE_TAGS)) && self->depth != 1;
    const bool shouldFlush = styleWillChange || headerOrBlockTag || matches(name, BOLD_TAGS, std::size(BOLD_TAGS)) ||
                             matches(name, ITALIC_TAGS, std::size(ITALIC_TAGS)) ||
                             matches(name, UNDERLINE_TAGS, std::size(UNDERLINE_TAGS)) ||
                             matches(name, LINETHROUGH_TAGS, std::size(LINETHROUGH_TAGS)) || tableStructuralTag ||
                             matches(name, IMAGE_TAGS, std::size(IMAGE_TAGS)) || self->depth == 1;

    if (shouldFlush) {
      self->flushPartWordBuffer();
      // If closing an inline element, the next word fragment continues the same visual word
      if (isInlineTag) {
        self->nextWordContinues = true;
      }
    }
  }

  self->depth -= 1;

  // Closing a footnote link — create entry from collected text and href
  if (self->insideFootnoteLink && self->depth == self->footnoteLinkDepth) {
    if (self->currentFootnote.number[0] != '\0' && self->currentFootnote.href[0] != '\0') {
      FootnoteEntry entry;
      strncpy(entry.number, self->currentFootnote.number, sizeof(entry.number) - 1);
      entry.number[sizeof(entry.number) - 1] = '\0';
      strncpy(entry.href, self->currentFootnote.href, sizeof(entry.href) - 1);
      entry.href[sizeof(entry.href) - 1] = '\0';
      int wordIndex =
          self->wordsExtractedInBlock + (self->currentTextBlock ? static_cast<int>(self->currentTextBlock->size()) : 0);
      self->pendingFootnotes.push_back({wordIndex, entry});
    }
    self->insideFootnoteLink = false;
  }

  // Leaving skip
  if (self->skipUntilDepth == self->depth) {
    self->skipUntilDepth = INT_MAX;
  }

  if (self->tableDepth == 1 && (strcmp(name, "td") == 0 || strcmp(name, "th") == 0)) {
    self->nextWordContinues = false;
  }

  if (self->tableDepth == 1 && (strcmp(name, "tr") == 0)) {
    self->nextWordContinues = false;
  }

  if (self->tableDepth == 1 && strcmp(name, "table") == 0) {
    self->tableDepth -= 1;
    self->tableRowIndex = 0;
    self->tableColIndex = 0;
    self->nextWordContinues = false;
  }

  // Leaving bold tag
  if (self->boldUntilDepth == self->depth) {
    self->boldUntilDepth = INT_MAX;
  }

  // Leaving italic tag
  if (self->italicUntilDepth == self->depth) {
    self->italicUntilDepth = INT_MAX;
  }

  // Pop from inline style stack if we pushed an entry at this depth
  // This handles all inline elements: b, i, u, span, etc.
  if (!self->inlineStyleStack.empty() && self->inlineStyleStack.back().depth == self->depth) {
    self->inlineStyleStack.pop_back();
    self->updateEffectiveInlineStyle();
  }

  // Clear block style when leaving header or block elements
  if (headerOrBlockTag) {
    self->currentCssStyle.reset();
    self->updateEffectiveInlineStyle();

    // br is self-closing and not a container — it doesn't push/pop the stack.
    if (strcmp(name, "br") != 0 && self->blockStyleStack.size() > 1) {
      // Apply closing element's bottom margin to the current text block so
      // container spacing appears after the element's content (on the last child),
      // not on the first child via the empty-block merge in startNewTextBlock.
      if (self->currentTextBlock) {
        const auto style = self->currentTextBlock->getBlockStyle();
        self->currentTextBlock->setBlockStyle(style.addBottom(self->blockStyleStack.back()));
      }
      self->blockStyleStack.pop_back();
    }

    // </li> closes: if the bullet never got inline text (empty <li> or <li> with only
    // block children that were flushed), clear the flag so the next sibling doesn't
    // merge into this block.
    if (strcmp(name, "li") == 0) {
      self->listItemBulletOnly = false;
    }
  }
}

ChapterHtmlSlimParser::~ChapterHtmlSlimParser() { abortParse(); }

bool ChapterHtmlSlimParser::beginParse() {
  // Rivulet: precompute size-step font ladder once per section parse.
  initStyleResolveContext(styleResolve_, fontId, lineCompression, embeddedStyle, renderer);

  // Initialize block style stack with a root entry representing "no ancestor block elements".
  // The user's paragraph alignment is set as the default so child elements without explicit
  // text-align inherit it correctly through getCombinedBlockStyle.
  BlockStyle rootBlockStyle;
  rootBlockStyle.alignment = (this->paragraphAlignment == static_cast<uint8_t>(CssTextAlign::None))
                                 ? CssTextAlign::Justify
                                 : static_cast<CssTextAlign>(this->paragraphAlignment);
  rootBlockStyle.sizeStep = SIZE_STEP_BASE;
  rootBlockStyle.lineHeightPx = static_cast<int16_t>(renderer.getLineHeight(fontId, lineCompression));
  blockStyleStack.clear();
  blockStyleStack.reserve(8);
  blockStyleStack.push_back(rootBlockStyle);

  auto paragraphAlignmentBlockStyle = BlockStyle();
  paragraphAlignmentBlockStyle.textAlignDefined = true;
  const auto align = rootBlockStyle.alignment;
  paragraphAlignmentBlockStyle.alignment = align;
  paragraphAlignmentBlockStyle.sizeStep = SIZE_STEP_BASE;
  paragraphAlignmentBlockStyle.lineHeightPx = rootBlockStyle.lineHeightPx;
  startNewTextBlock(paragraphAlignmentBlockStyle);

  xmlParser_ = XML_ParserCreate(nullptr);
  if (!xmlParser_) {
    LOG_ERR("EHP", "Couldn't allocate memory for parser");
    return false;
  }

  // Handle HTML entities (like &nbsp;) that aren't in XML spec or DTD
  // Using DefaultHandlerExpand preserves normal entity expansion from DOCTYPE
  XML_SetDefaultHandlerExpand(xmlParser_, defaultHandlerExpand);

  if (!Storage.openFileForRead("EHP", filepath, parseFile_)) {
    destroyXmlParser(xmlParser_);
    xmlParser_ = nullptr;
    return false;
  }

  // Get file size to decide whether to show indexing popup.
  if (popupFn && parseFile_.size() >= MIN_SIZE_FOR_POPUP) {
    popupFn();
  }

  XML_SetUserData(xmlParser_, this);
  XML_SetElementHandler(xmlParser_, startElement, endElement);
  XML_SetCharacterDataHandler(xmlParser_, characterData);

  parseStartTime_ = millis();
  return true;
}

ChapterHtmlSlimParser::ParseStatus ChapterHtmlSlimParser::parseStep() {
  void* const buf = XML_GetBuffer(xmlParser_, PARSE_BUFFER_SIZE);
  if (!buf) {
    LOG_ERR("EHP", "Couldn't allocate memory for buffer");
    return ParseStatus::Error;
  }

  const size_t len = parseFile_.read(buf, PARSE_BUFFER_SIZE);

  if (len == 0 && parseFile_.available() > 0) {
    LOG_ERR("EHP", "File read error");
    return ParseStatus::Error;
  }

  const int done = parseFile_.available() == 0;

  if (XML_ParseBuffer(xmlParser_, static_cast<int>(len), done) == XML_STATUS_ERROR) {
    LOG_ERR("EHP", "Parse error at line %lu:\n%s", XML_GetCurrentLineNumber(xmlParser_),
            XML_ErrorString(XML_GetErrorCode(xmlParser_)));
    return ParseStatus::Error;
  }

  return done ? ParseStatus::Done : ParseStatus::More;
}

void ChapterHtmlSlimParser::abortParse() {
  if (xmlParser_) {
    destroyXmlParser(xmlParser_);
    xmlParser_ = nullptr;
  }
  // Only close the file if it was successfully opened in beginParse()
  if (parseFile_.isOpen()) {
    parseFile_.close();
  }
}

bool ChapterHtmlSlimParser::finishParse() {
  if (xmlParser_) {
    LOG_DBG("EHP", "Time to parse and build pages: %lu ms", millis() - parseStartTime_);
    destroyXmlParser(xmlParser_);
    xmlParser_ = nullptr;
  }
  parseFile_.close();

  // Process last page if there is still text
  if (currentTextBlock) {
    makePages();
    if (!pendingAnchorId.empty()) {
      anchorData.push_back({std::move(pendingAnchorId), static_cast<uint16_t>(completedPageCount)});
      pendingAnchorId.clear();
    }
    completePageFn(std::move(currentPage), xpathParagraphIndex, xpathListItemIndex);
    completedPageCount++;
    currentPage.reset();
    currentTextBlock.reset();
  }

  return true;
}

bool ChapterHtmlSlimParser::parseAndBuildPages() {
  if (!beginParse()) {
    return false;
  }
  for (;;) {
    const ParseStatus status = parseStep();
    if (status == ParseStatus::Error) {
      abortParse();
      return false;
    }
    if (status == ParseStatus::Done) {
      break;
    }
  }
  return finishParse();
}

int ChapterHtmlSlimParser::lineAdvancePx(const BlockStyle& style) const {
  if (style.lineHeightPx > 0) {
    return style.lineHeightPx;
  }
  return renderer.getLineHeight(blockFontId(style), lineCompression);
}

void ChapterHtmlSlimParser::clearPageFloat() {
  pageFloatTop_ = 0;
  pageFloatBottom_ = 0;
  pageFloatWidth_ = 0;
  pageFloatIsRight_ = false;
}

void ChapterHtmlSlimParser::setPageFloat(const int16_t top, const int16_t bottom, const int16_t width,
                                         const bool isRight) {
  pageFloatTop_ = top;
  pageFloatBottom_ = bottom;
  pageFloatWidth_ = width;
  pageFloatIsRight_ = isRight;
}

void ChapterHtmlSlimParser::injectPageFloatIntoBlock(BlockStyle& bs) const {
  if (!pageFloatActive() || bs.floatZoneCount >= BlockStyle::kMaxFloatZones) return;
  // Still beside the float on this page?
  if (currentPageNextY >= pageFloatBottom_) return;
  FloatZone& z = bs.floatZones[bs.floatZoneCount++];
  z.top = pageFloatTop_;
  z.bottom = pageFloatBottom_;
  z.width = pageFloatWidth_;
  z.isRight = pageFloatIsRight_;
}

int ChapterHtmlSlimParser::leftFloatShiftAtY(const int lineTop, const int lineHeight) const {
  if (!pageFloatActive() || pageFloatIsRight_) return 0;
  // Prefer mid-line sampling (lineHeight may be 1 when caller already passes mid-Y).
  const int y = lineTop + (lineHeight > 1 ? lineHeight / 2 : 0);
  if (y >= pageFloatTop_ && y < pageFloatBottom_) {
    return pageFloatWidth_;
  }
  return 0;
}

void ChapterHtmlSlimParser::floatClearPast() {
  if (pageFloatActive()) {
    currentPageNextY =
        static_cast<int16_t>(std::max(static_cast<int>(currentPageNextY), static_cast<int>(pageFloatBottom_)));
  }
  clearPageFloat();
}

void ChapterHtmlSlimParser::placeFloatImage(const std::shared_ptr<ImageBlock>& imageBlock, const int displayWidth,
                                            const int displayHeight, const CssFloat side, const int16_t marginTop,
                                            const int16_t marginBottom) {
  if (!currentPage) {
    currentPage.reset(new Page());
    currentPageNextY = 0;
  }
  // Float stays on one page: if it won't fit, finish the page first.
  if (!currentPage->elements.empty() &&
      (currentPageNextY + marginTop + displayHeight + marginBottom > viewportHeight)) {
    completePageFn(std::move(currentPage), xpathParagraphIndex, xpathListItemIndex);
    completedPageCount++;
    currentPage.reset(new Page());
    currentPageNextY = 0;
    clearPageFloat();
  }
  currentPageNextY = static_cast<int16_t>(currentPageNextY + marginTop);

  // Letter-sized left floats (figleft drop-caps in many Gutenberg/Calibre books):
  // wrap exactly TWO body lines, then full-width — not Alice-only; any narrow
  // left float (≤28% viewport / ≤120px) gets the same tradepub treatment.
  const bool looksLikeDropCap =
      (side == CssFloat::Left) && (displayWidth <= std::max(120, static_cast<int>(viewportWidth) * 28 / 100));
  const int bodyLine = std::max(1, renderer.getLineHeight(fontId, lineCompression));
  int drawW = displayWidth;
  int drawH = displayHeight;
  int zoneH = displayHeight;
  if (looksLikeDropCap) {
    constexpr int kDropWrapLines = 2;
    const int targetH = kDropWrapLines * bodyLine;
    if (drawH > targetH && drawH > 0) {
      // Keep letter inside the 2-line wrap box so body line 3 is not under ink.
      drawW = std::max(1, (drawW * targetH) / drawH);
      drawH = targetH;
    }
    zoneH = targetH;
  } else if (bodyLine > 0) {
    // Whole-line zone heights for figures so wrap doesn't leave a half-indented line.
    const int n = std::max(1, (displayHeight + bodyLine - 1) / bodyLine);
    zoneH = n * bodyLine;
  }
  const int floatPad = looksLikeDropCap ? std::max(4, bodyLine / 6) : 6;
  const int16_t floatW = static_cast<int16_t>(drawW + floatPad);

  int xPos = 0;
  if (side == CssFloat::Right) {
    xPos = static_cast<int>(viewportWidth) - drawW;
    if (xPos < 0) xPos = 0;
  } else if (pageFloatActive() && !pageFloatIsRight_) {
    xPos = pageFloatWidth_;  // stack beside an existing left float
  }

  const int16_t imgY = currentPageNextY;
  // ImageBlock still holds source dims; PageImage position uses drawn box size via
  // the block's display size — re-set if we scaled for drop-cap snug.
  if (looksLikeDropCap && imageBlock && (drawW != displayWidth || drawH != displayHeight)) {
    imageBlock->setDisplaySize(drawW, drawH);
  }
  auto pageImage =
      std::shared_ptr<PageImage>(new (std::nothrow) PageImage(imageBlock, static_cast<int16_t>(xPos), imgY));
  if (!pageImage) {
    LOG_ERR("EHP", "Failed to create float PageImage");
    return;
  }
  currentPage->elements.push_back(pageImage);

  // Bake .pxc while section-parse heap is still the best of the session so first
  // ink does not re-enter PNGdec under fragmented paint heap (Alice floats).
  if (imageBlock) {
    (void)imageBlock->tryPrecache(renderer);
  }

  // Zone for wrapping text: top-aligned with image. Height is line-snapped (and
  // 2 lines max for letter floats) so the third body line is never half-indented.
  // currentPageNextY is NOT advanced by image height — body lines start at the same Y.
  setPageFloat(imgY, static_cast<int16_t>(imgY + zoneH), floatW, side == CssFloat::Right);
  LOG_DBG("RLC", "Float image side=%d draw=%dx%d zoneH=%d pad=%d dropCap=%d", static_cast<int>(side), drawW, drawH,
          zoneH, floatPad, looksLikeDropCap ? 1 : 0);
  if (currentTextBlock) {
    injectPageFloatIntoBlock(currentTextBlock->getBlockStyle());
  }
  (void)marginBottom;
}

void ChapterHtmlSlimParser::placeEpubImageSrc(const std::string& src, const CssStyle& /*cssStyle*/,
                                              const CssFloat forceFloat) {
  if (src.empty() || imageRendering == 2) return;

  std::string resolvedPath = FsHelpers::normalisePath(FsHelpers::decodeUriEscapes(contentBase + src));
  if (!ImageDecoderFactory::isFormatSupported(resolvedPath)) {
    LOG_DBG("RLC", "Background/package image format unsupported: %s", resolvedPath.c_str());
    return;
  }

  std::string ext;
  const size_t extPos = resolvedPath.rfind('.');
  if (extPos != std::string::npos) {
    ext = resolvedPath.substr(extPos);
  }
  const std::string cachedImagePath = imageBasePath + std::to_string(imageCounter++) + ext;

  ImageDimensions dims = {0, 0};
  ImageDimsProbe headerProbe;
  epub->readItemContentsToStream(resolvedPath, headerProbe, 1024, /*allowEarlyStop=*/true);
  bool gotDimensions = headerProbe.getDimensions(dims);

  // Always extract the package image now so first paint never depends on lazy
  // extract during the tight page-render heap window (Alice rabbithole PNGs).
  if (popupFn && !imagePopupFired) {
    imagePopupFired = true;
    popupFn();
  }
  if (!Storage.exists(cachedImagePath.c_str())) {
    HalFile cachedImageFile;
    if (Storage.openFileForWrite("EHP", cachedImagePath, cachedImageFile)) {
      const bool ok = epub->readItemContentsToStream(resolvedPath, cachedImageFile, 4096);
      cachedImageFile.flush();
      cachedImageFile.close();
      if (!ok) {
        Storage.remove(cachedImagePath.c_str());
        LOG_ERR("RLC", "Failed to extract package image %s", resolvedPath.c_str());
        return;
      }
    } else {
      LOG_ERR("RLC", "Cannot write package image cache %s", cachedImagePath.c_str());
      return;
    }
  }
  if (ext.size() >= 4) {
    // Case-insensitive .png — warm decoder while heap is still open.
    const char* e = ext.c_str();
    if ((e[1] == 'p' || e[1] == 'P') && (e[2] == 'n' || e[2] == 'N') && (e[3] == 'g' || e[3] == 'G')) {
      (void)PngToFramebufferConverter::warmSharedDecoder();
    }
  }
  if (!gotDimensions) {
    ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(cachedImagePath);
    for (int attempt = 0; attempt < 3 && !gotDimensions; ++attempt) {
      if (attempt > 0) delay(50);
      gotDimensions = decoder && decoder->getDimensions(cachedImagePath, dims);
    }
  }
  if (!gotDimensions || dims.width <= 0 || dims.height <= 0) {
    LOG_DBG("RLC", "Could not size package image %s", resolvedPath.c_str());
    return;
  }
  (void)PngToFramebufferConverter::warmSharedDecoder();

  // Alice rabbithole et al.: CSS puts text in a narrow left column over a full
  // background. We approximate with a right float at ~48% width so body text
  // (blockquot2 margin-right:40%) can sit beside the art instead of after a
  // full-page blank.
  CssFloat placeSide = forceFloat;
  int maxWidth = viewportWidth;
  int maxHeight = viewportHeight;
  if (placeSide == CssFloat::None && dims.height >= dims.width) {
    // Tall decorative background → float right, leave room for falling text.
    placeSide = CssFloat::Right;
    maxWidth = std::max(64, static_cast<int>(viewportWidth) * 48 / 100);
    maxHeight = viewportHeight;  // may span multiple text lines via float remainingH
  }

  float scaleX = (dims.width > maxWidth) ? static_cast<float>(maxWidth) / dims.width : 1.0f;
  float scaleY = (dims.height > maxHeight) ? static_cast<float>(maxHeight) / dims.height : 1.0f;
  float scale = (scaleX < scaleY) ? scaleX : scaleY;
  if (scale > 1.0f) scale = 1.0f;
  int displayWidth = static_cast<int>(dims.width * scale);
  int displayHeight = static_cast<int>(dims.height * scale);
  if (displayWidth < 1) displayWidth = 1;
  if (displayHeight < 1) displayHeight = 1;

  if (partWordBufferIndex > 0) flushPartWordBuffer();
  if (currentTextBlock && !currentTextBlock->isEmpty()) {
    startNewTextBlock(currentTextBlock->getBlockStyle());
  }

  if (placeSide == CssFloat::None) {
    if (currentPage && !currentPage->elements.empty() && (currentPageNextY + displayHeight > viewportHeight)) {
      completePageFn(std::move(currentPage), xpathParagraphIndex, xpathListItemIndex);
      completedPageCount++;
      currentPage.reset(new Page());
      currentPageNextY = 0;
    } else if (!currentPage) {
      currentPage.reset(new Page());
      currentPageNextY = 0;
    }
  } else if (!currentPage) {
    currentPage.reset(new Page());
    currentPageNextY = 0;
  }
  if (!currentPage) return;

  // Keep resolvedPath so render can re-extract if the eager write is empty.
  auto imageBlock = std::shared_ptr<ImageBlock>(
      new (std::nothrow) ImageBlock(cachedImagePath, resolvedPath, displayWidth, displayHeight));
  if (!imageBlock) return;

  if (placeSide == CssFloat::Left || placeSide == CssFloat::Right) {
    placeFloatImage(imageBlock, displayWidth, displayHeight, placeSide, 0, 0);
  } else {
    const int xPos = (static_cast<int>(viewportWidth) - displayWidth) / 2;
    auto pageImage = std::shared_ptr<PageImage>(new (std::nothrow) PageImage(imageBlock, xPos, currentPageNextY));
    if (!pageImage) return;
    currentPage->elements.push_back(pageImage);
    currentPageNextY = static_cast<int16_t>(currentPageNextY + displayHeight);
  }
  LOG_DBG("RLC", "Placed package image %s as %dx%d side=%d", src.c_str(), displayWidth, displayHeight,
          static_cast<int>(placeSide));
}

void ChapterHtmlSlimParser::emitDropCapIfPending() {
  if (!pendingDropCap_ || !embeddedStyle || !currentTextBlock || currentTextBlock->isEmpty()) {
    pendingDropCap_ = false;
    return;
  }
  pendingDropCap_ = false;

  std::string letter = currentTextBlock->peelDropCapLetter();
  if (letter.empty()) {
    LOG_DBG("RLC", "Drop-cap armed but peel returned empty");
    return;
  }

  BlockStyle& bs = currentTextBlock->getBlockStyle();
  const int bodyFontId = blockFontId(bs);
  const int bodyLine = std::max(1, lineAdvancePx(bs));
  const int bodyEm = std::max(8, renderer.getFontAscenderSize(bodyFontId));
  // Book drop-cap geometry (user / tradepub feel):
  //   - top of letter flush with top of first body line
  //   - bottom of letter ≈ end of second body line (snug; only TWO indented wrap lines)
  // CSS 3em is approximate size; wrap zone is fixed at 2 lines.
  constexpr int kDropLines = 2;
  const int targetCapH = kDropLines * bodyLine;

  // Prefer +2 ladder steps first, then search every loaded face for a larger glyph
  // that actually measures this letter (skip digit-only clock faces, empty glyphs).
  const uint8_t capStep =
      static_cast<uint8_t>(std::min(static_cast<int>(SIZE_STEP_MAX), static_cast<int>(bs.sizeStep) + 2));
  int capFontId = resolveRelativeFontId(styleResolve_, capStep);
  int bestAsc = renderer.getFontAscenderSize(capFontId);
  int bestW = renderer.getTextAdvanceX(capFontId, letter.c_str(), EpdFontFamily::BOLD);
  if (bestW < 4) bestW = 0;

  static constexpr int kPreferredCaps[] = {
      326065580,    // SOURCESERIF4_18
      1231166843,   // SOURCESERIF4_16
      -2078415541,  // LEXENDDECA_18
      -940581834,   // LEXENDDECA_16
      -1077864260,  // SOURCESERIF4_14
  };
  const auto& fontMap = renderer.getFontMap();
  auto considerCap = [&](const int cand) {
    if (cand == 0 || cand == bodyFontId) return;
    if (fontMap.find(cand) == fontMap.end()) return;
    const int w = renderer.getTextAdvanceX(cand, letter.c_str(), EpdFontFamily::BOLD);
    if (w < 4) return;  // face lacks this glyph
    const int asc = renderer.getFontAscenderSize(cand);
    // Prefer taller faces; on ties prefer wider (bolder presence).
    if (asc > bestAsc || (asc == bestAsc && w > bestW)) {
      bestAsc = asc;
      bestW = w;
      capFontId = cand;
    }
  };
  for (const int cand : kPreferredCaps) considerCap(cand);
  // SD Bookerly / user faces often include larger sizes than the builtin ladder.
  for (const auto& entry : fontMap) considerCap(entry.first);

  const int capLineH = std::max(1, renderer.getLineHeight(capFontId, lineCompression));
  const int glyphH = std::max(capLineH, bestAsc > 0 ? bestAsc + 2 : capLineH);
  // Always 2× paint for drop-caps: a 1× face is far too small for a 2-line zone
  // (looked like a tiny T below the first line). DROP_CAP draw path top-aligns
  // the 2× glyph to the line-box top so no negative hang into chrome.
  constexpr bool use2x = true;
  const auto capStyleBits = static_cast<EpdFontFamily::Style>(static_cast<uint8_t>(EpdFontFamily::BOLD) |
                                                              static_cast<uint8_t>(EpdFontFamily::DROP_CAP));
  const int paintGlyphH = glyphH * 2;
  // Exactly two wrap lines beside the letter (third line full width).
  const int capH = targetCapH;
  const int measuredW = renderer.getTextAdvanceX(capFontId, letter.c_str(), capStyleBits);
  // Gap ≈ CSS margin-right 0.1em on first-letter.
  const int gap = std::max(3, bodyEm / 10);
  const int letterW = std::max(16, measuredW + gap);
  // drawText(DROP_CAP) places ink top at PageLine y. Body capitals sit a few px
  // below the line-box origin (ascender slack), so a small positive nudge keeps
  // the T from sitting above "he" / into the gap above the first line.
  dropCapYAdjust_ = static_cast<int16_t>(std::max(2, std::min(bodyEm / 5, bodyLine / 4)));
  LOG_DBG("RLC", "Drop-cap '%s' bodyFont=%d capFont=%d w=%d h=%d paintH=%d yAdj=%d x2=1 lines=%d", letter.c_str(),
          bodyFontId, capFontId, letterW, capH, paintGlyphH, static_cast<int>(dropCapYAdjust_), kDropLines);
  (void)glyphH;

  if (!currentPage) {
    currentPage.reset(new Page());
    currentPageNextY = 0;
  }
  // Drop-cap letter as a one-word TextBlock; sizeStep only helps when ladder works —
  // paint still uses resolveRelativeFontId(base, sizeStep). When we picked a foreign
  // larger fontId, store sizeStep base and measure/paint with explicit capFontId by
  // temporarily using that id as the layout font (Page still passes reader base at
  // paint — CRITICAL). TextBlock::render re-resolves sizeStep from reader baseFontId,
  // so a foreign larger face would paint small again!
  //
  // Fix: store sizeStep at MAX and ensure paint ladder maps MAX to a larger face.
  // For SD single-size, paint collapses too — so force capStyle to use a sizeStep
  // that paint can only honor if we also teach paint about drop-cap.
  //
  // Practical approach: put the drop-cap letter's sizeStep = SIZE_STEP_MAX and
  // ensure StyleResolve for paint finds larger builtins… paint only knows Bitter/SS
  // ladders from base id. If base is Bookerly, paint stays Bookerly.
  //
  // So for true larger paint under Bookerly we must either (a) change TextBlock paint
  // to allow an absolute font override, or (b) embed the letter as image, or
  // (c) use SUP inverted — not available.
  //
  // Absolute override: BlockStyle optional paintFontId (0 = use resolve). SECTION bump.

  BlockStyle capStyle = bs;
  capStyle.sizeStep = capStep;
  // Always paint with the chosen larger face (never re-resolve to body size).
  capStyle.paintFontIdOverride = capFontId != 0 ? capFontId : bodyFontId;
  // Tall enough line box for 2× glyph so nothing clips the PageLine bounds.
  capStyle.lineHeightPx = static_cast<int16_t>(std::max(capLineH, paintGlyphH));
  capStyle.alignment = CssTextAlign::Left;
  capStyle.textAlignDefined = true;
  capStyle.marginTop = 0;
  capStyle.marginBottom = 0;
  capStyle.paddingTop = 0;
  capStyle.paddingBottom = 0;
  capStyle.textIndent = 0;
  capStyle.textIndentDefined = true;
  capStyle.marginLeft = 0;
  capStyle.paddingLeft = 0;

  auto capText = std::make_unique<ParsedText>(false, false, false, false, capStyle);
  // BOLD | DROP_CAP (2×) when face is still short of 3em — paint + measure agree.
  capText->addWord(letter, capStyleBits);
  const int leftInset = bs.leftInset();
  std::shared_ptr<TextBlock> capLine;
  capText->layoutAndExtractLines(
      renderer, capFontId, static_cast<uint16_t>(std::max(letterW + 2, 16)),
      [&](const std::shared_ptr<TextBlock>& line) { capLine = line; }, true);
  if (!capLine) {
    LOG_DBG("RLC", "Drop-cap layout produced no line");
    return;
  }

  if (currentPageNextY + capH > viewportHeight && !currentPage->elements.empty()) {
    completePageFn(std::move(currentPage), xpathParagraphIndex, xpathListItemIndex);
    completedPageCount++;
    currentPage.reset(new Page());
    currentPageNextY = 0;
  }

  const int16_t xOff = static_cast<int16_t>(leftInset);
  // Y re-anchored to first body line in addLineToPage; hang via dropCapYAdjust_.
  deferredDropCapLine_ = std::make_shared<PageLine>(capLine, xOff, currentPageNextY);
  currentPage->elements.push_back(deferredDropCapLine_);

  // Zone: full drop-cap height so N body lines wrap beside the letter.
  const int16_t floatW = static_cast<int16_t>(letterW);
  setPageFloat(currentPageNextY, static_cast<int16_t>(currentPageNextY + capH), floatW, /*isRight=*/false);
  injectPageFloatIntoBlock(bs);
  // Kill first-line indent so body sits flush to the cap.
  bs.textIndent = 0;
  bs.textIndentDefined = true;
}

void ChapterHtmlSlimParser::addLineToPage(std::shared_ptr<TextBlock> line) {
  const int lineHeight = lineAdvancePx(line->getBlockStyle());

  if (!currentPage) {
    currentPage.reset(new Page());
    currentPageNextY = 0;
  }

  if (currentPageNextY + lineHeight > viewportHeight) {
    // Image/drop-cap stay on the finished page; next page has no zone.
    completePageFn(std::move(currentPage), xpathParagraphIndex, xpathListItemIndex);
    completedPageCount++;
    currentPage.reset(new Page());
    currentPageNextY = 0;
    clearPageFloat();
    deferredDropCapLine_.reset();
  }

  // Re-anchor drop-cap so TOP of letter lines up with TOP of first body line.
  // dropCapYAdjust_ is typically >= 0 (push down for 2× paint); never hang into chrome.
  if (deferredDropCapLine_) {
    int y = currentPageNextY + static_cast<int>(dropCapYAdjust_);
    if (y < 0) y = 0;
    deferredDropCapLine_->yPos = static_cast<int16_t>(y);
    deferredDropCapLine_.reset();
    dropCapYAdjust_ = 0;
  }

  // Track cumulative words to assign footnotes to the page containing their anchor
  wordsExtractedInBlock += line->wordCount();
  auto footnoteIt = pendingFootnotes.begin();
  while (footnoteIt != pendingFootnotes.end() && footnoteIt->first <= wordsExtractedInBlock) {
    currentPage->addFootnote(footnoteIt->second.number, footnoteIt->second.href);
    ++footnoteIt;
  }
  pendingFootnotes.erase(pendingFootnotes.begin(), footnoteIt);

  // Mid-line test matches ParsedText::widthForLine so paint shift agrees with measure.
  const int leftEx = leftFloatShiftAtY(currentPageNextY + lineHeight / 2, /*lineHeight=*/1);
  const int16_t xOffset = static_cast<int16_t>(line->getBlockStyle().leftInset() + leftEx);
  currentPage->elements.push_back(std::make_shared<PageLine>(line, xOffset, currentPageNextY));
  currentPageNextY = static_cast<int16_t>(currentPageNextY + lineHeight);

  // Past the float bottom — release the zone so later paragraphs are full width.
  if (pageFloatActive() && currentPageNextY >= pageFloatBottom_) {
    clearPageFloat();
  }
}

void ChapterHtmlSlimParser::makePages() {
  if (!currentTextBlock) {
    LOG_ERR("EHP", "!! No text block to make pages for !!");
    return;
  }

  if (!currentPage) {
    currentPage.reset(new Page());
    currentPageNextY = 0;
  }

  // Top spacing before drop-cap / body so letter and first line share the same Y.
  BlockStyle& blockStyle = currentTextBlock->getBlockStyle();
  const int lineHeight = lineAdvancePx(blockStyle);
  const int measureFontId = blockFontId(blockStyle);

  // Keep short attribution lines (e.g. "— THE STOLEN JOURNALS") on the same page
  // as the preceding epigraph. Our CSS parser skips :last-child, so .ct1 p:last-child
  // { text-align:right; margin-top:2% } never applies — detect attributions here.
  int marginTop = blockStyle.marginTop;
  int paddingTop = blockStyle.paddingTop;
  const bool shortAttribution = currentTextBlock->size() <= 8;
  bool looksLikeDashAttribution = false;
  if (shortAttribution) {
    // First word starts with em/en dash (U+2014/U+2013) or ASCII '-'.
    const std::string& w0 = currentTextBlock->firstWord();
    if (!w0.empty()) {
      const unsigned char c0 = static_cast<unsigned char>(w0[0]);
      if (c0 == '-' || c0 == 0xE2 /* UTF-8 lead for U+2013/2014 */) {
        looksLikeDashAttribution = true;
      }
    }
  }
  // Poem/mouse-tail stanzas are short (2–4 words) with large left inset.
  // Do NOT use the epigraph attribution path (Y reclaim → stacked lines).
  const bool poemStanzaLine = blockStyle.marginLeft > lineHeight * 2;
  if (shortAttribution && !poemStanzaLine && currentPage && !currentPage->elements.empty()) {
    const int need = lineHeight + 2;
    // Keep a small gap so "— THE STOLEN JOURNALS" is not glued under the epigraph.
    const int minAttrGap = std::max(4, lineHeight / 4);
    marginTop = minAttrGap;
    paddingTop = 0;
    // Reclaim trailing bottom margin only for true dash attributions.
    if (looksLikeDashAttribution && currentPageNextY + minAttrGap + need > viewportHeight) {
      const int bodyEm = std::max(8, renderer.getFontAscenderSize(measureFontId));
      const int maxReclaim = std::min(static_cast<int>(currentPageNextY), bodyEm * 2);
      const int overshoot = currentPageNextY + minAttrGap + need - viewportHeight;
      const int reclaim = std::min(maxReclaim, overshoot + 2);
      if (reclaim > 0 && currentPageNextY - reclaim + minAttrGap + need <= viewportHeight) {
        currentPageNextY = static_cast<int16_t>(currentPageNextY - reclaim);
        LOG_DBG("RLC", "Attribution keep-with-prev: reclaimed %dpx Y now=%d", reclaim,
                static_cast<int>(currentPageNextY));
      }
    }
  }
  // First block on a fresh page: CSS often puts large chapter margin-top that
  // wastes the top third while the rest of the page is fine. Cap air above the
  // first ink so God Emperor / chapter heads start sooner.
  if (currentPage && currentPage->elements.empty() && currentPageNextY <= 2) {
    const int topCap = std::max(lineHeight, (lineHeight * 3) / 2);
    if (marginTop > topCap) marginTop = topCap;
    if (paddingTop > topCap) paddingTop = topCap;
  }
  // Epigraph attribution lines like "— THE STOLEN JOURNALS" (leading dash).
  // Do NOT right-align every short block — that forced Alice "CHAPTER I" (2 words,
  // book CSS text-align:left) to the right edge.
  if (looksLikeDashAttribution) {
    blockStyle.alignment = CssTextAlign::Right;
    blockStyle.textAlignDefined = true;
    blockStyle.textIndent = 0;
    blockStyle.textIndentDefined = true;
  }
  // Indented epigraphs (.ct1 margin-left ~25%): justify with few words/line makes
  // huge word gaps (serial screenshot). Prefer left for readable epigraph body.
  if (blockStyle.leftInset() > static_cast<int>(viewportWidth) / 6 && blockStyle.alignment == CssTextAlign::Justify &&
      !looksLikeDashAttribution) {
    blockStyle.alignment = CssTextAlign::Left;
    blockStyle.textAlignDefined = true;
  }

  // Beside an active page float (Alice figleft): no top shove — top-align wrap text.
  if (pageFloatActive() && currentPageNextY < pageFloatBottom_) {
    marginTop = 0;
    paddingTop = 0;
  }

  if (marginTop > 0) {
    currentPageNextY = static_cast<int16_t>(currentPageNextY + marginTop);
  }
  if (paddingTop > 0) {
    currentPageNextY = static_cast<int16_t>(currentPageNextY + paddingTop);
  }

  // Expired float zone?
  if (pageFloatActive() && currentPageNextY >= pageFloatBottom_) {
    clearPageFloat();
  }

  // Text drop-cap (h1/.ct1 path): letter + FloatZone on this block.
  emitDropCapIfPending();

  // Fresh zone list for this paragraph (never accumulate across makePages calls).
  blockStyle.floatZoneCount = 0;
  injectPageFloatIntoBlock(blockStyle);

  // Flush against drop-cap / figleft (no CSS text-indent on wrap lines).
  if (blockStyle.floatZoneCount > 0) {
    blockStyle.textIndent = 0;
    blockStyle.textIndentDefined = true;
    // Narrow wrap columns + justify → overlapping/cramped words beside the letter.
    if (blockStyle.alignment == CssTextAlign::Justify) {
      blockStyle.alignment = CssTextAlign::Left;
      blockStyle.textAlignDefined = true;
    }
  }

  const int horizontalInset = blockStyle.totalHorizontalInset();
  const auto lineCb = [this](const std::shared_ptr<TextBlock>& textBlock) { addLineToPage(textBlock); };

  // Full content width: float narrowing is done inside layout via widthForLine.
  int usable = static_cast<int>(viewportWidth) - horizontalInset;
  if (usable < 16) usable = 16;

  // Only enable float-aware breaks when we have a sane line height and zones.
  const int floatLineH = (blockStyle.floatZoneCount > 0 && lineHeight > 0 && lineHeight < 200) ? lineHeight : 0;
  currentTextBlock->layoutAndExtractLines(renderer, measureFontId, static_cast<uint16_t>(usable), lineCb,
                                          /*includeLastLine=*/true, /*maxLines=*/0,
                                          /*blockStartY=*/currentPageNextY, /*lineHeight=*/floatLineH);

  // Fallback: transfer any remaining pending footnotes to current page.
  if (!pendingFootnotes.empty() && currentPage) {
    for (const auto& [idx, fn] : pendingFootnotes) {
      currentPage->addFootnote(fn.number, fn.href);
    }
    pendingFootnotes.clear();
  }

  if (blockStyle.marginBottom > 0) {
    currentPageNextY = static_cast<int16_t>(currentPageNextY + blockStyle.marginBottom);
  }
  if (blockStyle.paddingBottom > 0) {
    currentPageNextY = static_cast<int16_t>(currentPageNextY + blockStyle.paddingBottom);
  }

  if (extraParagraphSpacing) {
    currentPageNextY = static_cast<int16_t>(currentPageNextY + lineHeight / 2);
  }
}
