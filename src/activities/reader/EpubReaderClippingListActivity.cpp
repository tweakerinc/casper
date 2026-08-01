#include "EpubReaderClippingListActivity.h"
#include "util/UiGhostPolicy.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstdio>

#include "MappedInputManager.h"
#include "activities/ActivityResult.h"
#include "activities/home/FileBrowserActionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int ROW_HEIGHT = 56;
constexpr int LIST_START_Y = 60;
constexpr int DETAIL_START_Y = 70;
constexpr int DETAIL_SIDE_MARGIN = 20;
constexpr int DETAIL_BOTTOM_RESERVE = 55;
constexpr int DETAIL_LINE_GAP = 6;
constexpr unsigned long CLIPPING_DELETE_HOLD_MS = 500;

bool isUtf8SpaceAt(const std::string& text, const size_t index, size_t& advance) {
  const auto c = static_cast<unsigned char>(text[index]);
  if (c == 0xC2 && index + 1 < text.size() && static_cast<unsigned char>(text[index + 1]) == 0xA0) {
    advance = 2;
    return true;
  }
  if (c == 0xE2 && index + 2 < text.size() && static_cast<unsigned char>(text[index + 1]) == 0x80) {
    const auto c2 = static_cast<unsigned char>(text[index + 2]);
    if (c2 == 0x83 || c2 == 0xAF) {
      advance = 3;
      return true;
    }
  }
  return false;
}

void buildOneLineSnippetText(const std::string& text, std::string& out) {
  out.clear();
  bool lastWasSpace = true;
  for (size_t i = 0; i < text.size();) {
    size_t advance = 0;
    if (isUtf8SpaceAt(text, i, advance)) {
      if (!lastWasSpace) {
        out += ' ';
        lastWasSpace = true;
      }
      i += advance;
      continue;
    }

    const char c = text[i++];
    if (c == '\r' || c == '\n' || c == '\t') {
      if (!lastWasSpace) {
        out += ' ';
        lastWasSpace = true;
      }
      continue;
    }
    out += c;
    lastWasSpace = c == ' ';
  }
  while (!out.empty() && out.back() == ' ') {
    out.pop_back();
  }
}

size_t utf8CharLen(const std::string& text, const size_t index) {
  const auto c = static_cast<unsigned char>(text[index]);
  if ((c & 0x80) == 0) return 1;
  if ((c & 0xE0) == 0xC0 && index + 1 < text.size()) return 2;
  if ((c & 0xF0) == 0xE0 && index + 2 < text.size()) return 3;
  if ((c & 0xF8) == 0xF0 && index + 3 < text.size()) return 4;
  return 1;
}

void appendLongWordLines(const GfxRenderer& renderer, const int fontId, const std::string& word, const int maxWidth,
                         std::vector<std::string>& out) {
  std::string line;
  for (size_t i = 0; i < word.size();) {
    const size_t charLen = utf8CharLen(word, i);
    const std::string next = word.substr(i, charLen);
    const std::string candidate = line + next;
    if (!line.empty() && renderer.getTextWidth(fontId, candidate.c_str()) > maxWidth) {
      out.push_back(line);
      line = next;
    } else {
      line = candidate;
    }
    i += charLen;
  }
  if (!line.empty()) out.push_back(line);
}

void appendWrappedWord(const GfxRenderer& renderer, const int fontId, const std::string& word, const int maxWidth,
                       std::string& currentLine, std::vector<std::string>& out) {
  if (word.empty()) return;

  if (renderer.getTextWidth(fontId, word.c_str()) > maxWidth) {
    if (!currentLine.empty()) {
      out.push_back(currentLine);
      currentLine.clear();
    }
    appendLongWordLines(renderer, fontId, word, maxWidth, out);
    return;
  }

  const std::string candidate = currentLine.empty() ? word : currentLine + " " + word;
  if (renderer.getTextWidth(fontId, candidate.c_str()) <= maxWidth) {
    currentLine = candidate;
    return;
  }

  if (!currentLine.empty()) out.push_back(currentLine);
  currentLine = word;
}

void buildWrappedDetailLines(const GfxRenderer& renderer, const int fontId, const std::string& text, const int maxWidth,
                             std::vector<std::string>& out) {
  out.clear();
  if (maxWidth <= 0) return;

  std::string currentLine;
  size_t wordStart = 0;
  while (wordStart < text.size()) {
    while (wordStart < text.size() && text[wordStart] == ' ') {
      wordStart++;
    }
    if (wordStart >= text.size()) break;

    size_t wordEnd = wordStart;
    while (wordEnd < text.size() && text[wordEnd] != ' ') {
      wordEnd++;
    }

    appendWrappedWord(renderer, fontId, text.substr(wordStart, wordEnd - wordStart), maxWidth, currentLine, out);
    wordStart = wordEnd;
  }

  if (!currentLine.empty()) out.push_back(currentLine);
}
}  // namespace

int EpubReaderClippingListActivity::getPageItems() const {
  const auto orientation = renderer.getOrientation();
  const bool isPortraitInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterHeight = isPortraitInverted ? 50 : 0;
  const int startY = LIST_START_Y + hintGutterHeight;
  const int available = renderer.getScreenHeight() - startY - ROW_HEIGHT;
  return std::max(1, available / ROW_HEIGHT);
}

void EpubReaderClippingListActivity::onEnter() {
  Activity::onEnter();
  selectedIndex = 0;
  detailText.reserve(CLIPPING_TEXT_MAX);
  detailLines.reserve(32);
  requestUpdate();
}

int EpubReaderClippingListActivity::getDetailTextWidth() const {
  const auto orientation = renderer.getOrientation();
  const bool isLandscapeCw = orientation == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const int hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? 30 : 0;
  return std::max(1, renderer.getScreenWidth() - hintGutterWidth - DETAIL_SIDE_MARGIN * 2);
}

int EpubReaderClippingListActivity::getDetailLinesPerPage() const {
  const auto orientation = renderer.getOrientation();
  const bool isPortraitInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterHeight = isPortraitInverted ? 50 : 0;
  const int lineStep = renderer.getLineHeight(UI_10_FONT_ID) + DETAIL_LINE_GAP;
  const int available = renderer.getScreenHeight() - (DETAIL_START_Y + hintGutterHeight) - DETAIL_BOTTOM_RESERVE;
  return std::max(1, available / std::max(1, lineStep));
}

int EpubReaderClippingListActivity::getDetailPageCount() const {
  if (detailLinesPerPage <= 0) return 1;
  return std::max(1, static_cast<int>((detailLines.size() + detailLinesPerPage - 1) / detailLinesPerPage));
}

void EpubReaderClippingListActivity::closeDetail() {
  detailMode = false;
  detailPage = 0;
  detailText.clear();
  detailLines.clear();
  detailLayoutWidth = 0;
  detailLinesPerPage = 0;
  requestUpdate();
}

void EpubReaderClippingListActivity::jumpToSelectedClipping() {
  if (selectedIndex < 0 || selectedIndex >= static_cast<int>(CLIPPINGS.clippingCount())) return;

  const Clipping* clipping = CLIPPINGS.clippingAt(static_cast<size_t>(selectedIndex));
  if (!clipping) return;
  setResult(ClippingJumpResult{clipping->spineIndex, clipping->startPage, clipping->pageCount, clipping->paragraphIndex,
                               static_cast<uint16_t>(selectedIndex)});
  finish();
}

void EpubReaderClippingListActivity::openSelectedDetail() {
  if (selectedIndex < 0 || selectedIndex >= static_cast<int>(CLIPPINGS.clippingCount())) return;

  std::string text;
  text.reserve(CLIPPING_TEXT_MAX);
  if (!CLIPPINGS.readClippingText(static_cast<size_t>(selectedIndex), text)) {
    text.clear();
  }
  buildOneLineSnippetText(text, detailText);
  detailMode = true;
  detailPage = 0;
  detailLayoutWidth = 0;
  detailLinesPerPage = 0;
  rebuildDetailLayoutIfNeeded();
  requestUpdate();
}

void EpubReaderClippingListActivity::rebuildDetailLayoutIfNeeded() {
  const int textWidth = getDetailTextWidth();
  const int linesPerPage = getDetailLinesPerPage();
  if (textWidth == detailLayoutWidth && linesPerPage == detailLinesPerPage && !detailLines.empty()) return;

  buildWrappedDetailLines(renderer, UI_10_FONT_ID, detailText, textWidth, detailLines);
  if (detailLines.empty()) detailLines.push_back("");
  detailLayoutWidth = textWidth;
  detailLinesPerPage = linesPerPage;
  detailPage = std::min(detailPage, getDetailPageCount() - 1);
}

void EpubReaderClippingListActivity::deleteSelectedClipping() {
  if (selectedIndex < 0 || selectedIndex >= static_cast<int>(CLIPPINGS.clippingCount())) return;

  if (!CLIPPINGS.removeClippingAt(static_cast<size_t>(selectedIndex))) return;

  detailMode = false;
  detailText.clear();
  detailLines.clear();
  detailLayoutWidth = 0;
  detailLinesPerPage = 0;
  if (CLIPPINGS.clippingCount() == 0) {
    selectedIndex = 0;
  } else if (selectedIndex >= static_cast<int>(CLIPPINGS.clippingCount())) {
    selectedIndex = static_cast<int>(CLIPPINGS.clippingCount()) - 1;
  }
  requestUpdate();
}

void EpubReaderClippingListActivity::showClippingActionMenu(const bool ignoreInitialConfirmRelease) {
  if (selectedIndex < 0 || selectedIndex >= static_cast<int>(CLIPPINGS.clippingCount())) return;

  const Clipping* selectedClipping = CLIPPINGS.clippingAt(static_cast<size_t>(selectedIndex));
  if (!selectedClipping) return;
  const char* title = selectedClipping->chapterTitle[0] != '\0' ? selectedClipping->chapterTitle : tr(STR_CLIPPINGS);
  const uint16_t selectedSpineIndex = selectedClipping->spineIndex;
  const uint16_t selectedStartPage = selectedClipping->startPage;
  const uint16_t selectedStartWordIndex = selectedClipping->startWordIndex;
  const uint32_t selectedTimestamp = selectedClipping->timestamp;
  std::vector<FileBrowserActionActivity::MenuItem> items;
  items.reserve(1);
  items.push_back({FileBrowserAction::Delete, StrId::STR_DELETE});

  auto actionMenu = makeUniqueNoThrow<FileBrowserActionActivity>(renderer, mappedInput, title, std::move(items),
                                                                 ignoreInitialConfirmRelease);
  if (!actionMenu) {
    LOG_ERR("CLIP", "OOM: failed to allocate clipping action menu");
    return;
  }
  startActivityForResult(
      std::move(actionMenu),
      [this, selectedSpineIndex, selectedStartPage, selectedStartWordIndex,
       selectedTimestamp](const ActivityResult& result) {
        longPressConfirmHandled = false;
        if (result.isCancelled) {
          requestUpdate();
          return;
        }

        const auto* actionResult = std::get_if<FileBrowserActionResult>(&result.data);
        if (!actionResult || static_cast<FileBrowserAction>(actionResult->action) != FileBrowserAction::Delete) {
          requestUpdate();
          return;
        }

        for (size_t i = 0; i < CLIPPINGS.clippingCount(); ++i) {
          const Clipping* clipping = CLIPPINGS.clippingAt(i);
          if (!clipping) continue;
          if (clipping->spineIndex == selectedSpineIndex && clipping->startPage == selectedStartPage &&
              clipping->startWordIndex == selectedStartWordIndex && clipping->timestamp == selectedTimestamp) {
            selectedIndex = static_cast<int>(i);
            deleteSelectedClipping();
            return;
          }
        }
        requestUpdate();
      });
}

void EpubReaderClippingListActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (detailMode) {
      closeDetail();
      return;
    }
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  if (CLIPPINGS.clippingCount() > 0 && !longPressConfirmHandled &&
      mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      mappedInput.getHeldTime() >= CLIPPING_DELETE_HOLD_MS) {
    longPressConfirmHandled = true;
    showClippingActionMenu(true);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (longPressConfirmHandled) {
      longPressConfirmHandled = false;
      return;
    }
    if (CLIPPINGS.clippingCount() > 0 && selectedIndex >= 0 &&
        selectedIndex < static_cast<int>(CLIPPINGS.clippingCount())) {
      if (detailMode) {
        jumpToSelectedClipping();
      } else {
        openSelectedDetail();
      }
    }
    return;
  }

  const int total = static_cast<int>(CLIPPINGS.clippingCount());
  if (total == 0) return;

  if (detailMode) {
    rebuildDetailLayoutIfNeeded();
    const int detailPageCount = getDetailPageCount();
    buttonNavigator.onNextRelease([this, detailPageCount] {
      if (detailPage < detailPageCount - 1) {
        detailPage++;
        requestUpdate();
      }
    });
    buttonNavigator.onPreviousRelease([this] {
      if (detailPage > 0) {
        detailPage--;
        requestUpdate();
      }
    });
    buttonNavigator.onNextContinuous([this, detailPageCount] {
      if (detailPage < detailPageCount - 1) {
        detailPage++;
        requestUpdate();
      }
    });
    buttonNavigator.onPreviousContinuous([this] {
      if (detailPage > 0) {
        detailPage--;
        requestUpdate();
      }
    });
    return;
  }

  const int pageItems = getPageItems();
  buttonNavigator.onNextRelease([this, total] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, total);
    requestUpdate();
  });
  buttonNavigator.onPreviousRelease([this, total] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, total);
    requestUpdate();
  });
  buttonNavigator.onNextContinuous([this, total, pageItems] {
    selectedIndex = ButtonNavigator::nextPageIndex(selectedIndex, total, pageItems);
    requestUpdate();
  });
  buttonNavigator.onPreviousContinuous([this, total, pageItems] {
    selectedIndex = ButtonNavigator::previousPageIndex(selectedIndex, total, pageItems);
    requestUpdate();
  });
}

void EpubReaderClippingListActivity::renderDetail() {
  rebuildDetailLayoutIfNeeded();

  const auto pageWidth = renderer.getScreenWidth();
  const auto orientation = renderer.getOrientation();
  const bool isLandscapeCw = orientation == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const bool isPortraitInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? 30 : 0;
  const int contentX = isLandscapeCw ? hintGutterWidth : 0;
  const int contentWidth = pageWidth - hintGutterWidth;
  const int hintGutterHeight = isPortraitInverted ? 50 : 0;
  const int contentY = hintGutterHeight;

  const char* chapter = tr(STR_CLIPPINGS);
  const Clipping* selectedClipping =
      selectedIndex >= 0 ? CLIPPINGS.clippingAt(static_cast<size_t>(selectedIndex)) : nullptr;
  if (selectedClipping && selectedClipping->chapterTitle[0] != '\0') {
    chapter = selectedClipping->chapterTitle;
  }

  const std::string title =
      renderer.truncatedText(UI_12_FONT_ID, chapter, contentWidth - DETAIL_SIDE_MARGIN * 2, EpdFontFamily::BOLD);
  const int titleX =
      contentX + (contentWidth - renderer.getTextWidth(UI_12_FONT_ID, title.c_str(), EpdFontFamily::BOLD)) / 2;
  renderer.drawText(UI_12_FONT_ID, titleX, 15 + contentY, title.c_str(), true, EpdFontFamily::BOLD);

  const int lineStep = renderer.getLineHeight(UI_10_FONT_ID) + DETAIL_LINE_GAP;
  const int textX = contentX + DETAIL_SIDE_MARGIN;
  const int firstLine = detailPage * detailLinesPerPage;
  const int lastLine = std::min(static_cast<int>(detailLines.size()), firstLine + detailLinesPerPage);
  int y = DETAIL_START_Y + contentY;
  for (int i = firstLine; i < lastLine; i++) {
    renderer.drawText(UI_10_FONT_ID, textX, y, detailLines[i].c_str());
    y += lineStep;
  }

  const int detailPageCount = getDetailPageCount();
  if (detailPageCount > 1) {
    char pageBuf[16];
    snprintf(pageBuf, sizeof(pageBuf), "%d/%d", detailPage + 1, detailPageCount);
    const int pageLabelWidth = renderer.getTextWidth(SMALL_FONT_ID, pageBuf);
    renderer.drawText(SMALL_FONT_ID, contentX + contentWidth - DETAIL_SIDE_MARGIN - pageLabelWidth,
                      renderer.getScreenHeight() - 35, pageBuf);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_OPEN), detailPage > 0 ? tr(STR_DIR_UP) : "",
                                            detailPage < detailPageCount - 1 ? tr(STR_DIR_DOWN) : "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void EpubReaderClippingListActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto orientation = renderer.getOrientation();
  const bool isLandscapeCw = orientation == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const bool isPortraitInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? 30 : 0;
  const int contentX = isLandscapeCw ? hintGutterWidth : 0;
  const int contentWidth = pageWidth - hintGutterWidth;
  const int hintGutterHeight = isPortraitInverted ? 50 : 0;
  const int contentY = hintGutterHeight;

  if (CLIPPINGS.clippingCount() == 0) {
    const int titleX =
        contentX + (contentWidth - renderer.getTextWidth(UI_12_FONT_ID, tr(STR_CLIPPINGS), EpdFontFamily::BOLD)) / 2;
    renderer.drawText(UI_12_FONT_ID, titleX, 15 + contentY, tr(STR_CLIPPINGS), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, LIST_START_Y + contentY + 20, tr(STR_NO_CLIPPINGS));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    UiGhostPolicy::displayMenuFrame(renderer);
    return;
  }

  if (detailMode) {
    renderDetail();
    UiGhostPolicy::displayMenuFrame(renderer);
    return;
  }

  const int titleX =
      contentX + (contentWidth - renderer.getTextWidth(UI_12_FONT_ID, tr(STR_CLIPPINGS), EpdFontFamily::BOLD)) / 2;
  renderer.drawText(UI_12_FONT_ID, titleX, 15 + contentY, tr(STR_CLIPPINGS), true, EpdFontFamily::BOLD);

  const int pageItems = getPageItems();
  const int total = static_cast<int>(CLIPPINGS.clippingCount());
  const int pageStartIndex = (selectedIndex / pageItems) * pageItems;
  const int marginLeft = contentX + 20;
  std::string clippingText;
  std::string snippetText;
  clippingText.reserve(CLIPPING_TEXT_MAX);
  snippetText.reserve(CLIPPING_TEXT_MAX);

  for (int i = 0; i < pageItems; i++) {
    const int itemIndex = pageStartIndex + i;
    if (itemIndex >= total) break;

    const int rowY = LIST_START_Y + contentY + i * ROW_HEIGHT;
    const bool isSelected = itemIndex == selectedIndex;
    if (isSelected) {
      renderer.fillRect(contentX, rowY, contentWidth - 1, ROW_HEIGHT, true);
    }

    const Clipping* clipping = CLIPPINGS.clippingAt(static_cast<size_t>(itemIndex));
    if (!clipping) continue;

    clippingText.clear();
    if (!CLIPPINGS.readClippingText(static_cast<size_t>(itemIndex), clippingText)) {
      clippingText.clear();
    }
    buildOneLineSnippetText(clippingText, snippetText);
    const std::string snippetTrunc = renderer.truncatedText(UI_10_FONT_ID, snippetText.c_str(), contentWidth - 40);
    renderer.drawText(UI_10_FONT_ID, marginLeft, rowY + 5, snippetTrunc.c_str(), !isSelected);

    const char* chapter = clipping->chapterTitle[0] != '\0' ? clipping->chapterTitle : tr(STR_UNKNOWN_CHAPTER);
    const std::string chapterTrunc = renderer.truncatedText(SMALL_FONT_ID, chapter, contentWidth - 40);
    renderer.drawText(SMALL_FONT_ID, marginLeft, rowY + 31, chapterTrunc.c_str(), !isSelected);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_OPEN), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  UiGhostPolicy::displayMenuFrame(renderer);
}
