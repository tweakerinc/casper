#pragma once

#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalGPIO.h>
#include <I18n.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "components/UITheme.h"
#include "components/themes/BaseTheme.h"
#include "fontIds.h"

// Quick Resume sleep/wake markers on the retained page.
// Home uses system status-bar slots; reader uses reader status-bar slots.
// Prefer under power (top-right). If that corner has chrome, sit ~8px left of it.

namespace SleepChromeIcon {

constexpr int kMoonSrcW = 48;
constexpr int kMoonSrcH = 48;
constexpr int kMoonCropX = 7;
constexpr int kMoonCropY = 3;
constexpr int kMoonCropW = 35;
constexpr int kMoonCropH = 41;
// Air between moon and right-side status content when the right slot is filled.
constexpr int kRightChromeGap = 8;
// When the right slot is empty, hug the physical corner (tighter than status chrome inset).
// X4 looks balanced at 2px; X3 reads a hair too far right → extra inset on X3 only.
constexpr int kCornerInsetX = 2;
constexpr int kCornerInsetX3Extra = 3;  // pull moon left ~3px on X3

enum class ChromeContext : uint8_t { Home, Reader };

// QR sleep/wake: use the chrome of the screen still on the panel.
inline ChromeContext currentContext() {
  return APP_STATE.lastSleepFromReader ? ChromeContext::Reader : ChromeContext::Home;
}

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

// Right edge for flush corner placement (tighter than full status chrome margin).
inline int cornerRightEdgeX(const GfxRenderer& renderer) {
  int oTop = 0, oRight = 0, oBottom = 0, oLeft = 0;
  renderer.getOrientedViewableTRBL(&oTop, &oRight, &oBottom, &oLeft);
  (void)oTop;
  (void)oBottom;
  (void)oLeft;
  const int inset = kCornerInsetX + (gpio.deviceIsX3() ? kCornerInsetX3Extra : 0);
  return renderer.getScreenWidth() - oRight - inset;
}

// Right edge of drawn status chrome (for sitting just left of progress/battery).
inline int statusRightEdgeX(const GfxRenderer& renderer, const ChromeContext ctx) {
  int oTop = 0, oRight = 0, oBottom = 0, oLeft = 0;
  renderer.getOrientedViewableTRBL(&oTop, &oRight, &oBottom, &oLeft);
  (void)oTop;
  (void)oBottom;
  (void)oLeft;
  const int screenW = renderer.getScreenWidth();
  if (ctx == ChromeContext::Reader) {
    const auto& m = UITheme::getInstance().getMetrics();
    return screenW - m.statusBarHorizontalMargin - oRight;
  }
  return screenW - oRight - BaseTheme::kTopChromeInsetX;
}

inline int leftEdgeX(const GfxRenderer& renderer, const ChromeContext ctx) {
  int oTop = 0, oRight = 0, oBottom = 0, oLeft = 0;
  renderer.getOrientedViewableTRBL(&oTop, &oRight, &oBottom, &oLeft);
  (void)oTop;
  (void)oBottom;
  (void)oRight;
  if (ctx == ChromeContext::Reader) {
    const auto& m = UITheme::getInstance().getMetrics();
    return m.statusBarHorizontalMargin + oLeft;
  }
  return oLeft + BaseTheme::kTopChromeInsetX;
}

inline int batteryGroupWidth(const GfxRenderer& renderer, const uint8_t displayMode) {
  const auto& metrics = BaseMetrics::values;
  const uint8_t mode = displayMode < CrossPointSettings::BATTERY_DISPLAY_MODE_COUNT
                           ? displayMode
                           : static_cast<uint8_t>(CrossPointSettings::BATTERY_DISPLAY_ICON_PERCENT);
  const bool showIcon = mode != CrossPointSettings::BATTERY_DISPLAY_PERCENT;
  const bool showPct = mode != CrossPointSettings::BATTERY_DISPLAY_ICON;
  const int iconW = metrics.batteryWidth;
  const int pctW = showPct ? renderer.getTextWidth(SMALL_FONT_ID, "100%") : 0;
  if (showIcon && showPct) return iconW + 4 + pctW;
  if (showIcon) return iconW;
  return pctW;
}

// Width of whatever is drawn in the top-right slot for this chrome context.
// 0 ⇒ right corner is free → moon flush right.
inline int rightSlotWidth(const GfxRenderer& renderer, const ChromeContext ctx) {
  using S = CrossPointSettings;
  if (ctx == ChromeContext::Home) {
    const uint8_t slot = SETTINGS.systemStatusBarRight;
    if (slot == S::SYS_SLOT_HIDE) return 0;
    if (slot == S::SYS_SLOT_BATTERY) {
      return batteryGroupWidth(renderer, SETTINGS.systemBatteryDisplay);
    }
    if (slot == S::SYS_SLOT_CLOCK) {
      char buf[16];
      if (halClock.isAvailable() &&
          halClock.formatTime(buf, sizeof(buf), SETTINGS.clockUtcOffsetQ, SETTINGS.clockFormat == 1)) {
        return renderer.getTextWidth(SMALL_FONT_ID, buf);
      }
      return renderer.getTextWidth(SMALL_FONT_ID, "12:00 PM");
    }
    if (slot == S::SYS_SLOT_BATTERY_WARNING) {
      // Only drawn when low; on sleep we still reserve a modest width if assigned.
      return renderer.getTextWidth(SMALL_FONT_ID, "Battery 15%");
    }
    return 0;
  }

  // Reader chrome
  using C = S::STATUS_BAR_CORNER_CONTENT;
  const uint8_t content = SETTINGS.statusBarUpperRight;
  if (content == C::CORNER_HIDE) return 0;
  if (content == C::CORNER_BATTERY) {
    return batteryGroupWidth(renderer, SETTINGS.readerBatteryDisplay);
  }
  if (content == C::CORNER_PROGRESS_PERCENT) {
    // Typical drawn form: "42% complete" — use a mid-length sample, not padded max.
    char buf[32];
    snprintf(buf, sizeof(buf), "99%% %s", tr(STR_COMPLETE));
    return renderer.getTextWidth(SMALL_FONT_ID, buf);
  }
  if (content == C::CORNER_CLOCK) {
    char buf[16];
    if (halClock.isAvailable() &&
        halClock.formatTime(buf, sizeof(buf), SETTINGS.clockUtcOffsetQ, SETTINGS.clockFormat == 1)) {
      return renderer.getTextWidth(SMALL_FONT_ID, buf);
    }
    return renderer.getTextWidth(SMALL_FONT_ID, "12:00 PM");
  }
  if (content == C::CORNER_CHAPTER_PAGE_COUNTER || content == C::CORNER_BOOK_PAGE_COUNTER) {
    return renderer.getTextWidth(SMALL_FONT_ID, "Pg. 999/999");
  }
  if (content == C::CORNER_CHAPTER_COUNTER) {
    return renderer.getTextWidth(SMALL_FONT_ID, "Ch. 99/99");
  }
  // Titles are rare on upper-right; keep modest.
  return renderer.getTextWidth(SMALL_FONT_ID, "999/999");
}

inline bool leftSlotEmpty(const ChromeContext ctx) {
  if (ctx == ChromeContext::Home) {
    return SETTINGS.systemStatusBarLeft == CrossPointSettings::SYS_SLOT_HIDE;
  }
  return SETTINGS.statusBarUpperLeft == CrossPointSettings::CORNER_HIDE;
}

inline bool midSlotEmpty(const ChromeContext ctx) {
  if (ctx == ChromeContext::Home) {
    return SETTINGS.systemStatusBarMiddle == CrossPointSettings::SYS_SLOT_HIDE;
  }
  return SETTINGS.statusBarUpperMiddle == CrossPointSettings::CORNER_HIDE;
}

// Under power (top-right). Free right → flush corner. Occupied → 8px left of that chrome.
inline int leftX(const GfxRenderer& renderer) {
  const ChromeContext ctx = currentContext();
  const int size = iconSize(renderer);
  const int cornerRight = cornerRightEdgeX(renderer);
  const int statusRight = statusRightEdgeX(renderer, ctx);
  const int leftEdge = leftEdgeX(renderer, ctx);
  const int centerX = renderer.getScreenWidth() / 2;
  const int rightW = rightSlotWidth(renderer, ctx);

  // 1) Empty right slot → perfect corner (tight inset only).
  if (rightW <= 0) {
    return cornerRight - size;
  }
  // 2) Occupied: sit just left of the right chrome (8px gap), still under power.
  const int underPowerX = statusRight - rightW - kRightChromeGap - size;
  if (underPowerX >= centerX) {
    return underPowerX;
  }

  // 3) Empty left / middle
  if (leftSlotEmpty(ctx)) return leftEdge;
  if (midSlotEmpty(ctx)) return centerX - size / 2;

  // 4) Last resort mid/right gap
  return (centerX + statusRight) / 2 - size / 2;
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

// Right-align the moon ink in the box so empty glyph margins don't leave it
// looking inset from the corner.
inline void drawMoonFitted(const GfxRenderer& renderer, const uint8_t* src, const int boxX, const int boxY,
                           const int boxSize) {
  const int dh = boxSize;
  const int dw = std::max(1, (kMoonCropW * boxSize) / kMoonCropH);
  const int ox = boxX + (boxSize - dw);  // flush right of the placement box
  drawTransparentScaledCrop(renderer, src, kMoonSrcW, kMoonCropX, kMoonCropY, kMoonCropW, kMoonCropH, ox, boxY, dw,
                            dh);
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
