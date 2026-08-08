#pragma once

#include <HalStorage.h>
#include <expat.h>

#include <climits>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Epub/FootnoteEntry.h"
#include "Epub/ParsedText.h"
#include "Epub/blocks/ImageBlock.h"
#include "Epub/blocks/TextBlock.h"
#include "Epub/css/CssParser.h"
#include "Epub/css/CssStyle.h"
#include "Epub/css/StyleResolve.h"

class Page;
class PageLine;
class GfxRenderer;
class Epub;

#define MAX_WORD_SIZE 200

class ChapterHtmlSlimParser {
  std::shared_ptr<Epub> epub;
  const std::string& filepath;
  GfxRenderer& renderer;
  std::function<void(std::unique_ptr<Page>, uint16_t, uint16_t)> completePageFn;
  std::function<void()> popupFn;  // Popup callback
  bool imagePopupFired = false;   // popupFn fired for the first image probe (single-shot)
  int depth = 0;
  int skipUntilDepth = INT_MAX;
  int boldUntilDepth = INT_MAX;
  int italicUntilDepth = INT_MAX;
  // buffer for building up words from characters, will auto break if longer than this
  // leave one char at end for null pointer
  char partWordBuffer[MAX_WORD_SIZE + 1] = {};
  int partWordBufferIndex = 0;
  bool nextWordContinues = false;  // true when next flushed word attaches to previous (inline element boundary)
  std::unique_ptr<ParsedText> currentTextBlock = nullptr;
  std::unique_ptr<Page> currentPage = nullptr;
  int16_t currentPageNextY = 0;
  int fontId;
  float lineCompression;
  bool extraParagraphSpacing;
  uint8_t paragraphAlignment;
  uint16_t viewportWidth;
  uint16_t viewportHeight;
  bool hyphenationEnabled;
  bool focusReadingEnabled;
  bool guideReadingEnabled;
  const CssParser* cssParser;
  bool embeddedStyle;
  uint8_t imageRendering;
  std::string contentBase;
  std::string imageBasePath;
  int imageCounter = 0;

  // Style tracking (replaces depth-based approach)
  struct StyleStackEntry {
    int depth = 0;
    bool hasBold = false, bold = false;
    bool hasItalic = false, italic = false;
    bool hasTextDecoration = false;
    CssTextDecoration textDecoration = CssTextDecoration::None;
    bool hasDirection = false;
    CssTextDirection direction = CssTextDirection::Ltr;
    bool hasSup = false, sup = false;
    bool hasSub = false, sub = false;
  };
  std::vector<StyleStackEntry> inlineStyleStack;
  std::vector<BlockStyle> blockStyleStack;  // accumulated block styles from open ancestor elements
  CssStyle currentCssStyle;
  bool effectiveBold = false;
  bool effectiveItalic = false;
  CssTextDecoration effectiveTextDecoration = CssTextDecoration::None;
  bool effectiveDirectionDefined = false;
  CssTextDirection effectiveDirection = CssTextDirection::Ltr;
  bool effectiveSup = false;
  bool effectiveSub = false;
  int tableDepth = 0;
  int tableRowIndex = 0;
  int tableColIndex = 0;
  bool listItemBulletOnly = false;  // true when currentTextBlock has only the <li> bullet

  // Rivulet: relative size ladder + line-height resolution (filled in startParse).
  StyleResolveContext styleResolve_;

  // Page-local active float (image or drop-cap). Cleared past zone bottom or on page emit.
  // Zones are also copied onto BlockStyle for the wrapping paragraph's layout pass.
  int16_t pageFloatTop_ = 0;
  int16_t pageFloatBottom_ = 0;  // 0 = inactive
  int16_t pageFloatWidth_ = 0;
  bool pageFloatIsRight_ = false;

  // Gutenberg/Alice pattern: float lives on the wrapper (.figleft/.figright), not on <img>.
  // Stack open containers so the next image inherits float side + optional CSS width.
  struct FloatInherit {
    int depth = 0;
    CssFloat side = CssFloat::None;
    int16_t cssWidthPx = 0;  // 0 = unspecified
  };
  static constexpr int kMaxFloatInherit = 4;
  FloatInherit floatInherit_[kMaxFloatInherit] = {};
  int floatInheritCount_ = 0;

  // Nested block CSS width (div width:11.2em > div width:28.1% > img width:100%).
  // Without this, img width:100% resolves against the full viewport and DCC scene
  // dividers (1920×853 JPEG with a ~40px ink stripe) become full-page thick bars.
  struct WidthContain {
    int depth = 0;
    int16_t cssWidthPx = 0;
  };
  static constexpr int kMaxWidthContain = 6;
  WidthContain widthContain_[kMaxWidthContain] = {};
  int widthContainCount_ = 0;

  // Drop-cap: next <p> after an explicit host (.ct1 / dropcap / first-letter classes —
  // God Emperor epigraph pattern). Do NOT arm after bare visible <h1>: tradepub chapter
  // markers like DCC "[ 70 ]" are real h1s but the body is not drop-capped in the book.
  // armDropCapOnNextTextBlock_ is set on <p> open; pendingDropCap_ is set only when the
  // new ParsedText for that <p> is created — never while makePages still runs on the
  // previous paragraph (which was peeling "—" from THE STOLEN JOURNALS by mistake).
  bool nextParagraphGetsDropCap_ = false;
  bool armDropCapOnNextTextBlock_ = false;
  bool pendingDropCap_ = false;
  bool openBlockquoteIsCt1_ = false;
  // Drop-cap PageLine may need Y re-anchor to first body line (top-align).
  std::shared_ptr<PageLine> deferredDropCapLine_;
  int16_t dropCapYAdjust_ = 0;

  // Anchor-to-page mapping: tracks which page each HTML id attribute lands on
  int completedPageCount = 0;
  std::vector<std::pair<std::string, uint16_t>> anchorData;
  std::string pendingAnchorId;          // deferred until after previous text block is flushed
  std::vector<std::string> tocAnchors;  // the list of anchors that are TOC chapter boundaries
  uint16_t xpathParagraphIndex = 0;
  uint16_t xpathListItemIndex = 0;

  // Footnote link tracking
  bool insideFootnoteLink = false;
  int footnoteLinkDepth = -1;
  FootnoteEntry currentFootnote = {};
  int currentFootnoteLinkTextLen = 0;
  std::vector<std::pair<int, FootnoteEntry>> pendingFootnotes;  // <wordIndex, entry>
  int wordsExtractedInBlock = 0;

  // Resumable parse state. The one-shot parseAndBuildPages() drives these
  // internally; the incremental section builder drives them across render ticks
  // so a large single chapter can yield between pages instead of blocking the UI
  // until the whole thing is laid out. parseFile_ and the expat parser stay alive
  // for the lifetime of the parse so it can be paused and resumed at buffer
  // boundaries.
  XML_Parser xmlParser_ = nullptr;
  HalFile parseFile_;
  uint32_t parseStartTime_ = 0;

  void updateEffectiveInlineStyle();
  void startNewTextBlock(const BlockStyle& blockStyle);
  void flushPendingAnchor();
  void flushPartWordBuffer();
  void makePages();
  static EpdFontFamily::Style fontStyleForTextDecoration(CssTextDecoration decoration);
  static void applyDirectionToEntry(StyleStackEntry& entry, const CssStyle& css);
  static void applyTextDecorationToEntry(StyleStackEntry& entry, const CssStyle& css);
  void pushDecorationStyleEntry(CssTextDecoration defaultDecoration, const CssStyle& cssStyle);
  void emitHorizontalRule(const BlockStyle& blockStyle);
  // Resolve block font for measure/advance from BlockStyle.sizeStep.
  [[nodiscard]] int blockFontId(const BlockStyle& style) const {
    return resolveRelativeFontId(styleResolve_, style.sizeStep);
  }
  [[nodiscard]] int lineAdvancePx(const BlockStyle& style) const;
  // Float-zone helpers (clean-room: page zone + BlockStyle.floatZones + widthForLine)
  [[nodiscard]] bool pageFloatActive() const { return pageFloatBottom_ > 0; }
  void clearPageFloat();
  void setPageFloat(int16_t top, int16_t bottom, int16_t width, bool isRight);
  void injectPageFloatIntoBlock(BlockStyle& bs) const;
  [[nodiscard]] int leftFloatShiftAtY(int lineTop, int lineHeight) const;
  void floatClearPast();  // advance Y past active float bottom (CSS clear)
  void emitDropCapIfPending();
  void placeFloatImage(const std::shared_ptr<ImageBlock>& imageBlock, int displayWidth, int displayHeight,
                       CssFloat side, int16_t marginTop, int16_t marginBottom);
  // Place an EPUB package image by relative src (e.g. CSS background-image URL).
  void placeEpubImageSrc(const std::string& src, const CssStyle& cssStyle, CssFloat forceFloat);
  // XML callbacks
  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char** atts);
  static void XMLCALL characterData(void* userData, const XML_Char* s, int len);
  static void XMLCALL defaultHandlerExpand(void* userData, const XML_Char* s, int len);
  static void XMLCALL endElement(void* userData, const XML_Char* name);

 public:
  explicit ChapterHtmlSlimParser(std::shared_ptr<Epub> epub, const std::string& filepath, GfxRenderer& renderer,
                                 const int fontId, const float lineCompression, const bool extraParagraphSpacing,
                                 const uint8_t paragraphAlignment, const uint16_t viewportWidth,
                                 const uint16_t viewportHeight, const bool hyphenationEnabled,
                                 const bool focusReadingEnabled, const bool guideReadingEnabled,
                                 const std::function<void(std::unique_ptr<Page>, uint16_t, uint16_t)>& completePageFn,
                                 const bool embeddedStyle, const std::string& contentBase,
                                 const std::string& imageBasePath, const uint8_t imageRendering = 0,
                                 std::vector<std::string> tocAnchors = {},
                                 const std::function<void()>& popupFn = nullptr, const CssParser* cssParser = nullptr)

      : epub(epub),
        filepath(filepath),
        renderer(renderer),
        fontId(fontId),
        lineCompression(lineCompression),
        extraParagraphSpacing(extraParagraphSpacing),
        paragraphAlignment(paragraphAlignment),
        viewportWidth(viewportWidth),
        viewportHeight(viewportHeight),
        hyphenationEnabled(hyphenationEnabled),
        focusReadingEnabled(focusReadingEnabled),
        guideReadingEnabled(guideReadingEnabled),
        completePageFn(completePageFn),
        popupFn(popupFn),
        cssParser(cssParser),
        embeddedStyle(embeddedStyle),
        imageRendering(imageRendering),
        contentBase(contentBase),
        imageBasePath(imageBasePath),
        tocAnchors(std::move(tocAnchors)) {}

  ~ChapterHtmlSlimParser();

  // One-shot parse: builds every page before returning (begin + step* + finish).
  bool parseAndBuildPages();

  // Resumable parse, for the incremental section builder. Drive as:
  //   if (!beginParse()) fail;
  //   loop: switch (parseStep()) { More: keep going / yield; Done: finishParse(); Error: abortParse(); }
  // Pages are emitted via completePageFn as they complete during parseStep(), so
  // the caller can stop once enough pages are built and resume on a later tick.
  enum class ParseStatus { More, Done, Error };
  bool beginParse();
  ParseStatus parseStep();
  bool finishParse();  // flush the trailing page and tear down; returns true
  void abortParse();   // tear down without flushing (error / abandon)

  void addLineToPage(std::shared_ptr<TextBlock> line);
  const std::vector<std::pair<std::string, uint16_t>>& getAnchors() const { return anchorData; }

  // Byte progress of the in-flight parse, used to estimate a still-building section's total page
  // count (a giant single-spine book never fully lays out, so its real count is unknown). Valid
  // between beginParse() and finishParse()/abortParse().
  size_t parseBytesConsumed() { return parseFile_ ? parseFile_.position() : 0; }
  size_t parseTotalBytes() { return parseFile_ ? parseFile_.size() : 0; }
};
