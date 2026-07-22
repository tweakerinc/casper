#include "BootActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "AppVersion.h"
#include "fontIds.h"
#include "images/Logo120.h"

namespace {
// Prefer a soft serif for the brand wordmark; fall back when faces are omitted.
int casperWordmarkFontId() {
#if !defined(OMIT_XLARGE_FONT)
  return BITTER_18_FONT_ID;
#elif !defined(OMIT_LARGE_FONT)
  return BITTER_16_FONT_ID;
#elif !defined(OMIT_MEDIUM_FONT)
  return BITTER_14_FONT_ID;
#else
  return UI_12_FONT_ID;
#endif
}
}  // namespace

void BootActivity::onEnter() {
  Activity::onEnter();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  // Centered Casper logo.
  constexpr int kLogoSize = 120;
  const int logoY = pageHeight / 2 - kLogoSize / 2 - 24;
  renderer.drawImage(Logo120, (pageWidth - kLogoSize) / 2, logoY, kLogoSize, kLogoSize);

  // "Casper" under the logo in a cute serif.
  const int wordFont = casperWordmarkFontId();
  const int wordY = logoY + kLogoSize + 12;
  renderer.drawCenteredText(wordFont, wordY, tr(STR_CROSSINK), true, EpdFontFamily::BOLD);

  // Build number pinned to the bottom of the page.
  const int versionY = pageHeight - renderer.getLineHeight(SMALL_FONT_ID) - 20;
  renderer.drawCenteredText(SMALL_FONT_ID, versionY, CROSSINK_VERSION, true);

  renderer.displayBuffer();
}
