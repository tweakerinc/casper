#include "HalPowerManager.h"

#include <Logging.h>
#include <WiFi.h>
#include <esp_sleep.h>

#include <algorithm>
#include <cassert>
#include <cmath>

#include "HalGPIO.h"

HalPowerManager powerManager;  // Singleton instance

namespace {
void disableWiFiBeforeDeepSleep() {
  const wifi_mode_t wifiMode = WiFi.getMode();
  if (wifiMode == WIFI_MODE_NULL) {
    return;
  }

  LOG_DBG("PWR", "Disabling WiFi before deep sleep (mode=%d)", static_cast<int>(wifiMode));
  if (wifiMode & WIFI_MODE_AP) {
    WiFi.softAPdisconnect(true);
  }
  if (wifiMode & WIFI_MODE_STA) {
    WiFi.disconnect(true);
  }
  delay(30);
  WiFi.mode(WIFI_OFF);
  delay(30);
}

// Little-endian 16-bit register read from BQ27220 (shared I2C bus with RTC/IMU).
bool readBq27220Reg16(const uint8_t reg, uint16_t& out) {
  Wire.beginTransmission(I2C_ADDR_BQ27220);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }
  if (Wire.requestFrom(static_cast<uint8_t>(I2C_ADDR_BQ27220), static_cast<uint8_t>(2),
                       static_cast<uint8_t>(true)) < 2) {
    while (Wire.available()) {
      (void)Wire.read();
    }
    return false;
  }
  const uint8_t lo = Wire.read();
  const uint8_t hi = Wire.read();
  out = static_cast<uint16_t>(lo) | (static_cast<uint16_t>(hi) << 8);
  return true;
}

// Reconcile fuel-gauge SoC with cell voltage. Unlearned / stuck BQ27220 units often
// report 100% for a long time while the cell is clearly not full (~4.2 V).
uint16_t reconcileSocWithVoltage(const bool socOk, uint16_t soc, const bool mvOk, const uint16_t mv,
                                 const bool maOk, const int16_t ma) {
  if (socOk && soc > 100) {
    soc = 100;
  }

  uint16_t fromMv = 0;
  const bool voltagePlausible = mvOk && mv >= 2500 && mv <= 5000;
  if (voltagePlausible) {
    fromMv = BatteryMonitor::percentageFromMillivolts(mv);
  }

  if (!socOk && voltagePlausible) {
    return fromMv;
  }
  if (socOk && !voltagePlausible) {
    return soc;
  }
  if (!socOk && !voltagePlausible) {
    return 0;
  }

  // Both SoC and voltage available.
  uint16_t reported = soc;

  // Under charge (current into cell), open-circuit voltage mapping is less reliable — keep SoC
  // unless voltage is wildly low for a "full" reading.
  const bool charging = maOk && ma > 20;

  if (soc >= 95 && mv < 4000) {
    // Classic stuck-at-full: SoC near 100% but cell well below full charge voltage.
    reported = fromMv;
  } else if (soc >= 90 && mv < 3900) {
    reported = static_cast<uint16_t>((static_cast<uint32_t>(fromMv) * 2 + soc) / 3);
  } else if (soc + 25 < fromMv && !charging) {
    // SoC lagging voltage (e.g. after charge, unlearned) — ease up.
    reported = static_cast<uint16_t>((static_cast<uint32_t>(fromMv) + soc) / 2);
  } else if (fromMv + 25 < soc && mv < 4050 && !charging) {
    // SoC much higher than voltage implies — pull down toward voltage.
    reported = static_cast<uint16_t>((static_cast<uint32_t>(fromMv) * 2 + soc) / 3);
  }

  if (reported > 100) {
    reported = 100;
  }
  return reported;
}
}  // namespace

void HalPowerManager::begin() {
  if (gpio.deviceIsX3()) {
    // X3 uses an I2C fuel gauge for battery monitoring.
    // I2C init must come AFTER gpio.begin() so early hardware detection/probes are finished.
    Wire.begin(X3_I2C_SDA, X3_I2C_SCL, X3_I2C_FREQ);
    // Shared bus (gauge + RTC + IMU): 4 ms was too short and caused sticky last-% on timeouts.
    Wire.setTimeOut(X3_I2C_TIMEOUT_MS);
    _batteryUseI2C = true;
    LOG_INF("BAT", "X3 battery: BQ27220 gauge (I2C timeout %u ms)", static_cast<unsigned>(X3_I2C_TIMEOUT_MS));
  } else {
    pinMode(BAT_GPIO0, INPUT);
  }
  normalFreq = getCpuFrequencyMhz();
  modeMutex = xSemaphoreCreateMutex();
  assert(modeMutex != nullptr);
}

void HalPowerManager::setPowerSaving(bool enabled) {
  if (normalFreq <= 0) {
    return;  // invalid state
  }

  auto wifiMode = WiFi.getMode();
  if (wifiMode != WIFI_MODE_NULL) {
    // Wifi is active, force disabling power saving
    enabled = false;
  }

  // Note: We don't use mutex here to avoid too much overhead,
  // it's not very important if we read a slightly stale value for currentLockMode
  const LockMode mode = currentLockMode;

  if (mode == None && enabled && !isLowPower) {
    LOG_DBG("PWR", "Going to low-power mode");
    if (!setCpuFrequencyMhz(LOW_POWER_FREQ)) {
      LOG_DBG("PWR", "Failed to set CPU frequency = %d MHz", LOW_POWER_FREQ);
      return;
    }
    isLowPower = true;

  } else if ((!enabled || mode != None) && isLowPower) {
    LOG_DBG("PWR", "Restoring normal CPU frequency");
    if (!setCpuFrequencyMhz(normalFreq)) {
      LOG_DBG("PWR", "Failed to set CPU frequency = %d MHz", normalFreq);
      return;
    }
    isLowPower = false;
  }

  // Otherwise, no change needed
}

void HalPowerManager::startDeepSleep(HalGPIO& gpio) const {
  disableWiFiBeforeDeepSleep();

  // Ensure that the power button has been released to avoid immediately turning back on if you're holding it
  while (gpio.isPressed(HalGPIO::BTN_POWER)) {
    delay(50);
    gpio.update();
  }

#ifdef ENABLE_SERIAL_LOG
  // Tear down HWCDC so the host sees a clean disconnect and the peripheral
  // doesn't hold power domains that interfere with USB-powered GPIO wake.
  // logSerial is the raw HWCDC reference; Serial is the MySerialImpl proxy
  // (which doesn't expose end()).
  logSerial.end();
#endif

  // Pre-sleep routines from the original firmware
  // GPIO13 is connected to battery latch MOSFET, we need to make sure it's low during sleep
  // Note that this means the MCU will be completely powered off during sleep, including RTC
  constexpr gpio_num_t GPIO_SPIWP = GPIO_NUM_13;
  gpio_set_direction(GPIO_SPIWP, GPIO_MODE_OUTPUT);
  gpio_set_level(GPIO_SPIWP, 0);
  esp_sleep_config_gpio_isolate();
  gpio_deep_sleep_hold_en();
  gpio_hold_en(GPIO_SPIWP);
  pinMode(InputManager::POWER_BUTTON_PIN, INPUT_PULLUP);
  // Arm the wakeup trigger *after* the button is released
  // Note: this is only useful for waking up on USB power. On battery, the MCU will be completely powered off, so the
  // power button is hard-wired to briefly provide power to the MCU, waking it up regardless of the wakeup source
  // configuration
  esp_deep_sleep_enable_gpio_wakeup(1ULL << InputManager::POWER_BUTTON_PIN, ESP_GPIO_WAKEUP_GPIO_LOW);
  // Enter Deep Sleep
  esp_deep_sleep_start();
}

uint16_t HalPowerManager::readX3BatteryPercentage() const {
  const unsigned long now = millis();
  if (_batteryLastPollMs != 0 && (now - _batteryLastPollMs) < BATTERY_POLL_MS) {
    return static_cast<uint16_t>((_batteryCachedPercent + 5) / 10);
  }

  uint16_t socRaw = 0;
  uint16_t mv = 0;
  uint16_t maRaw = 0;
  const bool socOk = readBq27220Reg16(BQ27220_SOC_REG, socRaw);
  const bool mvOk = readBq27220Reg16(BQ27220_VOLT_REG, mv);
  const bool maOk = readBq27220Reg16(BQ27220_CUR_REG, maRaw);
  const int16_t ma = static_cast<int16_t>(maRaw);

  if (!socOk && !mvOk) {
    ++_batteryI2cFailStreak;
    _batteryLastPollMs = now;
    if (_batteryI2cFailStreak == 1 || (_batteryI2cFailStreak % 30) == 0) {
      LOG_ERR("BAT", "BQ27220 I2C read failed (streak=%u); keeping last %%",
              static_cast<unsigned>(_batteryI2cFailStreak));
    }
    // Prefer last good display value; 0 only if we never had one.
    if (_batteryCachedPercent > 0) {
      return static_cast<uint16_t>((_batteryCachedPercent + 5) / 10);
    }
    return 0;
  }
  _batteryI2cFailStreak = 0;

  const uint16_t soc = socOk ? (socRaw > 100 ? 100 : socRaw) : 0;
  const uint16_t reported = reconcileSocWithVoltage(socOk, soc, mvOk, mv, maOk, ma);

  // Log first sample and periodic diagnostics (helps field-debug stuck 100%).
  const bool shouldLog = !_batteryLoggedOnce || (now - _batteryLastLogMs) >= BATTERY_LOG_INTERVAL_MS ||
                         (socOk && mvOk && soc >= 90 && mv < 4000) || (socOk && mvOk && std::abs(static_cast<int>(soc) -
                                                                                                static_cast<int>(
                                                                                                    BatteryMonitor::percentageFromMillivolts(
                                                                                                        mv))) > 20);
  if (shouldLog) {
    _batteryLoggedOnce = true;
    _batteryLastLogMs = now;
    const uint16_t fromMv = (mvOk && mv >= 2500 && mv <= 5000) ? BatteryMonitor::percentageFromMillivolts(mv) : 0;
    LOG_INF("BAT", "gauge SoC=%u%% V=%umV I=%dmA Vmap=%u%% -> show=%u%%", static_cast<unsigned>(socOk ? soc : 0),
            static_cast<unsigned>(mvOk ? mv : 0), maOk ? static_cast<int>(ma) : 0, static_cast<unsigned>(fromMv),
            static_cast<unsigned>(reported));
  }

  // Smooth in tenths of a percent. Allow faster fall when voltage correction pulls SoC down hard
  // (stuck-at-100 recovery); climb slowly so brief spikes don't jump the UI.
  const int newTenths = static_cast<int>(reported) * 10;
  if (_batteryCachedPercent <= 0) {
    _batteryCachedPercent = newTenths;
  } else {
    const int delta = newTenths - _batteryCachedPercent;
    if (delta <= -80) {
      // Drop of 8%+ this sample (correction) — catch down quickly.
      _batteryCachedPercent = (_batteryCachedPercent * 4 + newTenths * 6) / 10;
    } else if (delta >= 50) {
      // Climb of 5%+ — ease up (charge / noise).
      _batteryCachedPercent = (_batteryCachedPercent * 8 + newTenths * 2) / 10;
    } else {
      _batteryCachedPercent = (_batteryCachedPercent * 7 + newTenths * 3) / 10;
    }
  }
  if (_batteryCachedPercent < 0) {
    _batteryCachedPercent = 0;
  }
  if (_batteryCachedPercent > 1000) {
    _batteryCachedPercent = 1000;
  }

  _batteryLastPollMs = now;
  return static_cast<uint16_t>((_batteryCachedPercent + 5) / 10);
}

uint16_t HalPowerManager::getBatteryPercentage() const {
  if (_batteryUseI2C) {
    return readX3BatteryPercentage();
  }
  static const BatteryMonitor battery = BatteryMonitor(BAT_GPIO0);

  // Smooth the battery % (tenths).
  const int sample = static_cast<int>(battery.readPercentage()) * 10;
  if (_batteryCachedPercent <= 0) {
    _batteryCachedPercent = sample;
  } else {
    _batteryCachedPercent = (_batteryCachedPercent * 9 + sample) / 10;
  }
  return static_cast<uint16_t>((_batteryCachedPercent + 5) / 10);
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
