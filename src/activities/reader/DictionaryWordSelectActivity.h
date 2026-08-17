#pragma once

#include <Epub/Page.h>
#include <I18n.h>

#include <memory>
#include <vector>

#include "activities/Activity.h"

// Word selection over the current reader page: Left/Right step through words
// in reading order, Up/Down jump rows (logical directions — Landscape CCW /
// Portrait 180° with Orient Front Buttons follow MappedInputManager).
//
// Dictionary mode: short Select looks up the word (or multi-word range).
// Long-press Select starts a multi-word range; move, then short Select to look
// up the phrase. Back clears a range or returns to the reader.
//
// Clip mode (classic Clipping Tool): short Select marks the start word; move;
// short Select again confirms the end and returns ClippingResult. Back clears
// a mark or cancels. No 6-word phrase cap — range can cover the whole page.
//
// On touch devices a touch-down moves the highlight and a tap acts like Select.
class DictionaryWordSelectActivity final : public Activity {
 public:
  enum class Mode : uint8_t { Dictionary, Clip };

  // Screen box of one selectable word. `text` points into Page arena or textPool_.
  struct WordBox {
    int16_t x;
    int16_t y;
    int16_t width;
    uint16_t row;
    const char* text;
    EpdFontFamily::Style style;
  };

  // Classic path: words extracted from a Section Page.
  explicit DictionaryWordSelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                        std::unique_ptr<Page> page, int marginLeft, int marginTop,
                                        Mode mode = Mode::Dictionary)
      : Activity(mode == Mode::Clip ? "ClipWordSelect" : "DictionaryWordSelect", renderer, mappedInput),
        page(std::move(page)),
        marginLeft(marginLeft),
        marginTop(marginTop),
        mode_(mode) {}

  // Rivulet path: prebuilt word boxes + full-framebuffer snapshot of the page
  // (no Section/Page). Coordinates are already screen-absolute.
  DictionaryWordSelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::vector<WordBox> prebuilt,
                               std::vector<std::string> textPool, int fontId, std::unique_ptr<uint8_t[]> pageFb,
                               size_t pageFbBytes, int marginLeft, int marginTop, Mode mode = Mode::Dictionary)
      : Activity(mode == Mode::Clip ? "ClipWordSelect" : "DictionaryWordSelect", renderer, mappedInput),
        page(nullptr),
        marginLeft(marginLeft),
        marginTop(marginTop),
        fontId(fontId),
        mode_(mode),
        words(std::move(prebuilt)),
        textPool_(std::move(textPool)),
        pageFb_(std::move(pageFb)),
        pageFbBytes_(pageFbBytes) {
    // Re-point text pointers into the owned pool (vector reallocation already done).
    for (size_t i = 0; i < words.size() && i < textPool_.size(); ++i) {
      words[i].text = textPool_[i].c_str();
    }
    uint16_t maxRow = 0;
    for (const auto& w : words) {
      if (w.row > maxRow) maxRow = w.row;
    }
    rowCount = words.empty() ? 0 : static_cast<uint16_t>(maxRow + 1);
  }

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  static constexpr int kMaxPhraseWords = 6;

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
  // Clip mode: first Select marks start; second Select builds ClippingResult and finishes.
  void performClipConfirm();
  void handleSelectAction();
  bool drawHighlightWithSnapshot();
  void drawSelectionHighlights();
  // Highlight box: full line cell (must cover all white redraw ink).
  void highlightBoxFor(const WordBox& word, int& hx, int& hy, int& hw, int& hh) const;
  // Solid black fill + white text (cheapest BW e-ink path).
  void paintWordHighlight(const WordBox& word) const;
  // Compact top title while reader chrome is hidden (mode cue for the tool).
  void drawModeTitle() const;
  void drawHints() const;

  std::unique_ptr<Page> page;
  const int marginLeft;
  const int marginTop;
  int fontId = 0;
  // Reader line pitch (advanceY × Tight/Normal/Wide), not bare font advanceY.
  int lineHeight = 0;
  Mode mode_ = Mode::Dictionary;

  std::vector<WordBox> words;
  std::vector<std::string> textPool_;  // owns strings for Rivulet/prebuilt mode
  std::unique_ptr<uint8_t[]> pageFb_;  // full page snapshot for Rivulet mode
  size_t pageFbBytes_ = 0;
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
