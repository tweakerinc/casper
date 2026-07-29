#pragma once

#include "activities/Activity.h"
#include "components/OptionPopup.h"
#include "util/ButtonNavigator.h"

// Clock settings (X3 RTC): system-wide show/hide, format, UTC offset, NTP sync.
// Opened from Settings → Display → Clock.
class ClockSettingsActivity final : public Activity {
 public:
  explicit ClockSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ClockSettings", renderer, mappedInput) {}

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
