#pragma once

#include <cstdint>

// Which rails to hold off through ESP32-C3 deep sleep.
//
// Xteink C3 boards share GPIO13:
//   X4 / newer-batch X4 (UC8179, often called "X4 Pro"): battery MOSFET.
//     LOW = MCU unpowered. Months of shelf life.
//   X3: SD-card VCC. LOW = card unpowered.
//
// The SDK sleep path calls esp_sleep_config_gpio_isolate() *after* we gpio_hold_en
// that pin. Isolate enables gpio_sleep_sel on every pad, which drops the digital
// hold so GPIO13 floats. The MOSFET/SD rail stays on, the chip sits in deep sleep
// still powered, and a pack that should last months dies in days.
//
// Callers must: hold LOW, isolate, then gpio_sleep_sel_dis + gpio_hold_en again
// on this pin (and any other rail holds) before gpio_deep_sleep_hold_en().
namespace sleephold {

inline constexpr int kXteinkC3CutGpio = 13;

enum class Device : uint8_t { X3 = 0, X4 = 1, Other = 2 };

// Both C3 Xteink SKUs need GPIO13 held LOW in sleep. Skip on unrelated boards
// (X4 Pro S3 uses GPIO13 as display CS).
inline constexpr bool cutGpio13InSleep(const Device device) {
  switch (device) {
    case Device::X3:
    case Device::X4:
      return true;
    case Device::Other:
      return false;
  }
  return false;
}

// Isolate runs after the first hold. Without this re-hold, the cut does not stick.
inline constexpr bool reassertHoldAfterIsolate() { return true; }

}  // namespace sleephold
