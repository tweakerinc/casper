#pragma once

#include <functional>
#include <vector>

#include "MappedInputManager.h"

class ButtonNavigator final {
  using Callback = std::function<void()>;
  using Buttons = std::vector<MappedInputManager::Button>;

  const uint16_t continuousStartMs;
  const uint16_t continuousIntervalMs;
  uint32_t lastContinuousNavTime = 0;
  static const MappedInputManager* mappedInput;

  [[nodiscard]] bool shouldNavigateContinuously() const;

 public:
  explicit ButtonNavigator(const uint16_t continuousIntervalMs = 500, const uint16_t continuousStartMs = 500)
      : continuousStartMs(continuousStartMs), continuousIntervalMs(continuousIntervalMs) {}

  static void setMappedInputManager(const MappedInputManager& mappedInputManager) { mappedInput = &mappedInputManager; }

  void onNext(const Callback& callback);
  void onPrevious(const Callback& callback);
  void onPressAndContinuous(const Buttons& buttons, const Callback& callback);

  void onNextPress(const Callback& callback);
  void onPreviousPress(const Callback& callback);
  void onPress(const Buttons& buttons, const Callback& callback);

  void onNextRelease(const Callback& callback);
  void onPreviousRelease(const Callback& callback);
  void onRelease(const Buttons& buttons, const Callback& callback);

  void onNextContinuous(const Callback& callback);
  void onPreviousContinuous(const Callback& callback);
  void onContinuous(const Buttons& buttons, const Callback& callback);

  [[nodiscard]] static int nextIndex(int currentIndex, int totalItems);
  [[nodiscard]] static int previousIndex(int currentIndex, int totalItems);

  [[nodiscard]] static int nextPageIndex(int currentIndex, int totalItems, int itemsPerPage);
  [[nodiscard]] static int previousPageIndex(int currentIndex, int totalItems, int itemsPerPage);

  // Navigation uses the logical NavNext / NavPrevious buttons; MappedInputManager::mapButton resolves
  // them to physical buttons and applies any orientation-based direction swap, so this stays settings-free.
  // NavPrevious = Up | Left; NavNext = Down | Right (either axis can move a plain vertical list).
  [[nodiscard]] static Buttons getNextButtons() { return {MappedInputManager::Button::NavNext}; }
  [[nodiscard]] static Buttons getPreviousButtons() { return {MappedInputManager::Button::NavPrevious}; }

  // List navigation on tabbed screens (vertical): Up / Down only — not NavNext, which also
  // binds Left/Right used for tabs. Axis matches the function names.
  [[nodiscard]] static Buttons getFrontNextButtons() {
    if (mappedInput != nullptr && mappedInput->isNavDirectionSwapped()) {
      return {MappedInputManager::Button::Up};
    }
    return {MappedInputManager::Button::Down};
  }
  [[nodiscard]] static Buttons getFrontPreviousButtons() {
    if (mappedInput != nullptr && mappedInput->isNavDirectionSwapped()) {
      return {MappedInputManager::Button::Down};
    }
    return {MappedInputManager::Button::Up};
  }

  // Tab switch on tabbed screens (horizontal): Left / Right only.
  [[nodiscard]] static Buttons getSideNextButtons() {
    if (mappedInput != nullptr && mappedInput->isNavDirectionSwapped()) {
      return {MappedInputManager::Button::Left};
    }
    return {MappedInputManager::Button::Right};
  }
  [[nodiscard]] static Buttons getSidePreviousButtons() {
    if (mappedInput != nullptr && mappedInput->isNavDirectionSwapped()) {
      return {MappedInputManager::Button::Right};
    }
    return {MappedInputManager::Button::Left};
  }
};
