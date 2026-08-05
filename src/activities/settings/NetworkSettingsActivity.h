#pragma once

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// System → Network: Wi‑Fi, KOReader Sync, OPDS Servers.
class NetworkSettingsActivity final : public Activity {
 public:
  explicit NetworkSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("NetworkSettings", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;
  int selectedIndex = 0;
  int itemCount = 0;

  void handleSelection();
};
