#include "ClipSelectionActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>

#include "MappedInputManager.h"
#include "activities/ActivityResult.h"
#include "clippings/ClipTextBuilder.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/UiGhostPolicy.h"

namespace {

constexpr int CLIP_SELECTION_FALLBACK_FONT_ID = UI_12_FONT_ID;

bool hasEmSpace(const std::string& text) {
  return text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xE2 &&
         static_cast<unsigned char>(text[1]) == 0x80 && static_cast<unsigned char>(text[2]) == 0x83;
}

}  // namespace

ClipSelectionActivity::ClipSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                             std::vector<WordRef> words, const int fontId, Section& section,
                                             const int startPageInSection, const int marginTop, const int marginLeft)
    : Activity("ClipSelection", renderer, mappedInput),
      words(std::move(words)),
      renderFontId(fontId),
      section(section),
      startPageInSection(startPageInSection),
      marginTop(marginTop),
      marginLeft(marginLeft) {}

void ClipSelectionActivity::onEnter() {
  Activity::onEnter();

  if (words.empty()) {
    LOG_ERR("CLIP", "No words available for selection");
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }
  buildReadingOrder();
  if (readingOrderSize == 0) {
    LOG_ERR("CLIP", "No readable word order available");
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }
  cursorIdx = 0;

  savedSectionPage = section.currentPage;
  if (!allocateSavedBuffer()) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  if (!switchToPage(0)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  // Long-Press Back/Menu → Clipping Tool: the open key may still be held, or the
  // release may already be latched after a slow harvest. Always arm until idle
  // (and a short time fence) so residual edges cannot cancel or Select.
  using Button = MappedInputManager::Button;
  ignoreBackUntilReleased = true;
  ignoreConfirmUntilReleased = true;
  openGuardUntilMs = millis() + 450UL;
  (void)mappedInput.wasPressed(Button::Back);
  (void)mappedInput.wasReleased(Button::Back);
  (void)mappedInput.wasPressed(Button::Confirm);
  (void)mappedInput.wasReleased(Button::Confirm);

  requestUpdate();
}

void ClipSelectionActivity::onExit() {
  section.currentPage = savedSectionPage;
  resetSavedBufferChunks();
  hasSavedBuffer = false;
  if (usingFallbackFont) {
    if (auto* fcm = renderer.getFontCacheManager()) {
      fcm->clearCache();
    }
  }
  Activity::onExit();
}

bool ClipSelectionActivity::allocateSavedBuffer() {
  // Snapshot is optional. Under pressure (issue #4) always re-render pages instead
  // of holding a second full frame buffer + word list + SD font mini data.
  savedBufferSize = 0;
  savedBufferChunkCount = 0;
  hasSavedBuffer = false;

  const size_t fbSize = renderer.getBufferSize();
  const uint8_t* fb = renderer.getFrameBuffer();
  if (fb == nullptr || fbSize == 0) {
    LOG_DBG("CLIP", "No framebuffer; clip UI will re-render pages");
    return true;
  }

  const uint32_t maxAlloc = ESP.getMaxAllocHeap();
  const uint32_t freeHeap = ESP.getFreeHeap();
  // Need contiguous room for one chunk + headroom for page load / paint.
  if (maxAlloc < BUFFER_CHUNK_SIZE + 24U * 1024U || freeHeap < fbSize + 40U * 1024U) {
    LOG_DBG("CLIP", "Skip FB snapshot free=%u maxAlloc=%u fb=%u", freeHeap, maxAlloc,
            static_cast<unsigned>(fbSize));
    return true;
  }

  const size_t chunkCount = (fbSize + BUFFER_CHUNK_SIZE - 1) / BUFFER_CHUNK_SIZE;
  if (chunkCount > savedBufferChunks.size()) {
    LOG_DBG("CLIP", "FB %u too large for snapshot (%u chunks); re-render", static_cast<unsigned>(fbSize),
            static_cast<unsigned>(chunkCount));
    return true;
  }

  savedBufferSize = fbSize;
  for (size_t i = 0; i < chunkCount; i++) {
    const size_t offset = i * BUFFER_CHUNK_SIZE;
    const size_t chunkSize = std::min(BUFFER_CHUNK_SIZE, savedBufferSize - offset);
    auto chunk = makeUniqueNoThrow<uint8_t[]>(chunkSize);
    if (!chunk) {
      LOG_ERR("CLIP", "OOM: snapshot chunk %u; re-render fallback", static_cast<unsigned>(i));
      resetSavedBufferChunks();
      savedBufferSize = 0;
      return true;
    }
    savedBufferChunks[i] = std::move(chunk);
  }
  savedBufferChunkCount = chunkCount;
  return true;
}

void ClipSelectionActivity::resetSavedBufferChunks() {
  for (auto& chunk : savedBufferChunks) {
    chunk.reset();
  }
  savedBufferChunkCount = 0;
}

void ClipSelectionActivity::storeCurrentBuffer() {
  if (savedBufferChunkCount == 0 || savedBufferSize == 0) {
    hasSavedBuffer = false;
    return;
  }

  const uint8_t* frameBuffer = renderer.getFrameBuffer();
  if (frameBuffer == nullptr) {
    hasSavedBuffer = false;
    return;
  }
  for (size_t i = 0; i < savedBufferChunkCount; i++) {
    if (!savedBufferChunks[i]) {
      hasSavedBuffer = false;
      return;
    }
    const size_t offset = i * BUFFER_CHUNK_SIZE;
    const size_t chunkSize = std::min(BUFFER_CHUNK_SIZE, savedBufferSize - offset);
    memcpy(savedBufferChunks[i].get(), frameBuffer + offset, chunkSize);
  }
  hasSavedBuffer = true;
}

void ClipSelectionActivity::restoreSavedBuffer() const {
  if (!hasSavedBuffer || savedBufferChunkCount == 0) return;

  uint8_t* frameBuffer = renderer.getFrameBuffer();
  if (frameBuffer == nullptr) return;
  for (size_t i = 0; i < savedBufferChunkCount; i++) {
    if (!savedBufferChunks[i]) return;
    const size_t offset = i * BUFFER_CHUNK_SIZE;
    const size_t chunkSize = std::min(BUFFER_CHUNK_SIZE, savedBufferSize - offset);
    memcpy(frameBuffer + offset, savedBufferChunks[i].get(), chunkSize);
  }
}

void ClipSelectionActivity::buildReadingOrder() {
  readingOrderSize = 0;

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
        if (readingOrderSize >= readingOrder.size()) {
          LOG_ERR("CLIP", "Reading order cap hit (%u words); clipping range truncated",
                  static_cast<unsigned>(readingOrder.size()));
          return;
        }
        readingOrder[readingOrderSize++] = static_cast<uint16_t>(i);
      }
    } else {
      for (int i = lineStart; i < lineEnd; ++i) {
        if (readingOrderSize >= readingOrder.size()) {
          LOG_ERR("CLIP", "Reading order cap hit (%u words); clipping range truncated",
                  static_cast<unsigned>(readingOrder.size()));
          return;
        }
        readingOrder[readingOrderSize++] = static_cast<uint16_t>(i);
      }
    }
    lineStart = lineEnd;
  }
}

void ClipSelectionActivity::loop() {
  const int total = static_cast<int>(readingOrderSize);
  using Button = MappedInputManager::Button;

  // Drain residual open-gesture (long-press Back/Confirm) before treating edges.
  // Always consume wasReleased while latching — release may have fired before
  // onEnter (slow word harvest) when isPressed was already false.
  if (ignoreBackUntilReleased) {
    if (!mappedInput.isPressed(Button::Back)) {
      ignoreBackUntilReleased = false;
    }
    (void)mappedInput.wasReleased(Button::Back);
    (void)mappedInput.wasPressed(Button::Back);
  }
  if (ignoreConfirmUntilReleased) {
    if (!mappedInput.isPressed(Button::Confirm)) {
      ignoreConfirmUntilReleased = false;
    }
    (void)mappedInput.wasReleased(Button::Confirm);
    (void)mappedInput.wasPressed(Button::Confirm);
  }
  const bool openGuardActive = openGuardUntilMs != 0 && millis() < openGuardUntilMs;
  if (openGuardActive) {
    (void)mappedInput.wasReleased(Button::Back);
    (void)mappedInput.wasReleased(Button::Confirm);
  } else {
    openGuardUntilMs = 0;
  }
  // While waiting for the open hold / guard to end, still allow L/R/U/D navigation.
  const bool confirmReady = !ignoreConfirmUntilReleased && !openGuardActive;
  const bool backReady = !ignoreBackUntilReleased && !openGuardActive;

  auto moveCursor = [this](const int nextOrderIdx) {
    if (nextOrderIdx == cursorIdx || nextOrderIdx < 0 || nextOrderIdx >= static_cast<int>(readingOrderSize)) return;
    const int previousPage = words[readingOrder[cursorIdx]].pageIdx;
    cursorIdx = nextOrderIdx;
    if (words[readingOrder[cursorIdx]].pageIdx != previousPage) {
      needsPageSwitch = true;
    }
    requestUpdate();
  };

  buttonNavigator.onRelease({Button::Left}, [this, &moveCursor] {
    if (cursorIdx > 0) moveCursor(cursorIdx - 1);
  });
  buttonNavigator.onContinuous({Button::Left}, [this, &moveCursor] {
    if (cursorIdx > 0) moveCursor(cursorIdx - 1);
  });
  buttonNavigator.onRelease({Button::Right}, [this, total, &moveCursor] {
    if (cursorIdx + 1 < total) moveCursor(cursorIdx + 1);
  });
  buttonNavigator.onContinuous({Button::Right}, [this, total, &moveCursor] {
    if (cursorIdx + 1 < total) moveCursor(cursorIdx + 1);
  });
  buttonNavigator.onRelease({Button::Down}, [this, &moveCursor] { moveCursor(lineEndForward(cursorIdx)); });
  buttonNavigator.onContinuous({Button::Down}, [this, &moveCursor] { moveCursor(lineEndForward(cursorIdx)); });
  buttonNavigator.onRelease({Button::Up}, [this, &moveCursor] { moveCursor(lineEndBackward(cursorIdx)); });
  buttonNavigator.onContinuous({Button::Up}, [this, &moveCursor] { moveCursor(lineEndBackward(cursorIdx)); });

  if (confirmReady && mappedInput.wasReleased(Button::Confirm)) {
    if (startMarkIdx == -1) {
      startMarkIdx = cursorIdx;
      requestUpdate();
    } else {
      const int from = std::min(startMarkIdx, cursorIdx);
      const int to = std::max(startMarkIdx, cursorIdx);
      auto result =
          ClipTextBuilder::build(words, readingOrder.data(), from, to, total, startPageInSection, section.pageCount);
      if (const auto paragraphIndex = section.getParagraphIndexForPage(result.sectionPage)) {
        result.paragraphIndex = *paragraphIndex;
      }
      setResult(std::move(result));
      finish();
    }
    return;
  }

  if (backReady && mappedInput.wasReleased(Button::Back)) {
    if (startMarkIdx != -1) {
      startMarkIdx = -1;
      requestUpdate();
      return;
    }

    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
  }
}

void ClipSelectionActivity::render(RenderLock&&) {
  if (needsPageSwitch) {
    switchToPage(words[readingOrder[cursorIdx]].pageIdx);
    needsPageSwitch = false;
  } else if (hasSavedBuffer) {
    restoreSavedBuffer();
  } else if (!switchToPage(currentDisplayPage)) {
    return;
  }

  drawHighlights();

  // mapLabels(back, confirm, previous, next): previous/next caption the *Up/Down*
  // functions; Left/Right always get true L/R names. Passing Left/Right here made
  // remapped front Up/Down show "Left"/"Right" while moving by line (clipping
  // uses Up/Down for line jump, Left/Right for word step — same as dictionary).
  const auto confirmLabel = startMarkIdx == -1 ? tr(STR_SELECT) : tr(STR_DONE);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  UiGhostPolicy::displayMenuFrame(renderer);
}

bool ClipSelectionActivity::switchToPage(const int pageIdx) {
  const int oldPage = section.currentPage;
  section.currentPage = startPageInSection + pageIdx;
  auto page = section.loadPage(section.currentPage);
  if (!page) {
    section.currentPage = oldPage;
    LOG_ERR("CLIP", "Failed to load selection page %d", pageIdx);
    return false;
  }

  // Single paint only — no SD mini-kern prewarm scan (issue #4 OOM path).
  // Highlights are dither overlays on snapshotted or re-rendered glyphs.
  if (renderer.isSdCardFont(renderFontId) && ESP.getMaxAllocHeap() < 28U * 1024U) {
    useFallbackFont("switchToPage low maxAlloc");
  }

  renderer.clearScreen();
  page->render(renderer, renderFontId, marginLeft, marginTop);

  storeCurrentBuffer();
  currentDisplayPage = pageIdx;
  return true;
}

void ClipSelectionActivity::applyWordStyle(const WordRef& word, const ClipWordStyle& style) const {
  const auto textStyle = static_cast<EpdFontFamily::Style>(word.style & ~EpdFontFamily::UNDERLINE);
  const int skipX = hasEmSpace(word.text) ? renderer.getTextAdvanceX(renderFontId, "\xe2\x80\x83", textStyle) : 0;
  const int drawX = word.x + skipX;
  const int drawW = word.w - skipX;
  if (drawW <= 0) return;

  const bool fill = (style.flags & ClipWordStyle::FILL) != 0;
  if (fill) {
    // Add the dither's black pixels without clearing the word's existing black
    // pixels. This preserves the snapshotted glyphs and avoids a font-cache
    // allocation/redraw on every selection step.
    for (int y = word.y; y < word.y + word.h; y += 2) {
      for (int x = drawX; x < drawX + drawW; x += 2) {
        renderer.drawPixel(x, y, true);
      }
    }
  }

  if ((style.flags & ClipWordStyle::BORDER) != 0) {
    renderer.drawRect(drawX, word.y, drawW, word.h, true);
  }

  if ((style.flags & ClipWordStyle::UNDERLINE) != 0) {
    const int underlineY = word.y + renderer.getFontAscenderSize(renderFontId) + 2;
    renderer.drawLine(drawX, underlineY, drawX + drawW, underlineY, true);
  }
}

void ClipSelectionActivity::useFallbackFont(const char* reason) {
  if (usingFallbackFont) return;
  LOG_ERR("CLIP", "SD font %d failed during %s; using fallback font %d for clipping selection", renderFontId, reason,
          CLIP_SELECTION_FALLBACK_FONT_ID);
  renderFontId = CLIP_SELECTION_FALLBACK_FONT_ID;
  usingFallbackFont = true;
}

void ClipSelectionActivity::drawHighlights() {
  static constexpr ClipWordStyle selectionStyle{ClipWordStyle::FILL | ClipWordStyle::UNDERLINE};
  static constexpr ClipWordStyle cursorStyle{ClipWordStyle::BORDER | ClipWordStyle::UNDERLINE};

  if (startMarkIdx != -1) {
    const int from = std::min(startMarkIdx, cursorIdx);
    const int to = std::max(startMarkIdx, cursorIdx);
    for (int i = from; i <= to; i++) {
      const WordRef& word = words[readingOrder[i]];
      if (word.pageIdx == currentDisplayPage) {
        applyWordStyle(word, selectionStyle);
      }
    }
  }

  const WordRef& cursorWord = words[readingOrder[cursorIdx]];
  if (cursorWord.pageIdx == currentDisplayPage) {
    applyWordStyle(cursorWord, cursorStyle);
  }
}

int ClipSelectionActivity::lineEndForward(const int orderIdx) const {
  const int total = static_cast<int>(readingOrderSize);
  const WordRef& current = words[readingOrder[orderIdx]];
  for (int i = orderIdx + 1; i < total; ++i) {
    const WordRef& word = words[readingOrder[i]];
    if (word.pageIdx != current.pageIdx || word.y != current.y) return i;
  }
  return orderIdx;
}

int ClipSelectionActivity::lineEndBackward(const int orderIdx) const {
  const WordRef& current = words[readingOrder[orderIdx]];
  int i = orderIdx - 1;
  for (; i >= 0; --i) {
    const WordRef& word = words[readingOrder[i]];
    if (word.pageIdx != current.pageIdx || word.y != current.y) break;
  }
  if (i < 0) return orderIdx;

  const WordRef& previous = words[readingOrder[i]];
  int first = i;
  for (; i >= 0; --i) {
    const WordRef& word = words[readingOrder[i]];
    if (word.pageIdx != previous.pageIdx || word.y != previous.y) break;
    first = i;
  }
  return first;
}
