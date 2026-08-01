#pragma once

#include <GfxRenderer.h>
#include <HalClock.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>

#include "CrossPointSettings.h"
#include "components/themes/BaseTheme.h"
#include "fontIds.h"

// Quick Resume sleep/wake markers on the reader page.
// Ink-only (no white plate). Prefer under the power button (top-right),
// even when the right status slot has content (sit just inward of it).
// Only if the right side is unusable do we use empty left/middle; last resort
// mid/right gap.

namespace SleepChromeIcon {

constexpr int kMoonSrcW = 48;
constexpr int kMoonSrcH = 48;
constexpr int kMoonCropX = 7;
constexpr int kMoonCropY = 3;
constexpr int kMoonCropW = 35;
constexpr int kMoonCropH = 41;

inline int iconSize(const GfxRenderer& renderer) {
  const int clockH = renderer.getLineHeight(SMALL_FONT_ID);
  const int battH = BaseMetrics::values.batteryHeight;
  return std::max({clockH, battH, 14});
}

inline int topY(const GfxRenderer& renderer) {
  const int size = iconSize(renderer);
  const int rowY = BaseMetrics::values.topPadding + BaseTheme::kTopChromeBatteryY;
  const int rowH = std::max(BaseMetrics::values.batteryHeight, renderer.getLineHeight(SMALL_FONT_ID));
  return rowY + std::max(0, (rowH - size) / 2);
}

inline bool upperSlotEmpty(const uint8_t content) {
  return content == CrossPointSettings::CORNER_HIDE;
}

// Approximate width of the upper-right status item so the moon can sit just
// left of it (still under power) when the slot is occupied.
inline int estimateUpperRightWidth(const GfxRenderer& renderer) {
  using C = CrossPointSettings::STATUS_BAR_CORNER_CONTENT;
  const uint8_t content = SETTINGS.statusBarUpperRight;
  if (content == C::CORNER_HIDE) return 0;

  const auto& metrics = BaseMetrics::values;
  if (content == C::CORNER_BATTERY) {
    const uint8_t mode = SETTINGS.readerBatteryDisplay < CrossPointSettings::BATTERY_DISPLAY_MODE_COUNT
                             ? SETTINGS.readerBatteryDisplay
                             : static_cast<uint8_t>(CrossPointSettings::BATTERY_DISPLAY_ICON_PERCENT);
    const bool showIcon = mode != CrossPointSettings::BATTERY_DISPLAY_PERCENT;
    const bool showPct = mode != CrossPointSettings::BATTERY_DISPLAY_ICON;
    const int iconW = metrics.batteryWidth;
    const int pctW = showPct ? renderer.getTextWidth(SMALL_FONT_ID, "100%") : 0;
    if (showIcon && showPct) return iconW + 4 + pctW;
    if (showIcon) return iconW;
    return pctW;
  }
  if (content == C::CORNER_PROGRESS_PERCENT) {
    // "100% complete" worst case.
    return renderer.getTextWidth(SMALL_FONT_ID, "100% complete");
  }
  if (content == C::CORNER_CLOCK) {
    char buf[16];
    if (halClock.isAvailable() &&
        halClock.formatTime(buf, sizeof(buf), SETTINGS.clockUtcOffsetQ, SETTINGS.clockFormat == 1)) {
      return renderer.getTextWidth(SMALL_FONT_ID, buf);
    }
    return renderer.getTextWidth(SMALL_FONT_ID, "12:00 PM");
  }
  // Page counters / other short chrome.
  return renderer.getTextWidth(SMALL_FONT_ID, "999/999");
}

// Under power = top-right of the oriented panel.
// free right  → flush to right edge
// occupied   → just left of that right chrome (still top-right / under power)
// squeezed   → empty left, then empty middle, then mid/right gap
inline int leftX(const GfxRenderer& renderer) {
  int orientedMarginTop = 0, orientedMarginRight = 0, orientedMarginBottom = 0, orientedMarginLeft = 0;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  (void)orientedMarginTop;
  (void)orientedMarginBottom;

  const int screenW = renderer.getScreenWidth();
  const int centerX = screenW / 2;
  const int leftEdge = orientedMarginLeft + BaseTheme::kTopChromeInsetX;
  const int rightEdge = screenW - orientedMarginRight - BaseTheme::kTopChromeInsetX;
  const int size = iconSize(renderer);
  constexpr int kGap = 6;

  const bool rightFree = upperSlotEmpty(SETTINGS.statusBarUpperRight);
  const bool leftFree = upperSlotEmpty(SETTINGS.statusBarUpperLeft);
  const bool midFree = upperSlotEmpty(SETTINGS.statusBarUpperMiddle);

  // 1) Under power — top-right (flush or inset past right status content).
  if (rightFree) {
    return rightEdge - size;
  }
  const int rightW = estimateUpperRightWidth(renderer);
  const int underPowerX = rightEdge - rightW - kGap - size;
  // Keep it on the right half so it still reads as "under power".
  if (underPowerX >= centerX) {
    return underPowerX;
  }

  // 2) Empty left / middle.
  if (leftFree) return leftEdge;
  if (midFree) return centerX - size / 2;

  // 3) Last resort: between middle and right (legacy).
  const int iconCenter = (centerX + rightEdge) / 2;
  return iconCenter - size / 2;
}

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

inline void drawLoadingDots(const GfxRenderer& renderer, const int x, const int y, const int size) {
  const int r = std::max(2, size / 7);
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

inline void replaceAtTopChrome(const GfxRenderer& renderer, const uint8_t* /*src*/, const int /*srcW*/,
                               const int /*srcH*/) {
  const int x = leftX(renderer);
  const int y = topY(renderer);
  const int size = iconSize(renderer);
  renderer.fillRect(x, y, size, size, false);
  drawLoadingDots(renderer, x, y, size);
}

}  // namespace SleepChromeIcon
