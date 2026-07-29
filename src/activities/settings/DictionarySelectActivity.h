#pragma once

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"
#include "util/DictionaryRegistry.h"

// Multi-select dictionary page: Up/Down navigate, Confirm toggles selection
// (filled bubble). Back becomes Save when the selection changed; otherwise Back
// dismisses without writing settings.
class DictionarySelectActivity final : public Activity {
 public:
  explicit DictionarySelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("DictionarySelect", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  void loadFromSettings();
  bool selectionChanged() const;
  void saveAndExit();
  void toggleSelected();

  ButtonNavigator buttonNavigator;
  std::vector<DictionaryEntry> dictionaries;
  std::vector<uint8_t> selected;  // parallel to dictionaries; 1 = enabled
  std::vector<uint8_t> initialSelected;
  int selectedIndex = 0;
  // Absorb the Confirm release that opened this page from Settings so it does
  // not immediately toggle the first dictionary (usually English).
  bool ignoreNextConfirmRelease = false;
};
