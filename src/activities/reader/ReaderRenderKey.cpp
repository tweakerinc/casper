#include "ReaderRenderKey.h"

#include <GfxRenderer.h>

#include <algorithm>

#include "CasperSettings.h"
// ReaderUtils.h pulls ActivityManager, which holds a unique_ptr<Activity>.
#include "activities/Activity.h"
#include "ReaderUtils.h"
#include "components/UITheme.h"
#include "components/themes/BaseTheme.h"
#include "fontIds.h"

namespace readerkey {
namespace {

// SD-card fonts have no Rivulet v1 ladder, so fall back to the builtin ladder
// closest to the chosen size. Builtin ladder is 10/12/14/16 only; 8 maps to 10
// and 18 maps to 16.
int32_t builtinLadderFontId() {
  const bool literata = SETTINGS.fontFamily == CasperSettings::LITERATA;
  switch (SETTINGS.fontSize) {
    case CasperSettings::SIZE_8:
    case CasperSettings::SIZE_10:
      return literata ? LITERATA_10_FONT_ID : SOURCESERIF4_10_FONT_ID;
    case CasperSettings::SIZE_14:
      return literata ? LITERATA_14_FONT_ID : SOURCESERIF4_14_FONT_ID;
    case CasperSettings::SIZE_16:
    case CasperSettings::SIZE_18:
      return literata ? LITERATA_16_FONT_ID : SOURCESERIF4_16_FONT_ID;
    case CasperSettings::SIZE_12:
    default:
      return literata ? LITERATA_12_FONT_ID : SOURCESERIF4_12_FONT_ID;
  }
}

}  // namespace

Layout compute(const GfxRenderer& renderer) {
  Layout out;
  rivulet::RenderKey& key = out.key;

  key.fontId = SETTINGS.getReaderFontId();
  if (renderer.isSdCardFont(key.fontId)) {
    key.fontId = builtinLadderFontId();
  }

  // Match EpubReader computeReaderViewportLayout: top chrome air + bottom reserve
  // for status bar AND dictionary/clip front-button hint strip so last lines
  // stay readable when tools open.
  int oTop = 0, oRight = 0, oBottom = 0, oLeft = 0;
  renderer.getOrientedViewableTRBL(&oTop, &oRight, &oBottom, &oLeft);
  const int screenMargin = static_cast<int>(SETTINGS.screenMargin);
  const int statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  // Tool hint strip scales with Menu Font Size (dictionary/clip front keys).
  const int toolHintBand = ReaderUtils::readerToolHintBand(renderer);
  // Landscape front-key chrome is a *side* strip (CCW right / CW left). Keep body
  // text clear of it so dictionary/clip can still see edge words (same idea as
  // bottom reserve in portrait).
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
  // Top: screen margin + chrome extra (battery/clock). Avoid double-counting
  // oriented top if it's already large on some panels.
  const int marginT = std::max(0, oTop + screenMargin + ReaderUtils::readerTopChromeExtra());

  // Bottom: take the taller of status chrome vs tool-hint strip — do not stack
  // full top-chrome air on top of statusBarHeight (that cost ~one body line).
  // Menu Font Size grows/shrinks toolHintBand so reserve stays adaptive.
  int statusBand = statusBarHeight + ReaderUtils::readerBottomTextClearance();
  if (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight()) {
    statusBand = std::max(statusBand, statusBarHeight + UITheme::getInstance().getMetrics().statusBarVerticalMargin +
                                          ReaderUtils::readerBottomTextClearance());
  }
  int chromeBand = statusBand;
  if (!landscape) {
    chromeBand = std::max(chromeBand, toolHintBand);
  }
  if (SETTINGS.statusBarFontSize >= CasperSettings::STATUS_BAR_FONT_10) {
    chromeBand += 4;
  }
  const int marginB = std::max(0, oBottom + screenMargin + chromeBand + ReaderUtils::kReaderBottomChromePad);

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
  if (SETTINGS.paragraphAlignment == CasperSettings::BOOK_STYLE) {
    key.flags |= 1;
  } else {
    // JUSTIFIED..RIGHT are 0..3; mask so BOOK_STYLE (4) never leaks into force bits.
    const uint8_t force = static_cast<uint8_t>(SETTINGS.paragraphAlignment % CasperSettings::BOOK_STYLE);
    key.flags |= static_cast<uint8_t>((force & 0x3) << 2);
  }
  // Fingerprint Images mode + extra-para height so page maps invalidate on change.
  key.pad = static_cast<uint8_t>(SETTINGS.imageRendering & 0x0F);
  if (SETTINGS.extraParagraphSpacing != 0) {
    key.flags |= 2;
    const uint8_t h = SETTINGS.extraParagraphSpacingHeight;
    if (h == CasperSettings::SPACING_FULL) {
      key.flags |= 0x80;
      key.pad = static_cast<uint8_t>(key.pad | (1u << 4));
    } else if (h == CasperSettings::SPACING_QUARTER) {
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
