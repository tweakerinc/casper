#pragma once

#include <DictionaryLookup.h>
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

// Clip-like word selection for offline dictionary lookup.
// Short Select looks up the word under the cursor (joins soft-hyphen line breaks).
// Long-press Select starts a multi-word range; move, then Select to look up the phrase.
// Select again closes the definition popup. Back exits dictionary mode (or clears a range).
class DictionarySelectionActivity final : public Activity {
 public:
  DictionarySelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::vector<WordRef> words,
                              int fontId, Section& section, int startPageInSection, int marginTop, int marginLeft);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
  bool allowPowerAsConfirmInReaderMode() const override { return true; }

 private:
  static constexpr size_t BUFFER_CHUNK_SIZE = 4096;
  // Cap multi-word phrases so lookup keys stay within DictionaryLookup::kMaxKeyLen.
  static constexpr int kMaxPhraseWords = 6;

  std::vector<WordRef> words;
  int renderFontId = 0;
  Section& section;
  int startPageInSection = 0;
  int marginTop = 0;
  int marginLeft = 0;

  std::vector<std::unique_ptr<uint8_t[]>> savedBufferChunks;
  size_t savedBufferSize = 0;
  int currentDisplayPage = 0;
  int savedSectionPage = 0;

  int cursorIdx = 0;
  // -1 = single-word mode; otherwise multi-word range anchor in readingOrder.
  int startMarkIdx = -1;
  bool multiSelectHoldArmed = false;
  // True until Confirm/Power are fully released after open. Prevents long-press Menu
  // (which uses Confirm) from immediately arming multi-word selection mode.
  bool ignoreConfirmUntilReleased = true;
  bool needsPageSwitch = false;
  bool hasSavedBuffer = false;
  bool usingFallbackFont = false;
  mutable std::array<std::string, 4> prewarmTextByStyle;
  std::vector<int> readingOrder;

  // Definition popup state (drawn over the page snapshot; no full-screen clear).
  bool definitionOpen = false;
  std::string lookupWord;
  std::string definition;
  char langLabel[DictionaryLookup::kMaxLangLabelLen] = {};
  bool found = false;
  bool missingFile = false;
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

  void buildReadingOrder();
  // Prefer a word near the geometric center of the first page (light random among near-center).
  int pickInitialCursorIdx() const;
  // Vertical move that preserves column (closest x on next/prev line); wraps within the same page only.
  int moveVertical(int orderIdx, int direction) const;
  // Horizontal step in reading order, clamped to the current page (wraps first↔last on that page).
  int moveHorizontal(int orderIdx, int direction) const;
  bool allocateSavedBuffer();
  void storeCurrentBuffer();
  void restoreSavedBuffer() const;
  bool switchToPage(int pageIdx);
  bool prewarmHighlightedWords() const;
  void drawHighlights();
  void applyWordStyle(const WordRef& word, bool invert) const;
  void useFallbackFont(const char* reason);

  // Build a lookup phrase for [fromOrder, toOrder] (inclusive), joining soft hyphens.
  std::string buildLookupPhrase(int fromOrder, int toOrder) const;
  void openDefinitionForPhrase(const std::string& phrase);
  void openDefinitionForCursor();
  void openDefinitionForRange();
  void closeDefinition();
  void layoutPopupWidth();
  void layoutPopupFromContent();
  void rebuildLines();
  void drawDefinitionPopup();
  void exitDictionaryMode();
};
