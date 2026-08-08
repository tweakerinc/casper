#pragma once

#include <Epub/Page.h>
#include <Epub/Section.h>
#include <Memory.h>

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "WordRef.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

struct ClipWordStyle {
  enum Flags : uint8_t {
    NONE = 0,
    FILL = 1 << 0,
    UNDERLINE = 1 << 2,
    BORDER = 1 << 3,
  };

  uint8_t flags = FILL;
};

class ClipSelectionActivity final : public Activity {
 public:
  ClipSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::vector<WordRef> words, int fontId,
                        Section& section, int startPageInSection, int marginTop, int marginLeft);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }

 private:
  static constexpr size_t BUFFER_CHUNK_SIZE = 4096;
  // Snapshot at most ~48 KB (12×4 KB). Full X4 FB is larger — skip snapshot then.
  static constexpr size_t MAX_SAVED_BUFFER_CHUNKS = 12;
  // Keep in sync with harvest caps (issue #4: 768 + SDCF mini-kern OOM'd X4).
  static constexpr size_t MAX_READING_ORDER_WORDS = 400;

  std::vector<WordRef> words;
  int renderFontId = 0;
  Section& section;
  int startPageInSection = 0;
  int marginTop = 0;
  int marginLeft = 0;

  std::array<std::unique_ptr<uint8_t[]>, MAX_SAVED_BUFFER_CHUNKS> savedBufferChunks;
  size_t savedBufferChunkCount = 0;
  size_t savedBufferSize = 0;
  int currentDisplayPage = 0;
  int savedSectionPage = 0;

  int cursorIdx = 0;
  int startMarkIdx = -1;
  bool needsPageSwitch = false;
  bool hasSavedBuffer = false;
  bool usingFallbackFont = false;
  // Opened via Long-Press Back/Menu (or residual edges): require Back/Confirm to
  // go idle once and eat release edges so the open gesture cannot cancel/select.
  bool ignoreBackUntilReleased = true;
  bool ignoreConfirmUntilReleased = true;
  // Extra time fence after onEnter (harvest can outlast the physical hold).
  unsigned long openGuardUntilMs = 0;
  std::array<uint16_t, MAX_READING_ORDER_WORDS> readingOrder{};
  size_t readingOrderSize = 0;

  ButtonNavigator buttonNavigator;

  void buildReadingOrder();
  void resetSavedBufferChunks();
  bool allocateSavedBuffer();
  void storeCurrentBuffer();
  void restoreSavedBuffer() const;
  bool switchToPage(int pageIdx);
  void drawHighlights();
  void applyWordStyle(const WordRef& word, const ClipWordStyle& style) const;
  void useFallbackFont(const char* reason);
  int lineEndForward(int orderIdx) const;
  int lineEndBackward(int orderIdx) const;
};
