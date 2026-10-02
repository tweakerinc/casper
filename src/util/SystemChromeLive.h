#pragma once

#include <HalPowerManager.h>

#include "CrossPointSettings.h"
#include "SystemChromePolicy.h"

// Live Display → Status Bar slots + SoC. Firmware only (host tests use the
// pure SystemChromePolicy helpers).
inline systemchrome::HomeTopBarIn liveHomeTopBar() {
  using S = CrossPointSettings;
  systemchrome::HomeTopBarIn in;
  in.hasBattery = SETTINGS.systemStatusBarHas(S::SYS_SLOT_BATTERY);
  in.hasClock = SETTINGS.systemStatusBarHas(S::SYS_SLOT_CLOCK);
  in.hasWarningSlot = SETTINGS.systemStatusBarHas(S::SYS_SLOT_BATTERY_WARNING);
  in.warnThresholdPercent = SETTINGS.batteryWarningThresholdPercent();
  in.batteryPercent = static_cast<int>(powerManager.getBatteryPercentage());
  return in;
}

inline bool homeNeedsSystemChrome() { return systemchrome::needsHomeTopBar(liveHomeTopBar()); }
