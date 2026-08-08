#include "BluetoothSettingsActivity.h"

#include <BleKeyboardHost.h>
#include <BoardConfig.h>
#include <GfxRenderer.h>
#include <HalPowerManager.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "CrossPointSettings.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/UiGhostPolicy.h"

#if !FREEINK_CAP_BLE_HID_HOST
// Stubs still compile so callers need no #ifdefs; UI shows unavailable.
#endif

namespace {
constexpr unsigned long kConnectTimeoutMs = 20000;
// Long enough for active-scan responses (names often arrive only on the 2nd PDU).
constexpr unsigned long kScanDurationMs = 10000;
constexpr unsigned long kScanAnimMs = 600;

// Prefer advertised name; fall back to a short MAC suffix so the list isn't
// a wall of full addresses when a peripheral never sends a local name.
std::string bleDeviceLabel(const freeink::DiscoveredDevice& d) {
  if (d.hasName && d.name[0] && strcmp(d.name, d.addr) != 0) {
    return std::string(d.name);
  }
  if (d.name[0] && strcmp(d.name, d.addr) != 0) {
    return std::string(d.name);
  }
  // Compact address: last 3 octets (e.g. "…:DD:EE:FF")
  const char* a = d.addr;
  const size_t n = strlen(a);
  if (n >= 8) {
    return std::string("…") + (a + n - 8);
  }
  return std::string(a);
}
}  // namespace

BluetoothSettingsActivity::BluetoothSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                     const bool exitOnSuccessfulConnect, const bool disableOnExit)
    : Activity("BluetoothSettings", renderer, mappedInput),
      exitOnSuccessfulConnect_(exitOnSuccessfulConnect),
      disableOnExit_(disableOnExit) {}

void BluetoothSettingsActivity::setBanner(const char* text, const unsigned long durationMs) {
  banner_ = text ? text : "";
  bannerUntil_ = millis() + durationMs;
}

bool BluetoothSettingsActivity::ensureBleStarted() {
#if FREEINK_CAP_BLE_HID_HOST
  if (BleHid.isRunning()) return true;

  // Full CPU for the enable path (BT controller init is CPU-freq sensitive and
  // stack-deep — must not run under the 10 MHz idle clock).
  HalPowerManager::Lock powerLock;
  setCpuFrequencyMhz(160);

  // Shared radio: park WiFi before bringing up the controller.
  if (WiFi.getMode() != WIFI_OFF) {
    WiFi.disconnect(false);
    WiFi.mode(WIFI_OFF);
    delay(50);
  }

  LOG_INF("BT", "calling BleHid.begin heap=%u maxAlloc=%u", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  if (!BleHid.begin("Casper")) {
    LOG_ERR("BT", "BleHid.begin failed heap=%u", ESP.getFreeHeap());
    setBanner(tr(STR_BT_UNAVAILABLE));
    return false;
  }
  LOG_INF("BT", "BleHid.begin ok heap=%u maxAlloc=%u", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  return true;
#else
  setBanner(tr(STR_BT_UNAVAILABLE));
  return false;
#endif
}

void BluetoothSettingsActivity::onEnter() {
  Activity::onEnter();
  view_ = View::Menu;
  menuIndex_ = 0;
  scanIndex_ = 0;
  pairedIndex_ = 0;
  awaitingConnect_ = false;
  banner_.clear();

  LOG_INF("BT", "enter heap=%u maxAlloc=%u quick=%d cpu=%u", ESP.getFreeHeap(), ESP.getMaxAllocHeap(),
          exitOnSuccessfulConnect_ ? 1 : 0, static_cast<unsigned>(getCpuFrequencyMhz()));

#if FREEINK_CAP_BLE_HID_HOST
  // Settings / Home: stay OFF until the user toggles "Bluetooth" (clearer UX
  // than auto-enabling and showing "Disable Bluetooth").
  // Reader "Connect Remote": bring the stack up and scan/pair immediately.
  if (exitOnSuccessfulConnect_) {
    if (!ensureBleStarted()) {
      rebuildMenuRows();
      requestUpdate();
      return;
    }
    if (BleHid.isConnected()) {
      finish();
      return;
    }
    if (BleHid.pairedCount() > 0) {
      tryQuickConnect();
    } else {
      startScanView();
    }
  }
#else
  setBanner(tr(STR_BT_UNAVAILABLE));
#endif

  rebuildMenuRows();
  requestUpdate();
}

void BluetoothSettingsActivity::onExit() {
  Activity::onExit();
#if FREEINK_CAP_BLE_HID_HOST
  BleHid.stopScan();
  if (disableOnExit_ && !BleHid.isConnected()) {
    // Tear down NimBLE when leaving Settings so Home keeps contiguous heap.
    BleHid.end();
  }
#endif
}

void BluetoothSettingsActivity::rebuildMenuRows() {
  menuRows_.clear();
#if FREEINK_CAP_BLE_HID_HOST
  const bool running = BleHid.isRunning();
  const bool connected = BleHid.isConnected();
  // Label is always "Bluetooth"; ON/OFF is drawn as the row value (toggle chip).
  menuRows_.emplace_back(Action::ToggleBt, StrId::STR_BLUETOOTH);
  // Scan is always listed — selecting it brings the radio up (battery-friendly
  // default-off without an extra toggle step before every scan).
  menuRows_.emplace_back(Action::Scan, StrId::STR_BT_SCAN);
  if (running) {
    if (BleHid.pairedCount() > 0 && !connected) {
      menuRows_.emplace_back(Action::QuickConnect, StrId::STR_BT_QUICK_CONNECT);
    }
    if (connected) {
      menuRows_.emplace_back(Action::Disconnect, StrId::STR_BT_DISCONNECT);
    }
    if (BleHid.pairedCount() > 0) {
      menuRows_.emplace_back(Action::PairedDevices, StrId::STR_BT_PAIRED);
      menuRows_.emplace_back(Action::Forget, StrId::STR_BT_FORGET);
    }
  }
#else
  menuRows_.emplace_back(Action::ToggleBt, StrId::STR_BT_UNAVAILABLE);
#endif
  if (menuIndex_ >= static_cast<int>(menuRows_.size())) {
    menuIndex_ = std::max(0, static_cast<int>(menuRows_.size()) - 1);
  }
}

void BluetoothSettingsActivity::tryQuickConnect() {
#if FREEINK_CAP_BLE_HID_HOST
  if (BleHid.pairedCount() == 0) {
    setBanner(tr(STR_BT_NO_PAIRED));
    return;
  }
  const auto& p = BleHid.paired(0);
  pendingConnectAddr_ = p.addr;
  awaitingConnect_ = true;
  awaitingConnectStartedAt_ = millis();
  setBanner(tr(STR_BT_CONNECTING));
  if (!BleHid.connect(p.addr)) {
    awaitingConnect_ = false;
    setBanner(tr(STR_BT_CONNECT_FAILED));
  }
  requestUpdate();
#endif
}

void BluetoothSettingsActivity::startScanView() {
#if FREEINK_CAP_BLE_HID_HOST
  if (!ensureBleStarted()) return;
  view_ = View::Scan;
  scanIndex_ = 0;
  lastScanAnimMs_ = 0;
  BleHid.startScan(kScanDurationMs);
  setBanner(tr(STR_SCANNING));
  requestUpdate();
#endif
}

void BluetoothSettingsActivity::handleMenuConfirm() {
  if (menuRows_.empty()) return;
  const Action action = menuRows_[static_cast<size_t>(menuIndex_)].action;

#if FREEINK_CAP_BLE_HID_HOST
  switch (action) {
    case Action::ToggleBt:
      if (BleHid.isRunning()) {
        BleHid.end();
        setBanner(tr(STR_BT_DISABLED));
      } else if (ensureBleStarted()) {
        setBanner(tr(STR_BT_ENABLED));
      }
      rebuildMenuRows();
      requestUpdate();
      break;
    case Action::Scan:
      // Scan turns the radio on if needed (no separate Enable step).
      if (!ensureBleStarted()) {
        rebuildMenuRows();
        requestUpdate();
        break;
      }
      rebuildMenuRows();  // pick up ON + any paired rows after auto-enable
      startScanView();
      break;
    case Action::QuickConnect:
      tryQuickConnect();
      break;
    case Action::Disconnect:
      BleHid.disconnect();
      setBanner(tr(STR_BT_DISCONNECTED));
      rebuildMenuRows();
      requestUpdate();
      break;
    case Action::PairedDevices:
      view_ = View::Paired;
      pairedIndex_ = 0;
      requestUpdate();
      break;
    case Action::Forget:
      if (BleHid.pairedCount() > 0) {
        char addr[18];
        strlcpy(addr, BleHid.paired(0).addr, sizeof(addr));
        BleHid.forget(addr);
        setBanner(tr(STR_BT_FORGOTTEN));
        rebuildMenuRows();
        requestUpdate();
      }
      break;
  }
#else
  (void)action;
  setBanner(tr(STR_BT_UNAVAILABLE));
  requestUpdate();
#endif
}

void BluetoothSettingsActivity::pollConnectResult() {
#if FREEINK_CAP_BLE_HID_HOST
  if (!awaitingConnect_) return;

  if (BleHid.isConnected()) {
    awaitingConnect_ = false;
    BleHid.releaseScanResults();
    setBanner(tr(STR_BT_CONNECTED));
    if (exitOnSuccessfulConnect_) {
      requestUpdateAndWait();
      delay(400);
      finish();
      return;
    }
    view_ = View::Menu;
    rebuildMenuRows();
    requestUpdate();
    return;
  }

  char fail[48];
  if (BleHid.takeConnectFailure(fail, sizeof(fail))) {
    awaitingConnect_ = false;
    setBanner(tr(STR_BT_CONNECT_FAILED));
    view_ = View::Menu;
    rebuildMenuRows();
    requestUpdate();
    return;
  }

  if (millis() - awaitingConnectStartedAt_ > kConnectTimeoutMs) {
    awaitingConnect_ = false;
    BleHid.disconnect();
    setBanner(tr(STR_BT_CONNECT_FAILED));
    view_ = View::Menu;
    rebuildMenuRows();
    requestUpdate();
  }
#endif
}

void BluetoothSettingsActivity::loop() {
#if FREEINK_CAP_BLE_HID_HOST
  BleHid.poll();
#endif
  pollConnectResult();

  if (bannerUntil_ != 0 && millis() > bannerUntil_) {
    bannerUntil_ = 0;
    banner_.clear();
    requestUpdate();
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    if (view_ != View::Menu) {
#if FREEINK_CAP_BLE_HID_HOST
      BleHid.stopScan();
#endif
      view_ = View::Menu;
      rebuildMenuRows();
      requestUpdate();
      return;
    }
    finish();
    return;
  }

  if (view_ == View::Menu) {
    const int count = static_cast<int>(menuRows_.size());
    if (count <= 0) return;
    if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
        mappedInput.wasPressed(MappedInputManager::Button::Right)) {
      menuIndex_ = (menuIndex_ + 1) % count;
      requestUpdate();
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
               mappedInput.wasPressed(MappedInputManager::Button::Left)) {
      menuIndex_ = (menuIndex_ - 1 + count) % count;
      requestUpdate();
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      handleMenuConfirm();
    }
    return;
  }

#if FREEINK_CAP_BLE_HID_HOST
  if (view_ == View::Scan) {
    // Repaint while scanning so name upgrades (scan-response) replace MAC stubs.
    if (BleHid.isScanning() && millis() - lastScanAnimMs_ > kScanAnimMs) {
      lastScanAnimMs_ = millis();
      requestUpdate();
    } else if (!BleHid.isScanning() && lastScanAnimMs_ != 0) {
      // Final frame once the window ends (pick up late name upgrades).
      lastScanAnimMs_ = 0;
      requestUpdate();
    }
    const int count = static_cast<int>(BleHid.deviceCount());
    if (count > 0) {
      if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
          mappedInput.wasPressed(MappedInputManager::Button::Right)) {
        scanIndex_ = (scanIndex_ + 1) % count;
        requestUpdate();
      } else if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
                 mappedInput.wasPressed(MappedInputManager::Button::Left)) {
        scanIndex_ = (scanIndex_ - 1 + count) % count;
        requestUpdate();
      } else if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
        const auto& d = BleHid.device(static_cast<uint8_t>(scanIndex_));
        // Copy address before clearing scan bookkeeping (frees NimBLE adv RAM
        // so GATT discovery does not OOM/abort on the C3).
        char addrCopy[18];
        strlcpy(addrCopy, d.addr, sizeof(addrCopy));
        pendingConnectAddr_ = addrCopy;
        awaitingConnect_ = true;
        awaitingConnectStartedAt_ = millis();
        BleHid.stopScan();
        setBanner(tr(STR_BT_CONNECTING));
        if (!BleHid.connect(addrCopy)) {
          awaitingConnect_ = false;
          setBanner(tr(STR_BT_CONNECT_FAILED));
        }
        requestUpdate();
      }
    } else if (!BleHid.isScanning() && mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      BleHid.startScan(kScanDurationMs);
      setBanner(tr(STR_SCANNING));
      requestUpdate();
    }
    return;
  }

  if (view_ == View::Paired) {
    const int count = static_cast<int>(BleHid.pairedCount());
    if (count <= 0) {
      view_ = View::Menu;
      rebuildMenuRows();
      requestUpdate();
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
        mappedInput.wasPressed(MappedInputManager::Button::Right)) {
      pairedIndex_ = (pairedIndex_ + 1) % count;
      requestUpdate();
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
               mappedInput.wasPressed(MappedInputManager::Button::Left)) {
      pairedIndex_ = (pairedIndex_ - 1 + count) % count;
      requestUpdate();
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      const auto& p = BleHid.paired(static_cast<uint8_t>(pairedIndex_));
      pendingConnectAddr_ = p.addr;
      awaitingConnect_ = true;
      awaitingConnectStartedAt_ = millis();
      setBanner(tr(STR_BT_CONNECTING));
      if (!BleHid.connect(p.addr)) {
        awaitingConnect_ = false;
        setBanner(tr(STR_BT_CONNECT_FAILED));
      }
      requestUpdate();
    }
  }
#endif
}

void BluetoothSettingsActivity::renderMenu() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_BLUETOOTH));

  const int listTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int listHeight = renderer.getScreenHeight() - listTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;

  // highlightValue=true: black selection chip sits on ON/OFF (toggle), not the label.
  GUI.drawList(
      renderer, Rect{0, listTop, pageWidth, listHeight}, static_cast<int>(menuRows_.size()), menuIndex_,
      [this](int i) { return std::string(I18N.get(menuRows_[static_cast<size_t>(i)].labelId)); }, nullptr, nullptr,
      [this](int i) -> std::string {
#if FREEINK_CAP_BLE_HID_HOST
        if (menuRows_[static_cast<size_t>(i)].action == Action::ToggleBt) {
          return BleHid.isRunning() ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
        }
        if (menuRows_[static_cast<size_t>(i)].action == Action::Disconnect && BleHid.isConnected()) {
          return BleHid.connectedName();
        }
#else
        (void)i;
#endif
        return "";
      },
      /*highlightValue=*/true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void BluetoothSettingsActivity::renderScan() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_BT_SCAN));

  const int listTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int listHeight = renderer.getScreenHeight() - listTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;

#if FREEINK_CAP_BLE_HID_HOST
  const int count = static_cast<int>(BleHid.deviceCount());
  if (count == 0) {
    const char* msg = BleHid.isScanning() ? tr(STR_SCANNING) : tr(STR_BT_NO_DEVICES);
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, listTop + 20, msg);
  } else {
    GUI.drawList(
        renderer, Rect{0, listTop, pageWidth, listHeight}, count, scanIndex_,
        [](int i) {
          const auto& d = BleHid.device(static_cast<uint8_t>(i));
          return bleDeviceLabel(d);
        },
        nullptr, nullptr,
        [](int i) {
          const auto& d = BleHid.device(static_cast<uint8_t>(i));
          char buf[24];
          snprintf(buf, sizeof(buf), "%ddBm%s", d.rssi, d.hid ? " HID" : "");
          return std::string(buf);
        },
        false);
  }
#else
  renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, listTop + 20, tr(STR_BT_UNAVAILABLE));
#endif

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void BluetoothSettingsActivity::renderPaired() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_BT_PAIRED));

  const int listTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int listHeight = renderer.getScreenHeight() - listTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;

#if FREEINK_CAP_BLE_HID_HOST
  const int count = static_cast<int>(BleHid.pairedCount());
  GUI.drawList(
      renderer, Rect{0, listTop, pageWidth, listHeight}, count, pairedIndex_,
      [](int i) {
        const auto& p = BleHid.paired(static_cast<uint8_t>(i));
        return std::string(p.name[0] ? p.name : p.addr);
      },
      nullptr, nullptr, nullptr, false);
#else
  renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, listTop + 20, tr(STR_BT_UNAVAILABLE));
#endif

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void BluetoothSettingsActivity::render(RenderLock&&) {
  renderer.clearScreen();
  if (view_ == View::Scan) {
    renderScan();
  } else if (view_ == View::Paired) {
    renderPaired();
  } else {
    renderMenu();
  }

  if (!banner_.empty()) {
    const auto& metrics = UITheme::getInstance().getMetrics();
    const int y = renderer.getScreenHeight() - metrics.buttonHintsHeight - metrics.verticalSpacing -
                  renderer.getLineHeight(SMALL_FONT_ID) - 4;
    renderer.drawCenteredText(SMALL_FONT_ID, y, banner_.c_str());
  }

  UiGhostPolicy::displayMenuFrame(renderer);
}
