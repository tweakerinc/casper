#pragma once
#include "activities/Activity.h"

class Bitmap;

class SleepActivity final : public Activity {
 public:
  explicit SleepActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool fromTimeout = false)
      : Activity("Sleep", renderer, mappedInput) {
    (void)fromTimeout;
  }
  void onEnter() override;

 private:
  void renderDefaultSleepScreen() const;
  void renderCustomSleepScreen() const;
  void renderCoverSleepScreen() const;
  void renderBitmapSleepScreen(const Bitmap& bitmap) const;
  // PNG from /.sleep, /sleep, or /sleep.png — convert to a temp BMP then reuse BMP paint.
  bool renderPngSleepScreen(const std::string& pngPath) const;
  void renderBlankSleepScreen() const;
};
