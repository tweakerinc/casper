#include "BootActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "fontIds.h"
#include "images/Logo120.h"

void BootActivity::onEnter() {
  Activity::onEnter();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  // Centered CrossPoint sheet-ghost logo + name (no "BOOTING" caption).
  constexpr int kLogoSize = 120;
  const int logoY = pageHeight / 2 - kLogoSize / 2 - 24;
  renderer.drawImage(Logo120, (pageWidth - kLogoSize) / 2, logoY, kLogoSize, kLogoSize);

  const int wordY = logoY + kLogoSize + 12;
  renderer.drawCenteredText(UI_12_FONT_ID, wordY, tr(STR_CROSSPOINT), true, EpdFontFamily::BOLD);

  const int versionY = pageHeight - renderer.getLineHeight(SMALL_FONT_ID) - 20;
  renderer.drawCenteredText(SMALL_FONT_ID, versionY, CROSSPOINT_VERSION, true);

  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}
