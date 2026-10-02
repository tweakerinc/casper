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
  unsigned long lastContinuousNavTime = 0;
  std::string errorMessage;
  OptionPopup functionPopup;
  bool dirty = false;

  // Locked editor chrome (matches footer): hw0 Back, hw1 Select, hw2 Up, hw3 Down.
  static constexpr uint16_t kLockedNavStartMs = 500;
  static constexpr uint16_t kLockedNavIntervalMs = 350;

  void openFunctionPicker();
  bool tryAssign(uint8_t hwIndex, uint8_t function);
  void resetDefaults();
  void commitAndExit();
  void showError(const char* msg);
  bool lockedContinuousNav(uint8_t hwIndex);

  const char* slotName(uint8_t hwIndex) const;
  const char* functionName(uint8_t function) const;
  void drawSlotArrows() const;
};
