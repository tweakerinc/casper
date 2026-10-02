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
constexpr const char* kHyphenationPreviewText =
    "supercalifragilistic expialidocious antidisestablishmentarianism "
    "incomprehensibilities internationalization uncharacteristically "
    "counterrevolutionaries electroencephalograph multifunctional "
    "configurations extraordinary sophisticated";

// Two short paragraphs for the normal layout preview.
constexpr const char* kPreviewPara1 =
    "The quick brown fox jumps over the lazy dog. "
    "Pack my box with five dozen liquor jugs so every line can stretch.";
constexpr const char* kPreviewPara2 = "How vexingly quick daft zebras jump across the bright open field today.";

constexpr int kRuleThickness = 1;

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
  Hyphenator::setPreferredLanguage("en");

  if (SETTINGS.hyphenationEnabled != 0) {
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

void renderPreviewSampleText(const GfxRenderer& renderer, const PreviewLayout& layout, const PreviewPaint& paint) {
  if (!paint.hasSample || paint.fontId == 0) return;
  int y = paint.sampleTop;
  y = drawParagraphLines(renderer, layout.para1, paint.fontId, paint.textLeft, y, paint.lineH, paint.lineAdvance,
                         paint.bodyBottom);
  if (!layout.para2.empty()) {
    y += paint.paragraphGap;
    drawParagraphLines(renderer, layout.para2, paint.fontId, paint.textLeft, y, paint.lineH, paint.lineAdvance,
                       paint.bodyBottom);
  }
}

PreviewPaint renderPreview(const GfxRenderer& renderer, PreviewLayout& layout, const int top, const int height,
                           const char* familyName, const char* sizeName) {
  PreviewPaint paint;
  if (height <= 8) return paint;

  const int pageW = renderer.getScreenWidth();
  constexpr int kChromeFont = UI_12_FONT_ID;
  constexpr int kMetaFont = UI_10_FONT_ID;
  const int chromeLineH = renderer.getLineHeight(kChromeFont);
  const int labelBandH = chromeLineH + 10;
  const int headerChromeH = kRuleThickness + labelBandH + kRuleThickness;
  if (height < headerChromeH + 24) return paint;

  // --- Double-line header ---
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

  const int bodyTop = bottomRuleY + kRuleThickness;
  const int bodyBottom = top + height;
  if (bodyBottom - bodyTop < 8) return paint;

  const int margin = SETTINGS.screenMargin;
  const int textLeft = margin;
  const int textWidth = pageW - 2 * margin;
  if (textWidth <= 0) return paint;

  // Sample starts a few px under the rule (no "not in preview" band — AA/darkness show live).
  constexpr int kSamplePad = 4;
  const int sampleTop = bodyTop + kSamplePad;

  const int fontId = SETTINGS.getReaderFontId();
  if (fontId == 0) return paint;

  const int lineH = renderer.getTextHeight(fontId);
  if (lineH <= 0) return paint;

  const float compression = SETTINGS.getReaderLineCompression();
  const int lineAdvance = std::max(1, renderer.getLineHeight(fontId, compression));
  const int paragraphGap = SETTINGS.extraParagraphSpacing ? [&]() {
    const uint8_t h = SETTINGS.extraParagraphSpacingHeight;
    if (h == CrossPointSettings::SPACING_FULL) return lineAdvance;
    if (h == CrossPointSettings::SPACING_QUARTER) return std::max(1, lineAdvance / 4);
    return std::max(1, lineAdvance / 2);
  }()
                                                          : 0;

  const bool bionic = SETTINGS.focusReadingEnabled != 0;
  const bool guide = SETTINGS.guideReadingEnabled != 0;
  const PreviewKey key{.fontId = fontId,
                       .fontSize = SETTINGS.fontSize,
                       .screenMargin = SETTINGS.screenMargin,
                       .textWidth = textWidth,
                       .lineCompression = compression,
                       .alignment = SETTINGS.paragraphAlignment,
                       .extraParagraphSpacing = SETTINGS.extraParagraphSpacing != 0,
                       .extraParagraphSpacingHeight = SETTINGS.extraParagraphSpacingHeight,
                       .focusReading = bionic,
                       .guideReading = guide,
                       .hyphenation = SETTINGS.hyphenationEnabled != 0};
  if (key != layout.key) {
    if (auto* fcm = renderer.getFontCacheManager()) {
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

  paint.fontId = fontId;
  paint.textLeft = textLeft;
  paint.sampleTop = sampleTop;
  paint.bodyBottom = bodyBottom;
  paint.lineH = lineH;
  paint.lineAdvance = lineAdvance;
  paint.paragraphGap = paragraphGap;
  paint.hasSample = true;

  renderPreviewSampleText(renderer, layout, paint);
  return paint;
}

}  // namespace textsettings
