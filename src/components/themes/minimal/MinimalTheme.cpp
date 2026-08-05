#include "MinimalTheme.h"

#include <GfxRenderer.h>
#include <HalGPIO.h>

#include <string>

#include "components/UITheme.h"
#include "fontIds.h"

// Shared by Stats (Dashboard) + Bare: text-only footer labels in four equal
// columns — no rounded button chrome (matches Bare / mockup menus).
void MinimalTheme::drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                                   const char* btn4) const {
  if (gpio.hasTouch()) {
    return;
  }

  // Same rule as BaseTheme: keep Portrait 180 labels readable with the page.
  const GfxRenderer::Orientation origOrientation = renderer.getOrientation();
  const bool landscape = origOrientation == GfxRenderer::LandscapeClockwise ||
                         origOrientation == GfxRenderer::LandscapeCounterClockwise;
  const bool inverted = origOrientation == GfxRenderer::PortraitInverted;
  if (landscape) {
    renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageW = renderer.getScreenWidth();
  const int pageH = renderer.getScreenHeight();
  const int barH = metrics.buttonHintsHeight;
  const char* labels[] = {btn1, btn2, btn3, btn4};
  const int barY = inverted ? 0 : (pageH - barH);

  // Erase the full footer/header strip first. Labels change between screens (and when a
  // popup overlays mapLabels on top of function names) without clearScreen —
  // drawing new text alone leaves the previous glyphs stacked (worst on slots
  // 3–4 when Left/Right becomes Up/Down or vice versa).
  if (barH > 0) {
    renderer.fillRect(0, barY, pageW, barH, false);  // false = white on e-ink
  }

  // One step below headers (UI_12 = Source Serif 14): list-size Source Serif 12
  // so long hints like "Download" still fit a quarter-width slot.
  constexpr int kFooterFontId = UI_10_FONT_ID;
  constexpr int kSlots = 4;
  const int slotW = pageW / kSlots;
  const int lineH = renderer.getLineHeight(kFooterFontId);
  const int textY = barY + (barH - lineH) / 2;

  for (int i = 0; i < kSlots; ++i) {
    if (labels[i] == nullptr || labels[i][0] == '\0') continue;
    // Keep label i on physical key i: mirror column when drawing inverted.
    const int col = inverted ? (kSlots - 1 - i) : i;
    const int maxLabelW = slotW - 8;
    const std::string label =
        renderer.truncatedText(kFooterFontId, labels[i], maxLabelW, EpdFontFamily::REGULAR);
    const int tw = renderer.getTextWidth(kFooterFontId, label.c_str(), EpdFontFamily::REGULAR);
    const int tx = col * slotW + (slotW - tw) / 2;
    renderer.drawText(kFooterFontId, tx, textY, label.c_str(), true, EpdFontFamily::REGULAR);
  }

  if (landscape) {
    renderer.setOrientation(origOrientation);
  }
}
