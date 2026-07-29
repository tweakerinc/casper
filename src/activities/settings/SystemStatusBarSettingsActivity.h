#pragma once

#include "activities/Activity.h"
#include "components/OptionPopup.h"
#include "util/ButtonNavigator.h"

// Display → Status Bar: system top chrome (Left / Middle / Right).
// Each slot is Hide, Battery, or Clock (Battery and Clock exclusive).
// When Clock is placed, clock format / UTC offset / sync nest under that slot.
// Live top-chrome preview matches Customize Reader UI style.
class SystemStatusBarSettingsActivity final : public Activity {
 public:
  explicit SystemStatusBarSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("SystemStatusBarSettings", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;
  OptionPopup optionPopup;
  int selectedIndex = 0;
  int visibleItemCount = 0;

  void handleSelection();
};
