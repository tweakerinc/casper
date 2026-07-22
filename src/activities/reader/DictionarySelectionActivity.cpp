#include "DictionarySelectionActivity.h"

#include <DictionaryLookup.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>  // strncpy
#include <cstdlib>  // random

#include "MappedInputManager.h"
#include "ReaderUtils.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

constexpr int DICT_SELECTION_FALLBACK_FONT_ID = UI_12_FONT_ID;
// Horizontal inset from screen edge; leave room so book text peeks around the card.
constexpr int kPopupMarginX = 20;
constexpr int kPopupMarginY = 16;
// Inner padding of the card.
constexpr int kPopupPadX = 18;
constexpr int kPopupPadY = 14;
constexpr int kCorner = 10;
constexpr int kTitleBodyGap = 10;
constexpr int kHeaderLineGap = 8;
// Reserve bottom strip so button hints stay clear of the card.
constexpr int kButtonHintsReserve = 44;
constexpr int kMaxBodyLines = 96;
constexpr int kMaxDefBuf = DictionaryLookup::kMaxDefinitionLen + 1;
// Match chapter-select / Lyra list scroll chrome (LyraTheme::drawListWithMetrics).
// Bar sits in the card's right padding so it never shares the text column.
constexpr int kScrollBarW = 4;
constexpr int kScrollBarRightOffset = 5;

// popupLeft/popupW: card bounds; bodyTop/bodyH: vertical track span (text body only).
void drawDefinitionScrollBar(const GfxRenderer& renderer, const int popupLeft, const int popupW, const int bodyTop,
                             const int bodyH, const int totalLines, const int visibleLines, const int scrollLine) {
  if (totalLines <= visibleLines || bodyH < 12 || visibleLines <= 0) {
    return;
  }
  const int scrollBarHeight = std::max(kScrollBarW, (bodyH * visibleLines) / totalLines);
  const int maxStart = std::max(1, totalLines - visibleLines);
  const int clamped = std::clamp(scrollLine, 0, maxStart);
  const int scrollBarY = bodyTop + ((bodyH - scrollBarHeight) * clamped) / maxStart;
  // Same as list: track at rect.right - rightOffset, thumb immediately left of track.
  const int scrollBarX = popupLeft + popupW - kScrollBarRightOffset;
  renderer.drawLine(scrollBarX, bodyTop, scrollBarX, bodyTop + bodyH - 1, true);
  renderer.fillRect(scrollBarX - kScrollBarW, scrollBarY, kScrollBarW, scrollBarHeight, true);
}

bool hasEmSpace(const std::string& text) {
  return text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xE2 &&
         static_cast<unsigned char>(text[1]) == 0x80 && static_cast<unsigned char>(text[2]) == 0x83;
}

}  // namespace

DictionarySelectionActivity::DictionarySelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                         std::vector<WordRef> wordsIn, const int fontId,
                                                         Section& sectionIn, const int startPageInSectionIn,
                                                         const int marginTopIn, const int marginLeftIn)
    : Activity("DictionarySelection", renderer, mappedInput),
      words(std::move(wordsIn)),
      renderFontId(fontId),
      section(sectionIn),
      startPageInSection(startPageInSectionIn),
      marginTop(marginTopIn),
      marginLeft(marginLeftIn) {}

void DictionarySelectionActivity::onEnter() {
  Activity::onEnter();

  if (words.empty()) {
    LOG_ERR("DICT", "No words available for dictionary selection");
    exitDictionaryMode();
    return;
  }
  buildReadingOrder();
  if (readingOrder.empty()) {
    LOG_ERR("DICT", "No readable word order available");
    exitDictionaryMode();
    return;
  }
  cursorIdx = pickInitialCursorIdx();
  startMarkIdx = -1;
  multiSelectHoldArmed = false;
  // Long-press Menu opens dictionary while Confirm is still held — do not treat that
  // residual hold as multi-word selection. Wait for a full release first.
  ignoreConfirmUntilReleased = true;
  mappedInput.suppressNextConfirmRelease();
  mappedInput.suppressNextPowerConfirmRelease();

  // Keep dictionary packs open for the whole selection session (multi-word looks).
  DictionaryLookup::beginSession();

  savedSectionPage = section.currentPage;
  if (!allocateSavedBuffer()) {
    exitDictionaryMode();
    return;
  }

  // Start on the page that holds the chosen cursor word (usually page 0).
  const int startPage = words[readingOrder[static_cast<size_t>(cursorIdx)]].pageIdx;
  if (!switchToPage(startPage)) {
    exitDictionaryMode();
    return;
  }
  requestUpdate();
}

void DictionarySelectionActivity::onExit() {
  DictionaryLookup::endSession();
  section.currentPage = savedSectionPage;
  savedBufferChunks.clear();
  hasSavedBuffer = false;
  if (usingFallbackFont) {
    if (auto* fcm = renderer.getFontCacheManager()) {
      fcm->clearCache();
    }
  }
  Activity::onExit();
}

bool DictionarySelectionActivity::allocateSavedBuffer() {
  // Match clipping: if the ~48KB snapshot cannot allocate, keep going and re-render pages.
  savedBufferSize = renderer.getBufferSize();
  const size_t chunkCount = (savedBufferSize + BUFFER_CHUNK_SIZE - 1) / BUFFER_CHUNK_SIZE;
  savedBufferChunks.clear();
  savedBufferChunks.reserve(chunkCount);

  for (size_t i = 0; i < chunkCount; i++) {
    const size_t offset = i * BUFFER_CHUNK_SIZE;
    const size_t chunkSize = std::min(BUFFER_CHUNK_SIZE, savedBufferSize - offset);
    auto chunk = makeUniqueNoThrow<uint8_t[]>(chunkSize);
    if (!chunk) {
      LOG_ERR("DICT", "OOM: dictionary page snapshot chunk %u (%u bytes); using rerender fallback",
              static_cast<unsigned>(i), static_cast<unsigned>(chunkSize));
      savedBufferChunks.clear();
      savedBufferSize = 0;
      hasSavedBuffer = false;
      return true;
    }
    savedBufferChunks.push_back(std::move(chunk));
  }
  return true;
}

void DictionarySelectionActivity::storeCurrentBuffer() {
  if (savedBufferChunks.empty() || savedBufferSize == 0) {
    hasSavedBuffer = false;
    return;
  }
  const uint8_t* frameBuffer = renderer.getFrameBuffer();
  for (size_t i = 0; i < savedBufferChunks.size(); i++) {
    const size_t offset = i * BUFFER_CHUNK_SIZE;
    const size_t chunkSize = std::min(BUFFER_CHUNK_SIZE, savedBufferSize - offset);
    memcpy(savedBufferChunks[i].get(), frameBuffer + offset, chunkSize);
  }
  hasSavedBuffer = true;
}

void DictionarySelectionActivity::restoreSavedBuffer() const {
  if (!hasSavedBuffer || savedBufferChunks.empty()) return;
  uint8_t* frameBuffer = renderer.getFrameBuffer();
  for (size_t i = 0; i < savedBufferChunks.size(); i++) {
    const size_t offset = i * BUFFER_CHUNK_SIZE;
    const size_t chunkSize = std::min(BUFFER_CHUNK_SIZE, savedBufferSize - offset);
    memcpy(frameBuffer + offset, savedBufferChunks[i].get(), chunkSize);
  }
}

void DictionarySelectionActivity::buildReadingOrder() {
  readingOrder.clear();
  readingOrder.reserve(words.size());

  int lineStart = 0;
  const int total = static_cast<int>(words.size());
  while (lineStart < total) {
    int lineEnd = lineStart + 1;
    while (lineEnd < total && words[lineEnd].pageIdx == words[lineStart].pageIdx &&
           words[lineEnd].y == words[lineStart].y) {
      lineEnd++;
    }

    if (words[lineStart].lineIsRtl) {
      for (int i = lineEnd - 1; i >= lineStart; --i) {
        readingOrder.push_back(i);
      }
    } else {
      for (int i = lineStart; i < lineEnd; ++i) {
        readingOrder.push_back(i);
      }
    }
    lineStart = lineEnd;
  }
}

void DictionarySelectionActivity::exitDictionaryMode() {
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}

std::string DictionarySelectionActivity::buildLookupPhrase(const int fromOrder, const int toOrder) const {
  const int total = static_cast<int>(readingOrder.size());
  if (fromOrder < 0 || toOrder < fromOrder || toOrder >= total) return {};

  std::string phrase;
  phrase.reserve(64);
  for (int oi = fromOrder; oi <= toOrder; ++oi) {
    const WordRef& word = words[readingOrder[static_cast<size_t>(oi)]];
    std::string piece = word.text;
    // Drop leading em-space used for paragraph indent so it is not part of the key.
    if (piece.size() >= 3 && static_cast<unsigned char>(piece[0]) == 0xE2 &&
        static_cast<unsigned char>(piece[1]) == 0x80 && static_cast<unsigned char>(piece[2]) == 0x83) {
      piece.erase(0, 3);
    }
    // Soft hyphen at line end: join next token without a space and drop the '-'.
    if (word.endsWithInsertedHyphen && !piece.empty() && piece.back() == '-') {
      piece.pop_back();
    }
    if (piece.empty()) continue;

    if (!phrase.empty()) {
      const WordRef& prev = words[readingOrder[static_cast<size_t>(oi - 1)]];
      // Soft-hyphen continuation or same-line attached run: no space.
      const bool joinTight = prev.endsWithInsertedHyphen ||
                             (prev.pageIdx == word.pageIdx && prev.y == word.y &&
                              word.x <= prev.x + prev.w + 2);
      if (!joinTight) {
        phrase.push_back(' ');
      }
    }
    phrase += piece;
  }
  return phrase;
}

void DictionarySelectionActivity::openDefinitionForPhrase(const std::string& phrase) {
  char cleaned[DictionaryLookup::kMaxKeyLen];
  strncpy(cleaned, phrase.c_str(), sizeof(cleaned) - 1);
  cleaned[sizeof(cleaned) - 1] = '\0';
  if (DictionaryLookup::normalizeWord(cleaned, sizeof(cleaned))) {
    lookupWord = cleaned;
  } else {
    lookupWord = phrase;
  }

  definition.clear();
  langLabel[0] = '\0';
  found = false;
  missingFile = !DictionaryLookup::anyAvailable();
  scrollLine = 0;

  if (!missingFile) {
    char buf[kMaxDefBuf];
    buf[0] = '\0';
    found = DictionaryLookup::lookupAuto(lookupWord.c_str(), buf, sizeof(buf), langLabel, sizeof(langLabel));
    if (found) {
      definition = buf;
    }
  }

  layoutPopupWidth();
  rebuildLines();
  layoutPopupFromContent();
  definitionOpen = true;
  requestUpdate();
}

void DictionarySelectionActivity::openDefinitionForCursor() {
  const int total = static_cast<int>(readingOrder.size());
  if (cursorIdx < 0 || cursorIdx >= total) return;

  // Single-word path: auto-join soft-hyphen continuations (e.g. "fetid-" + "smelling").
  int to = cursorIdx;
  while (to + 1 < total) {
    const WordRef& cur = words[readingOrder[static_cast<size_t>(to)]];
    if (!cur.endsWithInsertedHyphen) break;
    if (to - cursorIdx + 1 >= kMaxPhraseWords) break;
    ++to;
  }
  openDefinitionForPhrase(buildLookupPhrase(cursorIdx, to));
}

void DictionarySelectionActivity::openDefinitionForRange() {
  const int total = static_cast<int>(readingOrder.size());
  if (startMarkIdx < 0 || cursorIdx < 0 || cursorIdx >= total) return;
  int from = std::min(startMarkIdx, cursorIdx);
  int to = std::max(startMarkIdx, cursorIdx);
  if (to - from + 1 > kMaxPhraseWords) {
    // Keep the end under the cursor so the user can shrink by moving back.
    if (cursorIdx >= startMarkIdx) {
      from = to - kMaxPhraseWords + 1;
    } else {
      to = from + kMaxPhraseWords - 1;
    }
  }
  openDefinitionForPhrase(buildLookupPhrase(from, to));
  startMarkIdx = -1;
}

void DictionarySelectionActivity::closeDefinition() {
  definitionOpen = false;
  lines.clear();
  definition.clear();
  requestUpdate();
}

void DictionarySelectionActivity::layoutPopupWidth() {
  const int pageW = renderer.getScreenWidth();
  popupW = std::max(220, pageW - kPopupMarginX * 2);
  popupX = (pageW - popupW) / 2;
}

void DictionarySelectionActivity::layoutPopupFromContent() {
  const int pageW = renderer.getScreenWidth();
  const int pageH = renderer.getScreenHeight();
  popupW = std::max(220, pageW - kPopupMarginX * 2);
  popupX = (pageW - popupW) / 2;

  const int titleLineH = std::max(1, renderer.getLineHeight(UI_12_FONT_ID));
  const int lineH = std::max(1, renderer.getLineHeight(UI_10_FONT_ID));
  // Chrome: top pad + title + gap + rule + gap + bottom pad.
  const int chromeH = kPopupPadY + titleLineH + kHeaderLineGap + 1 + kTitleBodyGap + kPopupPadY;

  // Auto-size to content; only scroll when content exceeds the usable screen band.
  // Scrollbar stays available for rare very-long entries (no fixed 8-line card).
  const int minBodyLines = 3;
  const int maxPopupH = std::max(chromeH + lineH * 4, pageH - kButtonHintsReserve - kPopupMarginY * 2);
  const int contentLines = std::max(minBodyLines, static_cast<int>(lines.size()));
  const int desiredH = chromeH + contentLines * lineH;
  popupH = std::clamp(desiredH, chromeH + minBodyLines * lineH, maxPopupH);

  // Slightly above center so the card sits in the reading zone, not on hints.
  popupY = std::max(kPopupMarginY, (pageH - kButtonHintsReserve - popupH) / 2);

  bodyTop = popupY + kPopupPadY + titleLineH + kHeaderLineGap + 1 + kTitleBodyGap;
  bodyH = popupY + popupH - kPopupPadY - bodyTop;
  visibleLines = std::max(1, bodyH / lineH);

  if (scrollLine > 0 && scrollLine + visibleLines > static_cast<int>(lines.size())) {
    scrollLine = std::max(0, static_cast<int>(lines.size()) - visibleLines);
  }
}

void DictionarySelectionActivity::rebuildLines() {
  lines.clear();
  // Text uses the full pad-to-pad column; scrollbar lives in the right pad (no overlap).
  const int contentW = std::max(40, popupW - kPopupPadX * 2);
  if (missingFile) {
    lines = renderer.wrappedText(UI_10_FONT_ID, tr(STR_DICT_NOT_INSTALLED), contentW, 16);
    return;
  }
  if (!found || definition.empty()) {
    char msg[96];
    snprintf(msg, sizeof(msg), tr(STR_DICT_NOT_FOUND_FORMAT), lookupWord.c_str());
    lines = renderer.wrappedText(UI_10_FONT_ID, msg, contentW, 10);
    return;
  }

  // GfxRenderer::wrappedText only splits on spaces, so multi-pack layouts
  // (newlines between pack headers, POS, senses) must be wrapped line-by-line.
  // Pack headers are "@EN" / "@ES->EN" (drawn bold; '@' stripped).
  // Sense lines start with UTF-8 "• ".
  const char* p = definition.c_str();
  while (*p && static_cast<int>(lines.size()) < kMaxBodyLines) {
    const char* nl = strchr(p, '\n');
    std::string para;
    if (nl != nullptr) {
      para.assign(p, static_cast<size_t>(nl - p));
      p = nl + 1;
    } else {
      para = p;
      p += para.size();
    }
    if (!para.empty() && para.back() == '\r') {
      para.pop_back();
    }

    if (para.empty()) {
      if (!lines.empty() && !lines.back().empty()) {
        lines.emplace_back();
      }
      continue;
    }

    const bool isPackHeader = !para.empty() && para[0] == '@';
    const int room = kMaxBodyLines - static_cast<int>(lines.size());
    if (room <= 0) break;

    if (isPackHeader) {
      // Keep pack label on one line; do not soft-wrap "ES->EN".
      lines.push_back(std::move(para));
      continue;
    }

    // Sense bullets: wrap with hanging indent so continued lines sit under text.
    const bool isSense = para.size() >= 4 && static_cast<unsigned char>(para[0]) == 0xE2 &&
                         static_cast<unsigned char>(para[1]) == 0x80 && static_cast<unsigned char>(para[2]) == 0xA2;
    auto wrapped = renderer.wrappedText(UI_10_FONT_ID, para.c_str(), contentW, room);
    for (size_t i = 0; i < wrapped.size(); ++i) {
      if (static_cast<int>(lines.size()) >= kMaxBodyLines) break;
      if (isSense && i > 0) {
        lines.push_back(std::string("  ") + wrapped[i]);
      } else {
        lines.push_back(std::move(wrapped[i]));
      }
    }
  }
}

void DictionarySelectionActivity::drawDefinitionPopup() {
  // Keep geometry in sync if screen metrics change mid-activity.
  layoutPopupFromContent();

  // Opaque white card; page text remains visible around the edges.
  renderer.fillRoundedRect(popupX, popupY, popupW, popupH, kCorner, Color::White);
  renderer.drawRoundedRect(popupX, popupY, popupW, popupH, 2, kCorner, true);

  // Title is the headword only; pack names live as bold section headers in the body.
  const std::string titleVis =
      renderer.truncatedText(UI_12_FONT_ID, lookupWord.c_str(), popupW - kPopupPadX * 2, EpdFontFamily::BOLD);
  const int titleW = renderer.getTextWidth(UI_12_FONT_ID, titleVis.c_str(), EpdFontFamily::BOLD);
  const int titleY = popupY + kPopupPadY;
  renderer.drawText(UI_12_FONT_ID, popupX + (popupW - titleW) / 2, titleY, titleVis.c_str(), true, EpdFontFamily::BOLD);

  const int titleLineH = renderer.getLineHeight(UI_12_FONT_ID);
  const int ruleY = titleY + titleLineH + kHeaderLineGap;
  renderer.drawLine(popupX + kPopupPadX, ruleY, popupX + popupW - kPopupPadX - 1, ruleY, true);

  // Pack headers: same UI_10 size as body, bold only (size jump was too loud).
  const int lineH = std::max(1, renderer.getLineHeight(UI_10_FONT_ID));
  bodyTop = ruleY + kTitleBodyGap;
  bodyH = popupY + popupH - kPopupPadY - bodyTop;
  visibleLines = std::max(1, bodyH / lineH);
  if (scrollLine > 0 && scrollLine + visibleLines > static_cast<int>(lines.size())) {
    scrollLine = std::max(0, static_cast<int>(lines.size()) - visibleLines);
  }

  int y = bodyTop;
  const int textX = popupX + kPopupPadX;
  const int bodyBottom = popupY + popupH - kPopupPadY;
  for (int i = scrollLine; i < static_cast<int>(lines.size()) && y + lineH <= bodyBottom + 2; ++i) {
    const std::string& line = lines[static_cast<size_t>(i)];
    if (!line.empty()) {
      const bool packHeader = line[0] == '@';
      if (packHeader) {
        const char* label = line.c_str() + 1;  // strip '@'
        renderer.drawText(UI_10_FONT_ID, textX, y, label, true, EpdFontFamily::BOLD);
      } else {
        renderer.drawText(UI_10_FONT_ID, textX, y, line.c_str(), true, EpdFontFamily::REGULAR);
      }
      y += lineH;
    } else {
      y += lineH / 2;  // tighter gap between pack sections
    }
  }

  // Same scrollbar style as chapter select; drawn in the right padding of the card.
  const int totalLines = static_cast<int>(lines.size());
  if (totalLines > visibleLines) {
    drawDefinitionScrollBar(renderer, popupX, popupW, bodyTop, bodyH, totalLines, visibleLines, scrollLine);
  }
}

void DictionarySelectionActivity::loop() {
  const int total = static_cast<int>(readingOrder.size());
  using Button = MappedInputManager::Button;

  // Definition open: Select closes; Back exits dictionary mode; Up/Down scroll definition.
  if (definitionOpen) {
    if (mappedInput.wasReleased(Button::Confirm) || mappedInput.wasReleased(Button::Power)) {
      closeDefinition();
      return;
    }
    if (mappedInput.wasReleased(Button::Back)) {
      exitDictionaryMode();
      return;
    }
    buttonNavigator.onNext([this] {
      if (scrollLine + visibleLines < static_cast<int>(lines.size())) {
        scrollLine++;
        requestUpdate();
      }
    });
    buttonNavigator.onPrevious([this] {
      if (scrollLine > 0) {
        scrollLine--;
        requestUpdate();
      }
    });
    return;
  }

  auto moveCursor = [this](const int nextOrderIdx) {
    if (nextOrderIdx == cursorIdx || nextOrderIdx < 0 || nextOrderIdx >= static_cast<int>(readingOrder.size())) return;
    const int previousPage = words[readingOrder[cursorIdx]].pageIdx;
    cursorIdx = nextOrderIdx;
    if (words[readingOrder[cursorIdx]].pageIdx != previousPage) {
      needsPageSwitch = true;
    }
    requestUpdate();
  };

  // Stay on the page where dictionary was opened: wrap within that page only.
  // Up/Down and Left/Right always move the word cursor so the user can find a word first.
  buttonNavigator.onRelease({Button::Left}, [this, &moveCursor] { moveCursor(moveHorizontal(cursorIdx, -1)); });
  buttonNavigator.onContinuous({Button::Left}, [this, &moveCursor] { moveCursor(moveHorizontal(cursorIdx, -1)); });
  buttonNavigator.onRelease({Button::Right}, [this, &moveCursor] { moveCursor(moveHorizontal(cursorIdx, +1)); });
  buttonNavigator.onContinuous({Button::Right}, [this, &moveCursor] { moveCursor(moveHorizontal(cursorIdx, +1)); });
  buttonNavigator.onRelease({Button::Down}, [this, &moveCursor] { moveCursor(moveVertical(cursorIdx, +1)); });
  buttonNavigator.onContinuous({Button::Down}, [this, &moveCursor] { moveCursor(moveVertical(cursorIdx, +1)); });
  buttonNavigator.onRelease({Button::Up}, [this, &moveCursor] { moveCursor(moveVertical(cursorIdx, -1)); });
  buttonNavigator.onContinuous({Button::Up}, [this, &moveCursor] { moveCursor(moveVertical(cursorIdx, -1)); });

  // Swallow Confirm/Power until the opening long-press (Menu → dictionary) is fully released.
  if (ignoreConfirmUntilReleased) {
    const bool confirmDown = mappedInput.isPressed(Button::Confirm) || mappedInput.isPressed(Button::Power);
    if (!confirmDown) {
      ignoreConfirmUntilReleased = false;
    }
    // Still allow Back to exit; never arm multi-select or open a definition yet.
    if (mappedInput.wasReleased(Button::Back)) {
      exitDictionaryMode();
    }
    return;
  }

  // Long-press Select starts multi-word range selection (clipping-style).
  const bool confirmHeld = mappedInput.isPressed(Button::Confirm) || mappedInput.isPressed(Button::Power);
  if (startMarkIdx < 0 && !multiSelectHoldArmed && confirmHeld &&
      mappedInput.getHeldTime() > ReaderUtils::SKIP_HOLD_MS) {
    startMarkIdx = cursorIdx;
    multiSelectHoldArmed = true;
    requestUpdate();
    return;
  }

  if (mappedInput.wasReleased(Button::Confirm) || mappedInput.wasReleased(Button::Power)) {
    if (multiSelectHoldArmed) {
      // Release after long-press only armed the range; do not look up yet.
      multiSelectHoldArmed = false;
      return;
    }
    if (startMarkIdx >= 0) {
      openDefinitionForRange();
    } else {
      openDefinitionForCursor();
    }
    return;
  }

  if (mappedInput.wasReleased(Button::Back)) {
    if (startMarkIdx >= 0) {
      startMarkIdx = -1;
      multiSelectHoldArmed = false;
      requestUpdate();
      return;
    }
    exitDictionaryMode();
  }
}

void DictionarySelectionActivity::render(RenderLock&&) {
  // Snapshot path restores a stored frame; low-memory path re-renders the page like clipping.
  if (needsPageSwitch) {
    if (!switchToPage(words[readingOrder[cursorIdx]].pageIdx)) return;
    needsPageSwitch = false;
  } else if (hasSavedBuffer) {
    restoreSavedBuffer();
  } else if (!switchToPage(currentDisplayPage)) {
    return;
  }

  if (!prewarmHighlightedWords() && renderer.isSdCardFont(renderFontId)) {
    useFallbackFont("highlight prewarm");
    if (!switchToPage(currentDisplayPage)) return;
  }
  drawHighlights();

  if (definitionOpen) {
    drawDefinitionPopup();
  }

  const auto confirmLabel =
      definitionOpen ? tr(STR_CLOSE) : (startMarkIdx >= 0 ? tr(STR_DONE) : tr(STR_SELECT));
  const auto labels = definitionOpen
                          ? mappedInput.mapLabels(tr(STR_BACK), confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN))
                          : mappedInput.mapLabels(tr(STR_BACK), confirmLabel, tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

bool DictionarySelectionActivity::switchToPage(const int pageIdx) {
  const int oldPage = section.currentPage;
  section.currentPage = startPageInSection + pageIdx;
  auto page = section.loadPageFromSectionFile();
  if (!page) {
    section.currentPage = oldPage;
    LOG_ERR("DICT", "Failed to load selection page %d", pageIdx);
    return false;
  }

  if (auto* fcm = renderer.getFontCacheManager()) {
    bool renderWithFallback = false;
    {
      auto scope = fcm->createPrewarmScope();
      page->renderText(renderer, renderFontId, marginLeft, marginTop, ReaderUtils::readerForegroundBlack());
      if (!scope.endScanAndPrewarm() && renderer.isSdCardFont(renderFontId)) {
        useFallbackFont("page prewarm");
        renderWithFallback = true;
      } else {
        renderer.clearScreen(ReaderUtils::readerBackgroundColor());
        page->render(renderer, renderFontId, marginLeft, marginTop, ReaderUtils::readerForegroundBlack());
      }
    }
    if (renderWithFallback) {
      auto fallbackScope = fcm->createPrewarmScope();
      page->renderText(renderer, renderFontId, marginLeft, marginTop, ReaderUtils::readerForegroundBlack());
      fallbackScope.endScanAndPrewarm();
      renderer.clearScreen(ReaderUtils::readerBackgroundColor());
      page->render(renderer, renderFontId, marginLeft, marginTop, ReaderUtils::readerForegroundBlack());
    }
  } else {
    renderer.clearScreen(ReaderUtils::readerBackgroundColor());
    page->render(renderer, renderFontId, marginLeft, marginTop, ReaderUtils::readerForegroundBlack());
  }

  storeCurrentBuffer();
  currentDisplayPage = pageIdx;
  return true;
}

void DictionarySelectionActivity::applyWordStyle(const WordRef& word, const bool invert) const {
  const auto textStyle = static_cast<EpdFontFamily::Style>(word.style & ~EpdFontFamily::UNDERLINE);
  const int skipX = hasEmSpace(word.text) ? renderer.getTextAdvanceX(renderFontId, "\xe2\x80\x83", textStyle) : 0;
  const int drawX = word.x + skipX;
  const int drawW = word.w - skipX;
  if (drawW <= 0) return;

  if (invert) {
    renderer.fillRect(drawX, word.y, drawW, word.h, true);
  } else {
    renderer.fillRectDither(drawX, word.y, drawW, word.h, Color::LightGray);
  }

  if (word.text.find_first_not_of(" \t") != std::string::npos) {
    const bool textBlack = !invert;
    renderer.drawText(renderFontId, drawX, word.y, hasEmSpace(word.text) ? word.text.c_str() + 3 : word.text.c_str(),
                      textBlack, textStyle);
  }
}

bool DictionarySelectionActivity::prewarmHighlightedWords() const {
  if (!renderer.isSdCardFont(renderFontId)) return true;

  auto* fcm = renderer.getFontCacheManager();
  if (!fcm) return true;

  for (auto& text : prewarmTextByStyle) {
    text.clear();
  }

  const int total = static_cast<int>(readingOrder.size());
  const int from = startMarkIdx >= 0 ? std::min(startMarkIdx, cursorIdx) : cursorIdx;
  const int to = startMarkIdx >= 0 ? std::max(startMarkIdx, cursorIdx) : cursorIdx;
  for (int oi = std::max(0, from); oi <= to && oi < total; ++oi) {
    const WordRef& w = words[readingOrder[static_cast<size_t>(oi)]];
    if (w.pageIdx != currentDisplayPage) continue;
    const uint8_t styleIdx = static_cast<uint8_t>(w.style) & 0x03;
    if (styleIdx < prewarmTextByStyle.size()) {
      prewarmTextByStyle[styleIdx] += w.text;
      prewarmTextByStyle[styleIdx].push_back(' ');
    }
  }

  bool ok = true;
  for (uint8_t styleIdx = 0; styleIdx < prewarmTextByStyle.size(); styleIdx++) {
    if (!prewarmTextByStyle[styleIdx].empty()) {
      ok =
          fcm->prewarmCache(renderFontId, prewarmTextByStyle[styleIdx].c_str(), static_cast<uint8_t>(1u << styleIdx)) &&
          ok;
    }
  }
  return ok;
}

void DictionarySelectionActivity::useFallbackFont(const char* reason) {
  if (usingFallbackFont) return;
  LOG_ERR("DICT", "SD font %d failed during %s; using fallback font %d for dictionary selection", renderFontId, reason,
          DICT_SELECTION_FALLBACK_FONT_ID);
  renderFontId = DICT_SELECTION_FALLBACK_FONT_ID;
  usingFallbackFont = true;
}

void DictionarySelectionActivity::drawHighlights() {
  const int total = static_cast<int>(readingOrder.size());
  if (startMarkIdx >= 0 && total > 0) {
    const int from = std::min(startMarkIdx, cursorIdx);
    const int to = std::max(startMarkIdx, cursorIdx);
    for (int oi = from; oi <= to && oi < total; ++oi) {
      const WordRef& w = words[readingOrder[static_cast<size_t>(oi)]];
      if (w.pageIdx != currentDisplayPage) continue;
      // Range fill (dither); cursor end inverted for the active edge.
      applyWordStyle(w, /*invert=*/oi == cursorIdx);
    }
    return;
  }

  const WordRef& cursorWord = words[readingOrder[cursorIdx]];
  if (cursorWord.pageIdx == currentDisplayPage) {
    applyWordStyle(cursorWord, /*invert=*/true);
  }
}

int DictionarySelectionActivity::pickInitialCursorIdx() const {
  const int total = static_cast<int>(readingOrder.size());
  if (total <= 0) return 0;

  // Geometry of words on the first harvested page (pageIdx 0).
  int minY = INT_MAX;
  int maxY = INT_MIN;
  int minX = INT_MAX;
  int maxX = INT_MIN;
  int page0Count = 0;
  for (int oi = 0; oi < total; ++oi) {
    const WordRef& w = words[readingOrder[static_cast<size_t>(oi)]];
    if (w.pageIdx != 0) continue;
    ++page0Count;
    minY = std::min(minY, w.y);
    maxY = std::max(maxY, w.y + w.h);
    minX = std::min(minX, w.x);
    maxX = std::max(maxX, w.x + w.w);
  }
  if (page0Count <= 0) return 0;

  const int targetX = (minX + maxX) / 2;
  const int targetY = (minY + maxY) / 2;

  // Keep a few nearest-to-center candidates, then pick one at random so it isn't always
  // the exact same word when several sit near the middle.
  constexpr int kNear = 8;
  int nearOi[kNear];
  int nearDist[kNear];
  int nearCount = 0;

  for (int oi = 0; oi < total; ++oi) {
    const WordRef& w = words[readingOrder[static_cast<size_t>(oi)]];
    if (w.pageIdx != 0) continue;
    const int cx = w.x + w.w / 2;
    const int cy = w.y + w.h / 2;
    const int dx = cx - targetX;
    const int dy = cy - targetY;
    const int d = dx * dx + dy * dy;

    if (nearCount < kNear) {
      nearOi[nearCount] = oi;
      nearDist[nearCount] = d;
      ++nearCount;
    } else {
      int worst = 0;
      for (int i = 1; i < kNear; ++i) {
        if (nearDist[i] > nearDist[worst]) worst = i;
      }
      if (d < nearDist[worst]) {
        nearOi[worst] = oi;
        nearDist[worst] = d;
      }
    }
  }

  if (nearCount <= 0) return 0;
  // Sort ascending by distance (small n).
  for (int i = 0; i < nearCount - 1; ++i) {
    for (int j = i + 1; j < nearCount; ++j) {
      if (nearDist[j] < nearDist[i]) {
        std::swap(nearDist[i], nearDist[j]);
        std::swap(nearOi[i], nearOi[j]);
      }
    }
  }
  // Prefer the closest half of the near set for a center-biased random pick.
  const int pool = std::max(1, (nearCount + 1) / 2);
  return nearOi[static_cast<int>(random(0, pool))];
}

int DictionarySelectionActivity::moveHorizontal(const int orderIdx, const int direction) const {
  // direction: +1 next in reading order, -1 previous. Stays on the same pageIdx; wraps.
  const int total = static_cast<int>(readingOrder.size());
  if (total <= 0 || orderIdx < 0 || orderIdx >= total) return orderIdx;

  const int page = words[readingOrder[static_cast<size_t>(orderIdx)]].pageIdx;

  // First and last reading-order indices on this page.
  int firstOnPage = -1;
  int lastOnPage = -1;
  for (int oi = 0; oi < total; ++oi) {
    if (words[readingOrder[static_cast<size_t>(oi)]].pageIdx != page) continue;
    if (firstOnPage < 0) firstOnPage = oi;
    lastOnPage = oi;
  }
  if (firstOnPage < 0) return orderIdx;

  if (direction > 0) {
    for (int oi = orderIdx + 1; oi <= lastOnPage; ++oi) {
      if (words[readingOrder[static_cast<size_t>(oi)]].pageIdx == page) return oi;
    }
    return firstOnPage;  // wrap to first word on this page
  }

  for (int oi = orderIdx - 1; oi >= firstOnPage; --oi) {
    if (words[readingOrder[static_cast<size_t>(oi)]].pageIdx == page) return oi;
  }
  return lastOnPage;  // wrap to last word on this page
}

int DictionarySelectionActivity::moveVertical(const int orderIdx, const int direction) const {
  // direction: +1 = down (next line), -1 = up (previous line).
  // Same page only: wrap top↔bottom of the page the cursor is on.
  // Stays in column: among words on the target line, pick closest horizontal center.
  const int total = static_cast<int>(readingOrder.size());
  if (total <= 0 || orderIdx < 0 || orderIdx >= total) return orderIdx;

  const WordRef& cur = words[readingOrder[static_cast<size_t>(orderIdx)]];
  const int page = cur.pageIdx;
  const int curCx = cur.x + cur.w / 2;

  bool foundLine = false;
  int targetY = cur.y;

  if (direction > 0) {
    // Next line on this page only (smallest y > current.y).
    int bestY = INT_MAX;
    for (int oi = 0; oi < total; ++oi) {
      const WordRef& w = words[readingOrder[static_cast<size_t>(oi)]];
      if (w.pageIdx != page || w.y <= cur.y) continue;
      if (w.y < bestY) {
        bestY = w.y;
        foundLine = true;
      }
    }
    if (!foundLine) {
      // Wrap to first line of this page (smallest y).
      bestY = INT_MAX;
      for (int oi = 0; oi < total; ++oi) {
        const WordRef& w = words[readingOrder[static_cast<size_t>(oi)]];
        if (w.pageIdx != page) continue;
        if (w.y < bestY) {
          bestY = w.y;
          foundLine = true;
        }
      }
    }
    if (foundLine) targetY = bestY;
  } else {
    // Previous line on this page only (largest y < current.y).
    int bestY = INT_MIN;
    for (int oi = 0; oi < total; ++oi) {
      const WordRef& w = words[readingOrder[static_cast<size_t>(oi)]];
      if (w.pageIdx != page || w.y >= cur.y) continue;
      if (w.y > bestY) {
        bestY = w.y;
        foundLine = true;
      }
    }
    if (!foundLine) {
      // Wrap to last line of this page (largest y).
      bestY = INT_MIN;
      for (int oi = 0; oi < total; ++oi) {
        const WordRef& w = words[readingOrder[static_cast<size_t>(oi)]];
        if (w.pageIdx != page) continue;
        if (w.y > bestY) {
          bestY = w.y;
          foundLine = true;
        }
      }
    }
    if (foundLine) targetY = bestY;
  }

  if (!foundLine) return orderIdx;

  int bestOi = orderIdx;
  int bestDist = INT_MAX;
  for (int oi = 0; oi < total; ++oi) {
    const WordRef& w = words[readingOrder[static_cast<size_t>(oi)]];
    if (w.pageIdx != page || w.y != targetY) continue;
    const int cx = w.x + w.w / 2;
    const int d = std::abs(cx - curCx);
    if (d < bestDist) {
      bestDist = d;
      bestOi = oi;
    }
  }
  return bestOi;
}
