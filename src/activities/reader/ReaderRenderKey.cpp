#include "ReaderRenderKey.h"

#include <GfxRenderer.h>
#include <HalGPIO.h>

#include <algorithm>

#include "CrossPointSettings.h"
// ReaderUtils.h pulls ActivityManager, which holds a unique_ptr<Activity>.
#include "ReaderUtils.h"
#include "activities/Activity.h"
#include "components/UITheme.h"
#include "components/themes/BaseTheme.h"
#include "fontIds.h"
#include "util/ReaderChromePolicy.h"

namespace readerkey {

Layout compute(const GfxRenderer& renderer) {
  Layout out;
  rivulet::RenderKey& key = out.key;

  // Keep the selected face. FontLadder::resolve() already returns unknown
  // (SD) ids unchanged; StyleResolve's SD ladder hook loads extra sizes when
  // the pack has them. Do not swap SD ids for Literata/Source Serif.
  key.fontId = SETTINGS.getReaderFontId();

  // Match EpubReader computeReaderViewportLayout: top chrome air + bottom reserve
  // for status bar AND dictionary/clip front-button hint strip so last lines
  // stay readable when tools open.
  int oTop = 0, oRight = 0, oBottom = 0, oLeft = 0;
  renderer.getOrientedViewableTRBL(&oTop, &oRight, &oBottom, &oLeft);
  const int screenMargin = static_cast<int>(SETTINGS.screenMargin);
  const int statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Air between top chrome and body text. A third of a line reads as a clean
  // gap without spending a line of text on padding. X4 bottom overlay does not
  // reuse this as extra pad on top of the hint strip (see readerchrome).
  const float lcForLine = SETTINGS.getReaderLineCompression();
  const int bodyLine = std::max(12, renderer.getLineHeight(key.fontId, lcForLine));
  const int clearance = std::max(8, bodyLine / 3);

  // Landscape front-key chrome is a *side* strip (CCW right / CW left). Keep body
  // text clear of it so dictionary/clip can still see edge words.
  const auto orient = renderer.getOrientation();
  const bool landscapeCw = orient == GfxRenderer::Orientation::LandscapeClockwise;
  const bool landscapeCcw = orient == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const bool landscape = landscapeCw || landscapeCcw;
  const int frontSideReserve = landscape ? BaseTheme::frontButtonHintReserve(renderer) : 0;

  int marginL = std::max(0, oLeft + screenMargin);
  int marginR = std::max(0, oRight + screenMargin);
  if (landscapeCcw) {
    marginR = std::max(marginR, oRight + screenMargin + frontSideReserve);
  } else if (landscapeCw) {
    marginL = std::max(marginL, oLeft + screenMargin + frontSideReserve);
  }
  // Top: only reserve chrome height when an upper status slot is actually shown.
  // Readers who put everything in the bottom lane get that space back as text.
  const auto sbSpec = SETTINGS.statusBarSpec();
  const bool topChromeVisible = sbSpec.upperLeft != CrossPointSettings::CORNER_HIDE ||
                                sbSpec.upperMiddle != CrossPointSettings::CORNER_HIDE ||
                                sbSpec.upperRight != CrossPointSettings::CORNER_HIDE;
  const int topChromeBottom =
      BaseTheme::kTopChromeBatteryY + std::max(metrics.batteryHeight, renderer.getLineHeight(SMALL_FONT_ID));
  const int marginT =
      std::max(0, oTop + (topChromeVisible ? (topChromeBottom + clearance) : (screenMargin + clearance)));

  // Bottom: status lane and dictionary/clip hint strip overlay the same panel
  // edge (max, never a sum). X4 does not also stack screenMargin + line
  // clearance on that strip — that hole was ~1–2 body lines after the X3
  // 48→34 hint shrink.
  readerchrome::BottomIn bottom;
  bottom.oBottom = oBottom;
  bottom.screenMargin = screenMargin;
  bottom.statusBarHeight = statusBarHeight;
  bottom.progressBarHeight = UITheme::getInstance().getProgressBarHeight();
  bottom.statusBarVerticalMargin = metrics.statusBarVerticalMargin;
  bottom.clearance = clearance;
  bottom.hintStrip = landscape ? 0 : BaseTheme::frontButtonHintReserve(renderer);
  bottom.x4 = !gpio.deviceIsX3();
  const int marginB = readerchrome::marginBottom(bottom);

  key.viewportW = static_cast<uint16_t>(std::max(32, renderer.getScreenWidth() - marginL - marginR));
  key.viewportH = static_cast<uint16_t>(std::max(32, renderer.getScreenHeight() - marginT - marginB));
  key.marginL = static_cast<uint8_t>(std::min(255, marginL));
  key.marginR = static_cast<uint8_t>(std::min(255, marginR));
  key.marginT = static_cast<uint8_t>(std::min(255, marginT));
  key.marginB = static_cast<uint8_t>(std::min(255, marginB));

  const float lc = SETTINGS.getReaderLineCompression();
  key.lineCompressionQ8 = static_cast<uint16_t>(std::clamp(lc * 256.0f, 64.0f, 512.0f));

  // bit0 = Book's Style only: PageLayouter honors IR CSS/heuristic align + spacers.
  // Clear bit0 = force one align for all text (Left/Center/Right/Justify) — no CSS layout.
  // bits 2-3 = forced align enum (JUSTIFIED=0 … RIGHT=3). bit1 = extra paragraph spacing.
  // pad low nibble = Images mode; pad[5:4] = spacing height; bit7 legacy full.
  key.flags = 0;
  if (SETTINGS.paragraphAlignment == CrossPointSettings::BOOK_STYLE) {
    key.flags |= 1;
  } else {
    // JUSTIFIED..RIGHT are 0..3; mask so BOOK_STYLE (4) never leaks into force bits.
    const uint8_t force = static_cast<uint8_t>(SETTINGS.paragraphAlignment % CrossPointSettings::BOOK_STYLE);
    key.flags |= static_cast<uint8_t>((force & 0x3) << 2);
  }
  // Fingerprint Images mode + extra-para height so page maps invalidate on change.
  key.pad = static_cast<uint8_t>(SETTINGS.imageRendering & 0x0F);
  if (SETTINGS.extraParagraphSpacing != 0) {
    key.flags |= 2;
    const uint8_t h = SETTINGS.extraParagraphSpacingHeight;
    if (h == CrossPointSettings::SPACING_FULL) {
      key.flags |= 0x80;
      key.pad = static_cast<uint8_t>(key.pad | (1u << 4));
    } else if (h == CrossPointSettings::SPACING_QUARTER) {
      key.pad = static_cast<uint8_t>(key.pad | (2u << 4));
    }
    // half: pad height bits stay 0, bit7 clear
  }
  if (SETTINGS.hyphenationEnabled) key.flags |= 0x10;
  if (SETTINGS.focusReadingEnabled) key.flags |= 0x20;
  if (SETTINGS.guideReadingEnabled) key.flags |= 0x40;

  out.lineCompression = lc;
  out.marginL = marginL;
  out.marginT = marginT;
  out.marginR = marginR;
  out.marginB = marginB;
  return out;
}

}  // namespace readerkey
