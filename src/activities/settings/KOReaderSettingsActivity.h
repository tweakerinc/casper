#pragma once

#include "activities/Activity.h"
#include "components/OptionPopup.h"
#include "util/ButtonNavigator.h"

/**
 * Submenu for KOReader Sync settings.
 * Credentials, match method, Auto Upload on Close (nests Sync Behavior popup with
 * Ask Every Time / Smart Sync / Percent / Time, plus Upload Metadata), Sign Up / Authenticate.
 */
class KOReaderSettingsActivity final : public Activity {
 public:
  explicit KOReaderSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("KOReaderSettings", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;
  OptionPopup optionPopup;
  size_t selectedIndex = 0;

  void handleSelection();
  void openTimeIntervalPicker();
  void openPercentThresholdPicker();
};
