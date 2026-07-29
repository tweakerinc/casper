#pragma once

#include <string>

#include "CrossPointSettings.h"
#include "activities/Activity.h"
#include "components/OptionPopup.h"

class ButtonRemapActivity final : public Activity {
 public:
  explicit ButtonRemapActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ButtonRemap", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  static constexpr int kSlotCount = CrossPointSettings::HW_REMAP_BUTTON_COUNT;  // 6 physical keys
  static constexpr int kResetRow = kSlotCount;                                  // 7th row: reset defaults
  static constexpr int kListCount = kSlotCount + 1;

  uint8_t tempMap[CrossPointSettings::HW_REMAP_BUTTON_COUNT] = {};
  int selectedIndex = 0;
  unsigned long errorUntil = 0;
  std::string errorMessage;
  OptionPopup functionPopup;
  bool dirty = false;

  void openFunctionPicker();
  bool tryAssign(uint8_t hwIndex, uint8_t function);
  void resetDefaults();
  void commitAndExit();
  void showError(const char* msg);

  const char* slotName(uint8_t hwIndex) const;
  const char* functionName(uint8_t function) const;
  void drawSlotArrows() const;
};
