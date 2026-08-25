#include "MinimalTheme.h"

#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <Utf8.h>

#include <algorithm>
#include <string>
#include <vector>

#include "components/UITheme.h"
#include "fontIds.h"
#include "util/ReaderChromePolicy.h"

namespace {

// Match BaseTheme landscape stack: ALL CAPS letters only, 10 pt regular, left column.
constexpr int kLandscapeStackFontId = SOURCESERIF4_10_FONT_ID;

std::string stackedLettersOnly(const char* text) {
  std::string out;
  if (!text) return out;
  const unsigned char* p = reinterpret_cast<const unsigned char*>(text);
  uint32_t cp;
  while ((cp = utf8NextCodepoint(&p)) != 0) {
    if (utf8IsCombiningMark(cp)) continue;
    if (cp >= 'a' && cp <= 'z') cp = cp - ('a' - 'A');
    const bool keep = (cp >= 'A' && cp <= 'Z') || (cp >= '0' && cp <= '9');
    if (keep) utf8AppendCodepoint(cp, out);
  }
  return out;
}

int landscapeLabelYBias(const char* text) {
  if (!text) return 0;
  const std::string u = stackedLettersOnly(text);
  if (u == "UP" || u == "NEXT") return -10;
  if (u == "SELECT" || u == "LOOKUP" || u == "DONE") return -10;
  if (u == "DOWN") return -4;
  return 0;
}

void drawStackedVerticalLabel(const GfxRenderer& renderer, const int fontId, const int stripX, const int stripW,
                              const int areaY, const int areaH, const char* text, const int yBias = 0) {
  if (!text || !*text || areaH <= 0 || stripW <= 0) return;

  const std::string upper = stackedLettersOnly(text);
  if (upper.empty()) return;

  std::vector<std::string> letters;
  letters.reserve(8);
  int maxCw = 0;
  {
    const unsigned char* p = reinterpret_cast<const unsigned char*>(upper.c_str());
    uint32_t cp;
    while ((cp = utf8NextCodepoint(&p)) != 0) {
      std::string one;
      utf8AppendCodepoint(cp, one);
      maxCw = std::max(maxCw, renderer.getTextWidth(fontId, one.c_str(), EpdFontFamily::REGULAR));
      letters.push_back(std::move(one));
    }
  }
  const int nChars = static_cast<int>(letters.size());
  if (nChars <= 0) return;

  constexpr EpdFontFamily::Style kStyle = EpdFontFamily::REGULAR;
  const int glyphH = std::max(8, renderer.getTextHeight(fontId));
  int step = glyphH + 1;
  int totalH = nChars * step;
  if (totalH > areaH) {
    step = std::max(glyphH, areaH / nChars);
    totalH = nChars * step;
  }
  constexpr int kTopPad = 2;
  int y = areaY + kTopPad + yBias;
  if (y + totalH > areaY + areaH) {
    y = areaY + std::max(0, areaH - totalH);
  }
  if (y < areaY) y = areaY;

  const int colLeft = stripX + std::max(2, (stripW - maxCw) / 2);

  for (const std::string& one : letters) {
    const int baseline = y + glyphH;
    renderer.drawText(fontId, colLeft, baseline, one.c_str(), true, kStyle);
    y += step;
  }
}

}  // namespace

// Shared by Stats (Dashboard) + Bare: text-only footer labels in four equal
// columns — no rounded button chrome (matches Bare / mockup menus).
void MinimalTheme::drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                                   const char* btn4) const {
  if (gpio.hasTouch()) {
    return;
  }

  const GfxRenderer::Orientation orient = renderer.getOrientation();
  const bool landscapeCw = orient == GfxRenderer::LandscapeClockwise;
  const bool landscapeCcw = orient == GfxRenderer::LandscapeCounterClockwise;
  const bool landscape = landscapeCw || landscapeCcw;
  const bool inverted = orient == GfxRenderer::PortraitInverted;
  const int barH = BaseTheme::frontButtonHintReserve(renderer);

  const int pageW = renderer.getScreenWidth();
  const int pageH = renderer.getScreenHeight();
  const char* labels[] = {btn1, btn2, btn3, btn4};

  constexpr int buttonWidth = 80;
  constexpr int x4ButtonPositions[] = {58, 146, 254, 342};
  constexpr int x3ButtonPositions[] = {65, 157, 291, 383};
  const int* buttonPositions = gpio.deviceIsX3() ? x3ButtonPositions : x4ButtonPositions;

  if (landscape) {
    const int stripX = landscapeCcw ? (pageW - barH) : 0;
    renderer.fillRect(stripX, 0, barH, pageH, false);

    const int portraitSpan = gpio.deviceIsX3() ? 528 : 480;
    constexpr int kClusterNudgeUp = 8;

    for (int i = 0; i < 4; ++i) {
      if (labels[i] == nullptr || labels[i][0] == '\0') continue;
      const int portraitCenterX = buttonPositions[i] + buttonWidth / 2;
      const int scaled = (portraitCenterX * pageH + portraitSpan / 2) / portraitSpan;
      int yCenter = landscapeCcw ? (pageH - 1 - scaled) : scaled;
      yCenter -= kClusterNudgeUp;
      const int pillY = yCenter - buttonWidth / 2;
      drawStackedVerticalLabel(renderer, kLandscapeStackFontId, stripX, barH, pillY + 2, buttonWidth - 4, labels[i],
                               landscapeLabelYBias(labels[i]));
    }
    return;
  }

  const int edgePad = readerchrome::portraitFooterEdgePad(!gpio.deviceIsX3());
  const int barY = readerchrome::portraitFooterBarY(pageH, barH, edgePad, inverted);
  if (barH > 0) {
    // Wipe through the X4 edge pad so the gap below the labels stays empty.
    const int wipeY = inverted ? 0 : barY;
    const int wipeH = inverted ? (barY + barH) : (pageH - barY);
    renderer.fillRect(0, wipeY, pageW, wipeH, false);
  }

  constexpr int kFooterFontId = UI_10_FONT_ID;
  constexpr int kSlots = 4;
  const int slotW = pageW / kSlots;
  const int lineH = renderer.getLineHeight(kFooterFontId);
  const int textY = barY + (barH - lineH) / 2;

  for (int i = 0; i < kSlots; ++i) {
    if (labels[i] == nullptr || labels[i][0] == '\0') continue;
    const int col = inverted ? (kSlots - 1 - i) : i;
    const int maxLabelW = slotW - 8;
    const std::string label = renderer.truncatedText(kFooterFontId, labels[i], maxLabelW, EpdFontFamily::REGULAR);
    const int tw = renderer.getTextWidth(kFooterFontId, label.c_str(), EpdFontFamily::REGULAR);
    const int tx = col * slotW + (slotW - tw) / 2;
    renderer.drawText(kFooterFontId, tx, textY, label.c_str(), true, EpdFontFamily::REGULAR);
  }
}
