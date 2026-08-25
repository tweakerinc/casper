#pragma once

#include <algorithm>

// System top chrome (Display → Status Bar) on home and settings.
//
// Bare/Penumbra default is Hide / Battery Warning / Hide. Home used to paint
// the top bar only for Battery or Clock, so the warning never appeared (Settings
// force-previews it). Glyph Y was origin + 5, inside the 9px portrait bezel.
namespace systemchrome {

constexpr int kMinTextAir = 4;

struct HomeTopBarIn {
  bool hasBattery = false;
  bool hasClock = false;
  bool hasWarningSlot = false;
  int warnThresholdPercent = 0;
  int batteryPercent = 100;
  bool forcePreview = false;
};

inline bool warningIsLive(const HomeTopBarIn& in) {
  if (in.forcePreview) return true;
  return in.warnThresholdPercent > 0 && in.batteryPercent <= in.warnThresholdPercent;
}

inline bool needsHomeTopBar(const HomeTopBarIn& in) {
  return in.hasBattery || in.hasClock || (in.hasWarningSlot && warningIsLive(in));
}

// Air above the 8pt chrome line. Scales with panel height so X4 (800) and
// X3 (792) stay in proportion; floor matches Dictionary Lookup title air.
inline int textAir(const int screenH) { return std::max(kMinTextAir, std::max(1, screenH) / 200); }

// Top of the chrome glyphs. Must clear the oriented viewable bezel (portrait 9px).
inline int textY(const int viewableTop, const int screenH) { return std::max(0, viewableTop + textAir(screenH)); }

// Middle-slot warning. Bare default leaves the sides empty — use the full
// usable width. X4 portrait is 480 vs X3 528, so a half-width cap clipped
// "Battery 15% · Charge Soon" on X4 when sides were occupied.
inline int warningMaxWidth(const int screenW, const int insetL, const int insetR, const bool sidesOccupied) {
  const int usable = std::max(40, screenW - insetL - insetR);
  if (!sidesOccupied) return usable;
  return std::max(40, usable / 2);
}

}  // namespace systemchrome
