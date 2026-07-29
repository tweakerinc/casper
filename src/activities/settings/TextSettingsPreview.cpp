#include "TextSettingsPreview.h"

#include <EpdFontFamily.h>
#include <Epub/ParsedText.h>
#include <Epub/blocks/BlockStyle.h>
#include <Epub/blocks/TextBlock.h>
#include <Epub/hyphenation/Hyphenator.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>

#include "CrossPointSettings.h"
#include "fontIds.h"

namespace textsettings {

namespace {

// Dense long-word sample so mid-word hyphens appear clearly at full page width.
// (Short "quick brown fox" almost never hyphenates.)
constexpr const char* kHyphenationPreviewText =
    "supercalifragilistic expialidocious antidisestablishmentarianism "
    "incomprehensibilities internationalization uncharacteristically "
    "counterrevolutionaries electroencephalograph multifunctional "
    "configurations extraordinary sophisticated";

// Two short paragraphs for the normal layout preview.
// Drawn separately so Extra Paragraph Spacing is obvious:
//   ON  → no first-line indent + half-line blank between paras (reader behavior)
//   OFF → first-line indent (3× space) + no blank between paras
// Each para is long enough to wrap (alignment / justify still visible).
constexpr const char* kPreviewPara1 =
    "The quick brown fox jumps over the lazy dog. "
    "Pack my box with five dozen liquor jugs so every line can stretch.";
constexpr const char* kPreviewPara2 =
    "How vexingly quick daft zebras jump across the bright open field today.";

// Match Settings / tab chrome rule thickness.
constexpr int kRuleThickness = 3;

// Map the paragraph-alignment setting to the engine's CssTextAlign.
// BOOK_STYLE (CssTextAlign::None in the reader) follows EPUB CSS — typically
// justified body text. Preview approximates that as Justify so it is not blank.
CssTextAlign toCssAlign(uint8_t align) {
  if (align == CrossPointSettings::BOOK_STYLE) return CssTextAlign::Justify;
  return static_cast<CssTextAlign>(align);
}

void addWordsFromText(ParsedText& parsed, const char* text) {
  if (text == nullptr) return;
  std::string word;
  for (const char* p = text;; p++) {
    if (*p == ' ' || *p == '\0') {
      if (!word.empty()) {
        parsed.addWord(word, EpdFontFamily::REGULAR);
        word.clear();
      }
      if (*p == '\0') break;
    } else {
      word.push_back(*p);
    }
  }
}

// Lay one paragraph through the reader engine (indent / justify / hyphenation).
void layoutParagraph(std::vector<std::shared_ptr<TextBlock>>& outLines, const GfxRenderer& renderer, int fontId,
                     int textWidth, const char* text) {
  outLines.clear();
  BlockStyle style;
  style.alignment = toCssAlign(SETTINGS.paragraphAlignment);
  style.textAlignDefined = true;

  ParsedText parsed(SETTINGS.extraParagraphSpacing != 0, SETTINGS.hyphenationEnabled != 0,
                    SETTINGS.focusReadingEnabled != 0, SETTINGS.guideReadingEnabled != 0, style);
  addWordsFromText(parsed, text);
  parsed.layoutAndExtractLines(renderer, fontId, static_cast<uint16_t>(textWidth),
                               [&outLines](std::shared_ptr<TextBlock> line) { outLines.push_back(std::move(line)); });
}

void relayout(PreviewLayout& layout, const GfxRenderer& renderer, int fontId, int textWidth) {
  // Match reader: hyphenation patterns are language-selected. Preview sample is English.
  Hyphenator::setPreferredLanguage("en");

  if (SETTINGS.hyphenationEnabled != 0) {
    // One dense block is enough to show mid-word hyphens.
    layoutParagraph(layout.para1, renderer, fontId, textWidth, kHyphenationPreviewText);
    layout.para2.clear();
    return;
  }

  layoutParagraph(layout.para1, renderer, fontId, textWidth, kPreviewPara1);
  layoutParagraph(layout.para2, renderer, fontId, textWidth, kPreviewPara2);
}

int drawParagraphLines(const GfxRenderer& renderer, const std::vector<std::shared_ptr<TextBlock>>& lines, int fontId,
                       int textLeft, int y, int lineH, int lineAdvance, int bodyBottom) {
  for (const auto& line : lines) {
    if (y + lineH > bodyBottom) break;
    line->render(renderer, fontId, textLeft, y);
    y += lineAdvance;
  }
  return y;
}

}  // namespace

void renderPreview(const GfxRenderer& renderer, PreviewLayout& layout, const int top, const int height,
                   const char* familyName, const char* sizeName, const char* notInPreviewNote) {
  if (height <= 8) return;

  const int pageW = renderer.getScreenWidth();
  constexpr int kChromeFont = UI_12_FONT_ID;
  constexpr int kMetaFont = UI_10_FONT_ID;
  const int chromeLineH = renderer.getLineHeight(kChromeFont);
  // Label band between the two rules (vertically centered like Settings tabs).
  const int labelBandH = chromeLineH + 10;
  const int headerChromeH = kRuleThickness + labelBandH + kRuleThickness;
  if (height < headerChromeH + 24) return;

  // --- Double-line header (same weight as Settings / Manage Fonts tab chrome) ---
  const int topRuleY = top;
  renderer.drawLine(0, topRuleY, pageW - 1, topRuleY, kRuleThickness, true);

  const int labelBandTop = top + kRuleThickness;
  const int labelY = labelBandTop + (labelBandH - chromeLineH) / 2;

  const char* title = tr(STR_PREVIEW);
  char metaBuf[96];
  metaBuf[0] = '\0';
  {
    const char* fam = familyName && familyName[0] ? familyName : "";
    const char* sz = sizeName && sizeName[0] ? sizeName : "";
    if (fam[0] && sz[0]) {
      snprintf(metaBuf, sizeof(metaBuf), "%s %s", fam, sz);
    } else if (fam[0]) {
      snprintf(metaBuf, sizeof(metaBuf), "%s", fam);
    } else if (sz[0]) {
      snprintf(metaBuf, sizeof(metaBuf), "%s", sz);
    }
  }

  // "Preview" bold + font/size to its right, group centered in the band.
  const int titleW = renderer.getTextWidth(kChromeFont, title, EpdFontFamily::BOLD);
  constexpr int kTitleMetaGap = 12;
  int metaW = 0;
  std::string metaShown;
  if (metaBuf[0] != '\0') {
    const int maxMetaW = std::max(40, pageW - titleW - kTitleMetaGap - 24);
    metaShown = renderer.truncatedText(kMetaFont, metaBuf, maxMetaW, EpdFontFamily::REGULAR);
    metaW = renderer.getTextWidth(kMetaFont, metaShown.c_str(), EpdFontFamily::REGULAR);
  }
  const int groupW = titleW + (metaW > 0 ? kTitleMetaGap + metaW : 0);
  int x = (pageW - groupW) / 2;
  renderer.drawText(kChromeFont, x, labelY, title, true, EpdFontFamily::BOLD);
  if (metaW > 0) {
    const int metaY = labelY + (chromeLineH - renderer.getLineHeight(kMetaFont)) / 2;
    renderer.drawText(kMetaFont, x + titleW + kTitleMetaGap, metaY, metaShown.c_str(), true, EpdFontFamily::REGULAR);
  }

  const int bottomRuleY = labelBandTop + labelBandH;
  renderer.drawLine(0, bottomRuleY, pageW - 1, bottomRuleY, kRuleThickness, true);

  // --- Unboxed body: full page width with the reader's horizontal margins ---
  // Matches EpubReader: left/right = screenMargin (line length = page − 2×margin).
  const int bodyTop = bottomRuleY + kRuleThickness;
  const int bodyBottom = top + height;
  if (bodyBottom - bodyTop < 8) return;

  const int margin = SETTINGS.screenMargin;
  const int textLeft = margin;
  const int textWidth = pageW - 2 * margin;
  if (textWidth <= 0) return;

  // Always reserve the note band so the sample body does not jump when
  // Embedded Style / AA show "Not Shown In Preview" vs other style rows.
  const int noteH = renderer.getLineHeight(kMetaFont);
  constexpr int kNoteBandExtra = 6;
  int sampleTop = bodyTop + noteH + kNoteBandExtra;
  if (notInPreviewNote && notInPreviewNote[0] != '\0') {
    const int noteW = renderer.getTextWidth(kMetaFont, notInPreviewNote, EpdFontFamily::REGULAR);
    renderer.drawText(kMetaFont, (pageW - noteW) / 2, bodyTop + 2, notInPreviewNote, true, EpdFontFamily::REGULAR);
  }

  const int fontId = SETTINGS.getReaderFontId();
  if (fontId == 0) return;

  const int lineH = renderer.getTextHeight(fontId);
  if (lineH <= 0) return;

  const float compression = SETTINGS.getReaderLineCompression();
  const int lineAdvance = std::max(1, renderer.getLineHeight(fontId, compression));
  // Same as ChapterHtmlSlimParser: half a line when Extra Paragraph Spacing is ON.
  const int paragraphGap = SETTINGS.extraParagraphSpacing ? lineAdvance / 2 : 0;

  const bool bionic = SETTINGS.focusReadingEnabled != 0;
  const bool guide = SETTINGS.guideReadingEnabled != 0;
  const PreviewKey key{.fontId = fontId,
                       .fontSize = SETTINGS.fontSize,
                       .screenMargin = SETTINGS.screenMargin,
                       .textWidth = textWidth,
                       .lineCompression = compression,
                       .alignment = SETTINGS.paragraphAlignment,
                       .extraParagraphSpacing = SETTINGS.extraParagraphSpacing != 0,
                       .focusReading = bionic,
                       .guideReading = guide,
                       .hyphenation = SETTINGS.hyphenationEnabled != 0};
  if (key != layout.key) {
    if (auto* fcm = renderer.getFontCacheManager()) {
      // Prewarm glyphs from both sample paragraphs.
      std::string warm = kPreviewPara1;
      warm += ' ';
      warm += kPreviewPara2;
      if (SETTINGS.hyphenationEnabled != 0) {
        warm = kHyphenationPreviewText;
      }
      fcm->prewarmCache(fontId, warm.c_str(), bionic ? 0x03 : 0x01);
    }
    relayout(layout, renderer, fontId, textWidth);
    layout.key = key;
  }

  // Paragraph 1, optional half-line gap (ON), paragraph 2.
  // OFF: each para's first line is indented by the engine (3× space width).
  // ON:  flush left + blank gap — matches reader, both traits should be obvious.
  int y = sampleTop;
  y = drawParagraphLines(renderer, layout.para1, fontId, textLeft, y, lineH, lineAdvance, bodyBottom);
  if (!layout.para2.empty()) {
    y += paragraphGap;
    drawParagraphLines(renderer, layout.para2, fontId, textLeft, y, lineH, lineAdvance, bodyBottom);
  }
}

}  // namespace textsettings
