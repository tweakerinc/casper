#pragma once

#include <GfxRenderer.h>

#include <algorithm>
#include <cstdint>

#include "components/themes/BaseTheme.h"
#include "fontIds.h"

// Quick Resume sleep/wake markers on the reader page.
// Ink-only (no white plate). Between middle and right status anchors.
// Vertically matched to the *reader* top status-bar row (clock / battery / %),
// then dropped a bit into the chrome air above the book text.

namespace SleepChromeIcon {

// MoonIcon 48×48 has large empty margins; crop to ink so it fills the chrome box.
constexpr int kMoonSrcW = 48;
constexpr int kMoonSrcH = 48;
constexpr int kMoonCropX = 7;
constexpr int kMoonCropY = 3;
constexpr int kMoonCropW = 35;
constexpr int kMoonCropH = 41;

// Match clock/battery text height (SMALL_FONT line height vs battery icon).
inline int iconSize(const GfxRenderer& renderer) {
  const int clockH = renderer.getLineHeight(SMALL_FONT_ID);
  const int battH = BaseMetrics::values.batteryHeight;
  return std::max({clockH, battH, 14});
}

// Same top row as BaseTheme::drawStatusBar reader chrome:
//   topTextY = topPadding + kTopChromeBatteryY
// Center the icon on that row so it lines up with clock / battery / %.
inline int topY(const GfxRenderer& renderer) {
  const int size = iconSize(renderer);
  const int rowY = BaseMetrics::values.topPadding + BaseTheme::kTopChromeBatteryY;
  const int rowH = std::max(BaseMetrics::values.batteryHeight, renderer.getLineHeight(SMALL_FONT_ID));
  return rowY + std::max(0, (rowH - size) / 2);
}

// Midway between middle and right status anchors (avoids centered clock).
inline int leftX(const GfxRenderer& renderer) {
  int orientedMarginTop = 0, orientedMarginRight = 0, orientedMarginBottom = 0, orientedMarginLeft = 0;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  (void)orientedMarginTop;
  (void)orientedMarginBottom;
  const int screenW = renderer.getScreenWidth();
  const int centerX = screenW / 2;
  const int rightX = screenW - orientedMarginRight - BaseTheme::kTopChromeInsetX;
  const int size = iconSize(renderer);
  const int iconCenter = (centerX + rightX) / 2;
  return iconCenter - size / 2;
}

// Scale a crop of a 1bpp MSB image into dstW×dstH, ink-only (white = transparent).
inline void drawTransparentScaledCrop(const GfxRenderer& renderer, const uint8_t* src, const int srcW,
                                      const int cropX, const int cropY, const int cropW, const int cropH,
                                      const int dstX, const int dstY, const int dstW, const int dstH) {
  if (!src || cropW <= 0 || cropH <= 0 || dstW <= 0 || dstH <= 0) return;
  const int rowBytes = (srcW + 7) / 8;
  for (int dy = 0; dy < dstH; ++dy) {
    const int sy = cropY + dy * cropH / dstH;
    for (int dx = 0; dx < dstW; ++dx) {
      const int sx = cropX + dx * cropW / dstW;
      if (sx < 0 || sy < 0 || sx >= kMoonSrcW || sy >= kMoonSrcH) continue;
      const uint8_t byte = src[sy * rowBytes + (sx >> 3)];
      const bool ink = ((byte >> (7 - (sx & 7))) & 1) == 0;
      if (ink) {
        renderer.drawPixel(dstX + dx, dstY + dy, true);
      }
    }
  }
}

// Fit moon crop into the square chrome box (fill height; center horizontally).
inline void drawMoonFitted(const GfxRenderer& renderer, const uint8_t* src, const int boxX, const int boxY,
                           const int boxSize) {
  const int dh = boxSize;
  const int dw = std::max(1, (kMoonCropW * boxSize) / kMoonCropH);
  const int ox = boxX + (boxSize - dw) / 2;
  const int oy = boxY;
  drawTransparentScaledCrop(renderer, src, kMoonSrcW, kMoonCropX, kMoonCropY, kMoonCropW, kMoonCropH, ox, oy, dw, dh);
}

inline void fillDot(const GfxRenderer& renderer, const int cx, const int cy, const int r) {
  const int r2 = r * r;
  for (int dy = -r; dy <= r; ++dy) {
    for (int dx = -r; dx <= r; ++dx) {
      if (dx * dx + dy * dy <= r2) {
        renderer.drawPixel(cx + dx, cy + dy, true);
      }
    }
  }
}

// Three horizontal loading dots. Bottom of the dots lines up with the bottom
// of the moon box (not vertically centered — that read as "too high").
inline void drawLoadingDots(const GfxRenderer& renderer, const int x, const int y, const int size) {
  const int r = std::max(2, size / 7);
  // Rest on the same baseline as the moon's bottom edge.
  const int cy = y + size - 1 - r;
  const int gap = std::max(r * 2 + 2, size / 3);
  const int midX = x + size / 2;
  fillDot(renderer, midX - gap, cy, r);
  fillDot(renderer, midX, cy, r);
  fillDot(renderer, midX + gap, cy, r);
}

inline void drawAtTopChrome(const GfxRenderer& renderer, const uint8_t* src, const int /*srcW*/, const int /*srcH*/) {
  const int size = iconSize(renderer);
  drawMoonFitted(renderer, src, leftX(renderer), topY(renderer), size);
}

// Clear chrome box then draw horizontal loading dots (same size/position as moon).
inline void replaceAtTopChrome(const GfxRenderer& renderer, const uint8_t* /*src*/, const int /*srcW*/,
                               const int /*srcH*/) {
  const int x = leftX(renderer);
  const int y = topY(renderer);
  const int size = iconSize(renderer);
  renderer.fillRect(x, y, size, size, false);
  drawLoadingDots(renderer, x, y, size);
}

}  // namespace SleepChromeIcon
