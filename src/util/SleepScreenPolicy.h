#pragma once

#include <cstdint>

// Sleep Screen factory default is last-frame Quick Resume (INX-style).
// Must match CrossPointSettings::SLEEP_SCREEN_MODE (append-only).
namespace sleepscreen {

inline constexpr uint8_t kDark = 0;
inline constexpr uint8_t kLight = 1;
inline constexpr uint8_t kQuickResume = 6;

// One-shot: factory Light (Casper ghost wallpaper) → last-frame. Dark / Cover /
// Custom / Blank were explicit choices and stay put.
inline uint8_t migrateFactoryLightToQuickResume(const uint8_t sleepScreen, const bool alreadyMigrated) {
  if (alreadyMigrated) return sleepScreen;
  if (sleepScreen == kLight) return kQuickResume;
  return sleepScreen;
}

}  // namespace sleepscreen
