#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Popup-style viewer for one dictionary definition (StarDict data, CrossInk-like layout).
// Headword left/centered bold; optional pronunciation; wrapped senses; Up/Down scroll.
// Restores a full-framebuffer snapshot under the card so the reader page is not whitened.
// Back → isCancelled (stay in word-select). Done/Confirm → not cancelled (exit dictionary).
class DictionaryDefinitionActivity final : public Activity {
 public:
  explicit DictionaryDefinitionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string headword,
                                        std::string definition)
      : Activity("DictionaryDefinition", renderer, mappedInput),
        headword(std::move(headword)),
        definition(std::move(definition)) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  void normalizeDefinition();
  void rebuildLines();
  void layoutPopup();
  bool captureBackground();

  const std::string headword;
  // Not const: onEnter() normalizes StarDict markup / HTML into plain text.
  std::string definition;
  std::string pronunciation;  // Extracted /.../ or [...] after headword, if any
  std::vector<std::string> lines;
  int scrollLine = 0;
  int visibleLines = 1;
  int popupX = 0;
  int popupY = 0;
  int popupW = 0;
  int popupH = 0;
  int bodyTop = 0;
  int bodyH = 0;
  // Card-region snapshot (not full framebuffer) so the popup redraws over the page.
  std::unique_ptr<uint8_t[]> backgroundBuffer;
  size_t backgroundBufferSize = 0;
  int bgX = 0;
  int bgY = 0;
  int bgW = 0;
  int bgH = 0;
  bool hasBackground = false;
  ButtonNavigator buttonNavigator;
};
