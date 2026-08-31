#pragma once

// Sleep vs cold-boot: enterDeepSleep sets showBootScreen false; a reset while
// the device was awake leaves it true. Wake must resume the last activity
// (INX-style last-frame sleep) instead of painting the Casper splash.
namespace bootwake {

inline bool sleptLastShutdown(const bool showBootScreen) { return !showBootScreen; }

// X4 battery sleep cuts the GPIO13 latch, so wake is a cold ESP_RST_POWERON —
// the same reset reason as EN reset then power. Only the former is a sleep wake.
inline bool x4PowerOnIsSleepWake(const bool showBootScreen) { return sleptLastShutdown(showBootScreen); }

// Fold GPIO's first guess into "power-button sleep wake" vs splash:
//   - GPIO PowerButton + X4 POWERON shape + boot screen still armed → EN reset
//     (splash). Same shape after a real sleep → resume.
//   - GPIO Other after a real sleep → resume. X3 rail-cut / POWERON sleep
//     looks like a cold boot but showBootScreen is false.
//   - Flash / USB reasons are not passed in (caller keeps those).
inline bool isPowerButtonSleepWake(const bool gpioPowerButton, const bool gpioOther, const bool showBootScreen,
                                   const bool x4PowerOnEnResetShape) {
  if (gpioPowerButton) {
    if (x4PowerOnEnResetShape && !sleptLastShutdown(showBootScreen)) return false;
    return true;
  }
  return gpioOther && sleptLastShutdown(showBootScreen);
}

}  // namespace bootwake
