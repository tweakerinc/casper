#include "HalPowerManager.h"

#include <BoardConfig.h>
#include <Logging.h>
#include <PowerManager.h>
#include <WiFi.h>
#include <driver/gpio.h>
#include <esp_sleep.h>
#include <esp_wifi.h>

#include <cassert>

#include "HalGPIO.h"
#include "SleepHoldPolicy.h"

HalPowerManager powerManager;  // Singleton instance

void HalPowerManager::begin() {
  if (BoardConfig::ACTIVE.batteryAdc >= 0) {
    pinMode(BoardConfig::ACTIVE.batteryAdc, INPUT);
  }
  normalFreq = getCpuFrequencyMhz();
  modeMutex = xSemaphoreCreateMutex();
  assert(modeMutex != nullptr);
}

void HalPowerManager::setPowerSaving(bool enabled) {
  if (normalFreq <= 0) {
    return;  // invalid state
  }

  // Serialize frequency changes: concurrent Lock + idle path logged dual
  // "Restoring normal CPU frequency" and can hang setCpuFrequencyMhz on C3.
  if (modeMutex) {
    xSemaphoreTake(modeMutex, portMAX_DELAY);
  }

  auto wifiMode = WiFi.getMode();
  if (wifiMode != WIFI_MODE_NULL) {
    // Wifi is active, force disabling power saving
    enabled = false;
  }

  const LockMode mode = currentLockMode;
  bool didChange = false;

  if (mode == None && enabled && !isLowPower) {
    LOG_DBG("PWR", "Going to low-power mode");
    if (!setCpuFrequencyMhz(LOW_POWER_FREQ)) {
      LOG_DBG("PWR", "Failed to set CPU frequency = %d MHz", LOW_POWER_FREQ);
    } else {
      isLowPower = true;
      didChange = true;
    }

  } else if ((!enabled || mode != None) && isLowPower) {
    LOG_DBG("PWR", "Restoring normal CPU frequency");
    if (!setCpuFrequencyMhz(normalFreq)) {
      LOG_DBG("PWR", "Failed to set CPU frequency = %d MHz", normalFreq);
    } else {
      isLowPower = false;
      didChange = true;
    }
  }

  (void)didChange;
  if (modeMutex) {
    xSemaphoreGive(modeMutex);
  }
}

namespace {

void keepHoldThroughIsolate(const gpio_num_t pin) {
  gpio_sleep_sel_dis(pin);
  gpio_hold_en(pin);
}

void keepHoldThroughIsolateIfAssigned(const int8_t pin) {
  if (pin < 0) return;
  keepHoldThroughIsolate(static_cast<gpio_num_t>(pin));
}

// GPIO13 LOW + hold. Must run again after isolate (see sleephold::reassertHoldAfterIsolate).
void holdXteinkC3Gpio13Off() {
  const auto pin = static_cast<gpio_num_t>(sleephold::kXteinkC3CutGpio);
  gpio_hold_dis(pin);
  gpio_set_direction(pin, GPIO_MODE_OUTPUT);
  gpio_set_level(pin, 0);
  keepHoldThroughIsolate(pin);
}

}  // namespace

void HalPowerManager::startDeepSleep(HalGPIO& gpio) const {
  // USB CDC-on-boot leaves the Serial/JTAG PHY up unless we end it even when
  // serial logging is compiled out.
#if defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT
  logSerial.end();
#endif

  // Modem domain stays alive after a mere disconnect. Wake is a chip reset.
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(true /*wifioff*/, false /*eraseap*/);
    WiFi.mode(WIFI_OFF);
  }
  (void)esp_wifi_stop();

#if defined(FREEINK_MCU_C3) && FREEINK_MCU_C3
  // X4: battery MOSFET. X3: SD VCC. Same pin, same OFF level. Skip if this
  // GPIO is a bus pin on the active profile (S3 X4 Pro display CS).
  const bool cutGpio13 = gpio.isXteinkDevice() && !BoardConfig::latchConflictsWithBus(sleephold::kXteinkC3CutGpio) &&
                         sleephold::cutGpio13InSleep(gpio.deviceIsX3() ? sleephold::Device::X3 : sleephold::Device::X4);
  if (cutGpio13) holdXteinkC3Gpio13Off();
#else
  constexpr bool cutGpio13 = false;
  (void)gpio;
#endif

  // Cut the gated peripheral rails (touch/SD/EPD on boards like the Sticky) and
  // hold the enables off through deep sleep — otherwise the GT911 and SD card
  // stay powered all through "off" and drain the battery. No-op on boards with
  // no switched rails (X4/X3). Trade-off: no touch-to-wake; wake is the power
  // button. Must run after display.deepSleep() so the panel controller gets its
  // deep-sleep command while its rail is still up (enterDeepSleep() in main.cpp
  // guarantees that ordering).
  freeink::PowerManager::powerDownRailsForSleep();

  freeink::PowerManager::waitForPowerButtonRelease();
  freeink::PowerManager::armPowerButtonWakeup();

  // Isolate floating pads, then put the rail holds back — isolate enables
  // sleep_sel on every GPIO and would otherwise drop gpio_hold_en.
  esp_sleep_config_gpio_isolate();
  if (sleephold::reassertHoldAfterIsolate()) {
    if (cutGpio13) holdXteinkC3Gpio13Off();
    const auto& b = BoardConfig::ACTIVE;
    keepHoldThroughIsolateIfAssigned(b.display.powerEnable);
    keepHoldThroughIsolateIfAssigned(b.sd.powerEnable);
    keepHoldThroughIsolateIfAssigned(b.touch.powerEnable);
    keepHoldThroughIsolateIfAssigned(b.mic.enable);
    keepHoldThroughIsolateIfAssigned(b.power.latch0);
    keepHoldThroughIsolateIfAssigned(b.power.latch1);
  }
  gpio_deep_sleep_hold_en();
  esp_deep_sleep_start();
  while (true) {
  }
}

uint16_t HalPowerManager::getBatteryPercentage() const {
  static const BatteryMonitor battery;
  if (BoardConfig::ACTIVE.batteryGauge.gaugeAddr != 0) {
    const unsigned long now = millis();
    if (_batteryLastPollMs != 0 && (now - _batteryLastPollMs) < BATTERY_POLL_MS) {
      return _batteryCachedPercent;
    }

    _batteryLastPollMs = now;
    uint16_t percent = 0;
    if (!battery.readPercentageChecked(percent)) {
      return _batteryCachedPercent;
    }
    _batteryCachedPercent = percent;
    return _batteryCachedPercent;
  }

  // smooth the battery %.
  if (_batteryCachedPercent == 0) {
    _batteryCachedPercent = 10 * battery.readPercentage();
  } else {
    _batteryCachedPercent = (_batteryCachedPercent * 9 + battery.readPercentage() * 10) / 10;
  }
  return _batteryCachedPercent / 10;
}

HalPowerManager::Lock::Lock() {
  xSemaphoreTake(powerManager.modeMutex, portMAX_DELAY);
  // Current limitation: only one lock at a time
  if (powerManager.currentLockMode != None) {
    LOG_ERR("PWR", "Lock already held, ignore");
    valid = false;
  } else {
    powerManager.currentLockMode = NormalSpeed;
    valid = true;
  }
  xSemaphoreGive(powerManager.modeMutex);
  if (valid) {
    // Immediately restore normal CPU frequency if currently in low-power mode
    powerManager.setPowerSaving(false);
  }
}

HalPowerManager::Lock::~Lock() {
  xSemaphoreTake(powerManager.modeMutex, portMAX_DELAY);
  if (valid) {
    powerManager.currentLockMode = None;
  }
  xSemaphoreGive(powerManager.modeMutex);
}
