#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include "activities/Activity.h"

/**
 * Activity for testing KOReader credentials, or — in sign-up mode — creating a
 * new account on the sync server with the entered username/password.
 * Connects to WiFi, then authenticates or registers.
 *
 * Prefers quiet saved-network connect (no scan UI) so TLS has enough free heap.
 * Falls back to WifiSelectionActivity when no credentials are stored.
 */
class KOReaderAuthActivity final : public Activity {
 public:
  enum class Mode { AUTHENTICATE, SIGN_UP };

  explicit KOReaderAuthActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, Mode mode = Mode::AUTHENTICATE)
      : Activity("KOReaderAuth", renderer, mappedInput), mode(mode) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override {
    return state == CONNECTING || state == AUTHENTICATING || quietWifiPending;
  }

 private:
  enum State { WIFI_SELECTION, CONNECTING, AUTHENTICATING, SUCCESS, FAILED };

  Mode mode = Mode::AUTHENTICATE;
  State state = WIFI_SELECTION;
  std::string statusMessage;
  std::string errorMessage;

  // Quiet saved-SSID connect (same idea as leave-sync) — avoids scan heap.
  bool quietWifiPending = false;
  bool quietWifiBeginIssued = false;
  size_t quietWifiCredIndex = 0;
  size_t quietWifiAttempts = 0;
  unsigned long quietWifiStartMs = 0;
  static constexpr unsigned long QUIET_WIFI_TIMEOUT_MS = 12000;

  void onWifiSelectionComplete(bool success);
  void performAuthentication();
  void startQuietWifiConnect();
  void tickQuietWifiConnect();
};
