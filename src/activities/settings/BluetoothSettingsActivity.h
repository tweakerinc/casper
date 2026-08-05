#pragma once

#include <I18n.h>

#include <string>
#include <vector>

#include "activities/Activity.h"

// BLE page-turner pairing UI (FreeInk BleKeyboardHost / NimBLE central).
// Used from Settings → Bluetooth and from the in-book menu as a quick-connect.
//
// When exitOnSuccessfulConnect is true (reader entry), the activity finishes as
// soon as a device links so the user returns to the book. disableOnExit=false
// keeps the BLE stack up after exit so HID keys keep working in the reader.
class BluetoothSettingsActivity final : public Activity {
 public:
  explicit BluetoothSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                     bool exitOnSuccessfulConnect = false, bool disableOnExit = true);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }

 private:
  enum class View : uint8_t { Menu, Scan, Paired };

  enum class Action : uint8_t {
    ToggleBt,
    Scan,
    QuickConnect,
    Disconnect,
    PairedDevices,
    Forget,
  };

  struct MenuRow {
    Action action = Action::ToggleBt;
    StrId labelId = StrId::STR_BLUETOOTH;
    MenuRow() = default;
    MenuRow(Action a, StrId id) : action(a), labelId(id) {}
  };

  View view_ = View::Menu;
  std::vector<MenuRow> menuRows_;
  int menuIndex_ = 0;
  int scanIndex_ = 0;
  int pairedIndex_ = 0;

  std::string banner_;
  unsigned long bannerUntil_ = 0;

  bool awaitingConnect_ = false;
  std::string pendingConnectAddr_;
  unsigned long awaitingConnectStartedAt_ = 0;
  unsigned long lastScanAnimMs_ = 0;

  const bool exitOnSuccessfulConnect_;
  const bool disableOnExit_;

  void rebuildMenuRows();
  void handleMenuConfirm();
  void setBanner(const char* text, unsigned long durationMs = 2500);
  // Bring up NimBLE if needed; returns false when unavailable (banner set).
  bool ensureBleStarted();
  void startScanView();
  void tryQuickConnect();
  void renderMenu();
  void renderScan();
  void renderPaired();
  void pollConnectResult();
};
