#pragma once

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// System → Stats: enable tracking, auto backup (when on), backup now.
class StatsSettingsActivity final : public Activity {
 public:
  explicit StatsSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("StatsSettings", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;
  int selectedIndex = 0;
  int visibleItemCount = 0;

  void rebuildMenu();
  void handleSelection();
};
