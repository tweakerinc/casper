#pragma once

#include <DictionaryLookup.h>

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Shows an offline English dictionary definition for a selected word as a centered popup.
class DictionaryLookupActivity final : public Activity {
 public:
  DictionaryLookupActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string lookupWord);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
  bool allowPowerAsConfirmInReaderMode() const override { return true; }

 private:
  std::string lookupWord;  // not named "word" — Arduino.h macros `word` to makeWord
  std::string definition;
  char langLabel[DictionaryLookup::kMaxLangLabelLen] = {};
  bool found = false;
  bool missingFile = false;
  // Ignore Confirm release that opens us (still held from long-press).
  bool ignoreConfirmUntilRelease = true;
  int scrollLine = 0;
  int popupX = 0;
  int popupY = 0;
  int popupW = 0;
  int popupH = 0;
  int bodyTop = 0;
  int bodyH = 0;
  int visibleLines = 1;
  std::vector<std::string> lines;
  ButtonNavigator buttonNavigator;

  void rebuildLines();
  void layoutPopup();
};
