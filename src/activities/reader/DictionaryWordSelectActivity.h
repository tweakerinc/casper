#pragma once

#include <Epub/Page.h>
#include <I18n.h>

#include <memory>
#include <vector>

#include "activities/Activity.h"

// Word selection over the current reader page: Left/Right step through words
// in reading order, Up/Down jump rows. Short Select looks up the word (or
// multi-word range). Long-press Select starts a multi-word range; move, then
// short Select to look up the phrase. Back clears a range or returns to the
// reader. On touch devices a touch-down moves the highlight and a tap looks up.
class DictionaryWordSelectActivity final : public Activity {
 public:
  explicit DictionaryWordSelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                        std::unique_ptr<Page> page, int marginLeft, int marginTop)
      : Activity("DictionaryWordSelect", renderer, mappedInput),
        page(std::move(page)),
        marginLeft(marginLeft),
        marginTop(marginTop) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  static constexpr int kMaxPhraseWords = 6;

  // Screen box of one selectable word. `text` points into the owned Page's
  // TextBlock arena (NUL-terminated), valid for this activity's lifetime.
  struct WordBox {
    int16_t x;
    int16_t y;
    int16_t width;
    uint16_t row;
    const char* text;
    EpdFontFamily::Style style;
  };

  enum class Popup : uint8_t { None, Busy, NotFound, Error };

  void extractWords();
  int closestInRow(uint16_t row, int centerX) const;
  int wordAt(int x, int y) const;
  void moveVertical(int direction);
  // Inclusive range bounds for current selection (single word or multi-word).
  void selectionBounds(int& lo, int& hi) const;
  // Join soft-/line-break hyphens; multi-word joins with spaces.
  std::string buildLookupToken() const;
  void performLookup();
  bool drawHighlightWithSnapshot();
  void drawSelectionHighlights();
  // Compact top title while reader chrome is hidden (mode cue for the tool).
  void drawModeTitle() const;
  void drawHints() const;

  std::unique_ptr<Page> page;
  const int marginLeft;
  const int marginTop;
  int fontId = 0;
  int lineHeight = 0;

  std::vector<WordBox> words;
  int selected = 0;
  // -1 = single-word mode; otherwise multi-word range anchor index.
  int startMarkIdx = -1;
  uint16_t rowCount = 0;

  Popup popup = Popup::None;
  StrId popupMsg = StrId::STR_DICT_NOT_FOUND;
  unsigned long popupTime = 0;

  // Differential highlight repaint (single-word mode only).
  static constexpr size_t SNAPSHOT_CAPACITY = 4096;
  std::unique_ptr<uint8_t[]> snapshot;
  int16_t snapshotX = 0;
  int16_t snapshotY = 0;
  int16_t snapshotW = 0;
  int16_t snapshotH = 0;
  int snapshotIdx = -1;

  // Confirm/Select state machine (do not use global getHeldTime — it remembers
  // prior nav holds). Ignore residual press that opened dictionary.
  bool ignoreConfirmUntilReleased = true;
  bool confirmDown = false;
  unsigned long confirmDownAtMs = 0;
  bool multiSelectArmedThisHold = false;
};
