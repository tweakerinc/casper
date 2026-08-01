#include <BoardConfig.h>
#include <HalGPIO.h>
#include <Logging.h>
#include <PowerManager.h>
#include <Preferences.h>
#include <SPI.h>
#include <Wire.h>
#include <XteinkDetect.h>
#include <esp_sleep.h>

// Global HalGPIO instance
HalGPIO gpio;

namespace X3GPIO {

bool readI2CReg16LE(uint8_t addr, uint8_t reg, uint16_t* outValue) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }
  if (Wire.requestFrom(addr, static_cast<uint8_t>(2), static_cast<uint8_t>(true)) < 2) {
    while (Wire.available()) {
      Wire.read();
    }
    return false;
  }
  const uint8_t lo = Wire.read();
  const uint8_t hi = Wire.read();
  *outValue = (static_cast<uint16_t>(hi) << 8) | lo;
  return true;
}

bool readBQ27220CurrentMA(int16_t* outCurrent) {
  uint16_t raw = 0;
  if (!readI2CReg16LE(I2C_ADDR_BQ27220, BQ27220_CUR_REG, &raw)) {
    return false;
  }
  *outCurrent = static_cast<int16_t>(raw);
  return true;
}

}  // namespace X3GPIO

namespace {
constexpr char HW_NAMESPACE[] = "cphw";
constexpr char NVS_KEY_DEV_OVERRIDE[] = "dev_ovr";  // 0=auto, 1=x4, 2=x3
constexpr char NVS_KEY_DEV_CACHED[] = "dev_det";    // 0=unknown, 1=x4, 2=x3

enum class NvsDeviceValue : uint8_t { Unknown = 0, X4 = 1, X3 = 2 };

NvsDeviceValue readNvsDeviceValue(const char* key, NvsDeviceValue defaultValue) {
  Preferences prefs;
  if (!prefs.begin(HW_NAMESPACE, true)) {
    return defaultValue;
  }
  const uint8_t raw = prefs.getUChar(key, static_cast<uint8_t>(defaultValue));
  prefs.end();
  if (raw > static_cast<uint8_t>(NvsDeviceValue::X3)) {
    return defaultValue;
  }
  return static_cast<NvsDeviceValue>(raw);
}

void writeNvsDeviceValue(const char* key, NvsDeviceValue value) {
  Preferences prefs;
  if (!prefs.begin(HW_NAMESPACE, false)) {
    return;
  }
  prefs.putUChar(key, static_cast<uint8_t>(value));
  prefs.end();
}

HalGPIO::DeviceType nvsToDeviceType(NvsDeviceValue value) {
  return value == NvsDeviceValue::X3 ? HalGPIO::DeviceType::X3 : HalGPIO::DeviceType::X4;
}

HalGPIO::DeviceType detectDeviceTypeWithFingerprint() {
  // Explicit override for recovery/support:
  // 0 = auto, 1 = force X4, 2 = force X3
  const NvsDeviceValue overrideValue = readNvsDeviceValue(NVS_KEY_DEV_OVERRIDE, NvsDeviceValue::Unknown);
  if (overrideValue == NvsDeviceValue::X3 || overrideValue == NvsDeviceValue::X4) {
    LOG_INF("HW", "Device override active: %s", overrideValue == NvsDeviceValue::X3 ? "X3" : "X4");
    return nvsToDeviceType(overrideValue);
  }

  const NvsDeviceValue cachedValue = readNvsDeviceValue(NVS_KEY_DEV_CACHED, NvsDeviceValue::Unknown);
  if (cachedValue == NvsDeviceValue::X3 || cachedValue == NvsDeviceValue::X4) {
    LOG_INF("HW", "Using cached device type: %s", cachedValue == NvsDeviceValue::X3 ? "X3" : "X4");
    return nvsToDeviceType(cachedValue);
  }

  // X3 vs X4 I2C fingerprint only. Display-controller sibling (UC8253 vs UC8279
  // / SSD1677 vs UC8179) is resolved in begin() via the live bus probe.
  const bool isX3 = freeink::detectXteinkIsX3();
  LOG_INF("HW", "Xteink I2C probe: isX3=%d", isX3 ? 1 : 0);
  writeNvsDeviceValue(NVS_KEY_DEV_CACHED, isX3 ? NvsDeviceValue::X3 : NvsDeviceValue::X4);
  return isX3 ? HalGPIO::DeviceType::X3 : HalGPIO::DeviceType::X4;
}

const char* displayControllerName(BoardConfig::DisplayController c) {
  switch (c) {
    case BoardConfig::DisplayController::SSD1677:
      return "SSD1677";
    case BoardConfig::DisplayController::UC8253:
      return "UC8253";
    case BoardConfig::DisplayController::UC8279:
      return "UC8279";
    case BoardConfig::DisplayController::UC8179:
      return "UC8179";
    case BoardConfig::DisplayController::ED2208:
      return "ED2208";
    case BoardConfig::DisplayController::LgfxEpd:
      return "LgfxEpd";
    case BoardConfig::DisplayController::IT8951:
      return "IT8951";
    default:
      return "unknown";
  }
}

// Pick board profile + UltraChip sibling when the bus probe confirms it.
// Logs VER/FLG so serial boot can tell old UC8253 glass from new UC8279 glass.
void selectBoardAndPanelController(bool isX3) {
  if (isX3) {
    BoardConfig::selectDevice(BoardConfig::Board::XteinkX3);  // default UC8253
    uint8_t ver[5] = {};
    uint8_t flg = 0;
    const freeink::X3DisplayVerdict v = freeink::detectX3DisplayController(ver, &flg);
    LOG_INF("HW", "X3 panel probe VER=%02X %02X %02X %02X %02X FLG=%02X verdict=%u", ver[0], ver[1], ver[2],
            ver[3], ver[4], flg, static_cast<unsigned>(v));
    if (v == freeink::X3DisplayVerdict::Uc8279Confirmed) {
      BoardConfig::selectDevice(BoardConfig::Board::XteinkX3Uc8279);
      LOG_INF("HW", "promoted UC8253 -> UC8279 (new-batch panel)");
    } else {
      LOG_INF("HW", "panel controller UC8253 (classic / probe not UC8279)");
    }
  } else {
    BoardConfig::selectDevice(BoardConfig::Board::XteinkX4);  // default SSD1677
    if (freeink::applyXteinkDisplayController()) {
      LOG_INF("HW", "promoted SSD1677 -> UC8179 (new-batch panel)");
    } else {
      LOG_INF("HW", "panel controller SSD1677 (classic / probe not UC8179)");
    }
  }
}

}  // namespace

void HalGPIO::begin() {
#if FREEINK_MCU_C3
#ifdef FORCE_DEVICE_X3
  _deviceType = DeviceType::X3;
  LOG_INF("HW", "Device override active via build flag: X3");
#else
  _deviceType = detectDeviceTypeWithFingerprint();
#endif
  selectBoardAndPanelController(deviceIsX3());
  LOG_INF("HW", "Board=%s displayController=%u (%s)", BoardConfig::ACTIVE.name,
          static_cast<unsigned>(BoardConfig::ACTIVE.displayController),
          displayControllerName(BoardConfig::ACTIVE.displayController));

  SPI.begin(EPD_SCLK, SPI_MISO, EPD_MOSI, EPD_CS);

  if (deviceIsX4()) {
    pinMode(BAT_GPIO0, INPUT);
    pinMode(UART0_RXD, INPUT);
  }
#else
  _deviceType = DeviceType::X4;
#endif
  inputMgr.begin();
}

void HalGPIO::update() {
  inputMgr.update();
  const bool connected = isUsbConnected();
  usbStateChanged = (connected != lastUsbConnected);
  lastUsbConnected = connected;
}

bool HalGPIO::wasUsbStateChanged() const { return usbStateChanged; }

bool HalGPIO::isPressed(uint8_t buttonIndex) const { return inputMgr.isPressed(buttonIndex); }

bool HalGPIO::wasPressed(uint8_t buttonIndex) const { return inputMgr.wasPressed(buttonIndex); }

bool HalGPIO::wasAnyPressed() const { return inputMgr.wasAnyPressed(); }

bool HalGPIO::wasReleased(uint8_t buttonIndex) const { return inputMgr.wasReleased(buttonIndex); }

bool HalGPIO::wasAnyReleased() const { return inputMgr.wasAnyReleased(); }

unsigned long HalGPIO::getHeldTime() const { return inputMgr.getHeldTime(); }

unsigned long HalGPIO::getPowerButtonHeldTime() const { return inputMgr.getPowerButtonHeldTime(); }

bool HalGPIO::hasTouch() const { return inputMgr.hasTouch(); }

bool HalGPIO::wasTouchTap(float& nx, float& ny) const { return inputMgr.wasTouchTap(nx, ny); }

bool HalGPIO::wasTouchDown(float& nx, float& ny) const { return inputMgr.wasTouchPressedAt(nx, ny); }

bool HalGPIO::isTouchTapCandidate(float& nx, float& ny, unsigned long& heldMs) const {
  return inputMgr.isTouchTapCandidate(nx, ny, heldMs);
}

bool HalGPIO::isTouchHeldAt(float& nx, float& ny) const { return inputMgr.isTouchHeldAt(nx, ny); }

unsigned long HalGPIO::lastTouchHeldMs() const { return inputMgr.lastTouchHeldMs(); }

bool HalGPIO::wasSwipe(float& nxStart, float& nyStart, float& nxEnd, float& nyEnd) const {
  return inputMgr.wasSwipe(nxStart, nyStart, nxEnd, nyEnd);
}

bool HalGPIO::wasTouchActivity() const { return inputMgr.wasTouchActivity(); }

void HalGPIO::setSharedConfirmPowerShortPressEmitsPower(const bool enabled) {
  InputManager::setSharedConfirmPowerShortPressEmitsPower(enabled);
}

bool HalGPIO::isXteinkDevice() const {
  return BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX3 ||
         BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX3Uc8279 ||
         BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX4;
}

bool HalGPIO::verifyPowerButtonWakeup(uint16_t requiredDurationMs, bool shortPressAllowed) {
  // Boards without a power button (or M5Paper's latch circuit) cannot verify a
  // hold; treat the wake as valid.
  if (BoardConfig::ACTIVE.input.power < 0) {
    return true;
  }
#if defined(FREEINK_DEVICE_M5PAPER) && FREEINK_DEVICE_M5PAPER
  return true;
#endif
  if (shortPressAllowed) {
    // Fast path - no duration check needed
    return true;
  }
  // TODO: Intermittent edge case remains: a single tap followed by another single tap
  // can still power on the device. Tighten wake debounce/state handling here.

  // Calibrate: subtract boot time already elapsed, assuming button held since boot.
  const unsigned long calibration = millis();
  const unsigned long calibratedDuration = (calibration < requiredDurationMs) ? (requiredDurationMs - calibration) : 1;

  const auto start = millis();
  inputMgr.update();
  // inputMgr.isPressed() may take up to ~500ms to return correct state
  while (!inputMgr.isPressed(BTN_POWER) && millis() - start < 1000) {
    delay(10);
    inputMgr.update();
  }
  if (inputMgr.isPressed(BTN_POWER)) {
    do {
      delay(10);
      inputMgr.update();
    } while (inputMgr.isPressed(BTN_POWER) && inputMgr.getPowerButtonHeldTime() < calibratedDuration);
    if (inputMgr.getPowerButtonHeldTime() < calibratedDuration) {
      return false;
    }
  } else {
    return false;
  }
  return true;
}

bool HalGPIO::isUsbConnected() const {
  if (deviceIsX3()) {
    // X3: infer USB/charging via BQ27220 Current() register (0x0C, signed mA).
    // Positive current means charging.
    for (uint8_t attempt = 0; attempt < 2; ++attempt) {
      int16_t currentMa = 0;
      if (X3GPIO::readBQ27220CurrentMA(&currentMa)) {
        return currentMa > 0;
      }
      delay(2);
    }
    return false;
  }
  if (BoardConfig::ACTIVE.usbDetect < 0) {
    return false;
  }
  return digitalRead(BoardConfig::ACTIVE.usbDetect) == HIGH;
}

HalGPIO::WakeupReason HalGPIO::getWakeupReason() const {
  const auto wakeupCause = esp_sleep_get_wakeup_cause();
  const auto resetReason = esp_reset_reason();

  const bool usbConnected = isUsbConnected();

  // Real deep-sleep exit (X3, and X4 while on USB — latch stays powered).
  if (resetReason == ESP_RST_DEEPSLEEP &&
      (wakeupCause == ESP_SLEEP_WAKEUP_GPIO || wakeupCause == ESP_SLEEP_WAKEUP_EXT1)) {
    return WakeupReason::PowerButton;
  }
  // X4 on battery: GPIO13 power latch cuts MCU power in sleep, so wake is a
  // cold ESP_RST_POWERON (not DEEPSLEEP). Without USB that is still a power-
  // button wake and must QuickResume like v0.1.3 — not Splash/"reboot".
  // Flash/USB paths below keep sticky lastSleepFromReader from auto-opening a book.
  if (wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED && resetReason == ESP_RST_POWERON && !usbConnected) {
    return WakeupReason::PowerButton;
  }
#ifdef ESP_RST_USB
  // USB-Serial/JTAG download reset (common after esptool on ESP32-C3).
  if (resetReason == ESP_RST_USB) {
    return WakeupReason::AfterFlash;
  }
#endif
  if (usbConnected && wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED &&
      (resetReason == ESP_RST_UNKNOWN || resetReason == ESP_RST_SW)) {
    return WakeupReason::AfterFlash;
  }
  if (wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED && resetReason == ESP_RST_POWERON && usbConnected) {
    return WakeupReason::AfterUSBPower;
  }
  // Brownout recovery, unknown, etc. → cold boot (not sleep wake).
  return WakeupReason::Other;
}
