#pragma once

// X4 battery sleep cuts the GPIO13 latch, so wake is a cold ESP_RST_POWERON —
// the same reset reason as EN reset then power. enterDeepSleep sets
// showBootScreen false; a reset while the device was awake leaves it true.
// Only the former is a sleep wake (Quick Resume). The latter must splash.
namespace bootwake {

inline bool x4PowerOnIsSleepWake(const bool showBootScreen) { return !showBootScreen; }

}  // namespace bootwake
