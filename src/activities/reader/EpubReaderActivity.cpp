#include "EpubReaderActivity.h"

#include <Epub/Page.h>
#include <Epub/blocks/TextBlock.h>
#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <esp_system.h>

#include <array>
#include <algorithm>
#include <functional>
#include <iterator>
#include <limits>

#include "../../util/BookmarkFile.h"
#include "BookmarkEntry.h"
#include "ClipSelectionActivity.h"
#include "activities/home/BookActions.h"
#include "ClippingStore.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "BookStatsActivity.h"
#include "DictionaryWordSelectActivity.h"
#include "EpubReaderBookmarksActivity.h"
#include "EpubReaderChapterSelectionActivity.h"
#include "EpubReaderClippingListActivity.h"
#include "EpubReaderFootnotesActivity.h"
#include "EpubReaderPercentSelectionActivity.h"
#include "EpubReaderUtils.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderSyncActivity.h"
#include "MappedInputManager.h"
#include "ProgressMapper.h"
#include "QrDisplayActivity.h"
#include "ReaderActivity.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "WordRef.h"
#include "clippings/ClippingsManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "activities/settings/TextSettingsActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "SdCardFontSystem.h"
#include "util/BookmarkUtil.h"
#include "util/DictionaryRegistry.h"
#include "util/ScreenshotUtil.h"

#include <cctype>
#include <cstring>

namespace {
// pagesPerRefresh now comes from SETTINGS.getRefreshFrequency()
// pages per minute, first item is 1 to prevent division by zero if accessed
constexpr int PAGE_TURN_RATES[] = {1, 1, 3, 6, 12};
constexpr size_t initialBookmarkCacheCapacity = 16;
constexpr float bookmarkProgressEpsilon = 0.0001f;

int clampPercent(int percent) {
  if (percent < 0) {
    return 0;
  }
  if (percent > 100) {
    return 100;
  }
  return percent;
}

// SD card folder finished books are moved into. Single source of truth for the path.
// constexpr ⇒ lives in flash .rodata, no DRAM cost.
constexpr char READ_FOLDER[] = "/read";

// True if path is inside READ_FOLDER (starts with "<READ_FOLDER>/"). Non-allocating so
// it is cheap to call from loop(), and avoids reintroducing a separate "/Read/" literal.
bool isInReadFolder(const std::string& path) {
  constexpr size_t n = sizeof(READ_FOLDER) - 1;  // length of "/Read" (excludes NUL)
  return path.size() > n && path.compare(0, n, READ_FOLDER) == 0 && path[n] == '/';
}

struct ProgressRange {
  float start;
  float end;
};

ProgressRange getPageProgressRange(const std::shared_ptr<Epub>& epub, const int spineIndex, const int page,
                                   const int pageCount) {
  if (pageCount <= 1) {
    return {epub->calculateProgress(spineIndex, 0.0f), epub->calculateProgress(spineIndex, 1.0f)};
  }

  const float step = 1.0f / static_cast<float>(pageCount - 1);
  const float anchor = std::clamp(static_cast<float>(page) * step, 0.0f, 1.0f);
  const float start = std::max(0.0f, anchor - (step * 0.5f));
  const float end = std::min(1.0f, anchor + (step * 0.5f));
  return {epub->calculateProgress(spineIndex, start), epub->calculateProgress(spineIndex, end)};
}

bool bookmarkMatchesProgress(const BookmarkEntry& bookmark, const int spineIndex, const int page, const int pageCount,
                             const ProgressRange& pageRange) {
  if (bookmark.computedSpineIndex == spineIndex && bookmark.computedChapterPageCount == pageCount &&
      bookmark.computedChapterProgress == page) {
    return true;
  }

  const float bookmarkProgress = std::clamp(bookmark.percentage, 0.0f, 1.0f);
  return bookmarkProgress + bookmarkProgressEpsilon >= pageRange.start &&
         bookmarkProgress - bookmarkProgressEpsilon <= pageRange.end;
}

// Pick a non-colliding destination path inside /Read/ for a finished book.
// Mirrors the suffixing scheme used elsewhere: "name.epub" -> "name (2).epub", etc.
std::string buildReadFolderDestination(const std::string& srcPath) {
  const size_t lastSlash = srcPath.rfind('/');
  const std::string filename = (lastSlash != std::string::npos) ? srcPath.substr(lastSlash + 1) : srcPath;

  Storage.mkdir(READ_FOLDER);
  std::string dstPath = std::string(READ_FOLDER) + "/" + filename;
  if (!Storage.exists(dstPath.c_str())) {
    return dstPath;
  }

  const size_t dotPos = filename.rfind('.');
  const std::string base = (dotPos != std::string::npos) ? filename.substr(0, dotPos) : filename;
  const std::string ext = (dotPos != std::string::npos) ? filename.substr(dotPos) : "";
  int suffix = 2;
  do {
    dstPath = std::string(READ_FOLDER) + "/" + base + " (" + std::to_string(suffix) + ")" + ext;
    suffix++;
  } while (Storage.exists(dstPath.c_str()) && suffix < 100);
  return dstPath;
}

// Relocate a finished book and its cache dir into /read/, keep it in recents by
// repointing its entry to the new path, and repoint the resume pointer too.
// On rename failure: LOG_ERR and leave everything in place (no UI alert subsystem here).
void moveFinishedBookToReadFolder(const std::string& srcPath, const std::string& dstPath,
                                  const std::string& oldCachePath) {
  LOG_INF("ERS", "Moving finished epub: %s -> %s", srcPath.c_str(), dstPath.c_str());
  if (!Storage.rename(srcPath.c_str(), dstPath.c_str())) {
    LOG_ERR("ERS", "Failed to move finished book to '/Read' folder");
    return;
  }

  // Cache dir is keyed by hash of the epub path (see Epub ctor), so it must be re-keyed.
  const std::string newCachePath = "/.crosspoint/epub_" + std::to_string(std::hash<std::string>{}(dstPath));
  if (!oldCachePath.empty() && Storage.exists(oldCachePath.c_str())) {
    if (!Storage.rename(oldCachePath.c_str(), newCachePath.c_str())) {
      LOG_ERR("ERS", "Failed to rename cache dir %s -> %s (non-fatal)", oldCachePath.c_str(), newCachePath.c_str());
    }
  }

  // Keep the book in recents (crossink behavior): repoint the entry to its new
  // location instead of dropping it. updatePath persists on success.
  RECENT_BOOKS.updatePath(srcPath, dstPath, oldCachePath, newCachePath);
  if (APP_STATE.openEpubPath == srcPath) {
    APP_STATE.openEpubPath = dstPath;
    APP_STATE.saveToFile();
  }
}

// ---- Clipping page matching / word harvest (reader-only Phase A) ----

struct ClippingPageMatch {
  uint16_t startWord = 0;
  uint16_t endWord = 0;
};

bool hasEmSpacePrefix(const char* text) {
  return text && static_cast<unsigned char>(text[0]) == 0xE2 && static_cast<unsigned char>(text[1]) == 0x80 &&
         static_cast<unsigned char>(text[2]) == 0x83;
}

bool hasVisibleWordText(const char* text) {
  if (!text) return false;
  const char* cursor = text + (hasEmSpacePrefix(text) ? 3 : 0);
  while (*cursor) {
    if (*cursor != ' ' && *cursor != '\t' && *cursor != '\r' && *cursor != '\n') return true;
    cursor++;
  }
  return false;
}

bool isUtf8SpaceAt(const char* cursor, size_t& advance) {
  const auto c = static_cast<unsigned char>(cursor[0]);
  if (c == 0xC2 && cursor[1] != '\0' && static_cast<unsigned char>(cursor[1]) == 0xA0) {
    advance = 2;
    return true;
  }
  if (c == 0xE2 && cursor[1] != '\0' && cursor[2] != '\0' && static_cast<unsigned char>(cursor[1]) == 0x80) {
    const auto c2 = static_cast<unsigned char>(cursor[2]);
    if (c2 == 0x83 || c2 == 0xAF) {
      advance = 3;
      return true;
    }
  }
  return false;
}

bool nextClipToken(const char*& cursor, const char*& tokenStart, size_t& tokenLen) {
  while (*cursor != '\0') {
    size_t advance = 0;
    if (std::isspace(static_cast<unsigned char>(*cursor)) || isUtf8SpaceAt(cursor, advance)) {
      cursor += advance > 0 ? advance : 1;
      continue;
    }
    break;
  }
  if (*cursor == '\0') {
    tokenStart = nullptr;
    tokenLen = 0;
    return false;
  }

  tokenStart = cursor;
  while (*cursor != '\0') {
    size_t advance = 0;
    if (std::isspace(static_cast<unsigned char>(*cursor)) || isUtf8SpaceAt(cursor, advance)) {
      break;
    }
    cursor++;
  }
  tokenLen = static_cast<size_t>(cursor - tokenStart);
  return true;
}

uint16_t countClipTokens(const std::string& text) {
  uint16_t count = 0;
  const char* cursor = text.c_str();
  const char* token = nullptr;
  size_t len = 0;
  while (nextClipToken(cursor, token, len) && count < UINT16_MAX) {
    count++;
  }
  return count;
}

bool advanceClipCursorToToken(const std::string& text, const uint16_t targetIndex, const char*& cursor,
                              const char*& tokenStart, size_t& tokenLen) {
  cursor = text.c_str();
  uint16_t index = 0;
  while (nextClipToken(cursor, tokenStart, tokenLen)) {
    if (index == targetIndex) {
      return true;
    }
    index++;
  }
  tokenStart = nullptr;
  tokenLen = 0;
  return false;
}

bool wordMatchesToken(const char* word, const char* token, const size_t tokenLen) {
  if (!token || tokenLen == 0) return false;
  const char* visibleWord = word + (hasEmSpacePrefix(word) ? 3 : 0);
  return std::strlen(visibleWord) == tokenLen && std::strncmp(visibleWord, token, tokenLen) == 0;
}

template <typename Callback>
bool forEachVisiblePageWord(const Page& page, Callback&& callback) {
  uint16_t wordIndex = 0;
  for (const auto& element : page.elements) {
    if (element->getTag() != TAG_PageLine) continue;
    const auto& line = static_cast<const PageLine&>(*element);
    if (!line.getBlock()) continue;

    const auto& block = *line.getBlock();
    const uint16_t count = block.wordCount();
    for (uint16_t i = 0; i < count; ++i) {
      const char* word = block.wordText(i);
      if (!hasVisibleWordText(word)) continue;

      if (!callback(wordIndex, line, block, i)) {
        return false;
      }
      wordIndex++;
    }
  }
  return true;
}

bool matchClipRunFromPageWord(const Page& page, const std::string& clippingText, const uint16_t startPageWord,
                              const uint16_t startClipToken, const uint16_t minPartialMatch, ClippingPageMatch& match) {
  const char* cursor = nullptr;
  const char* token = nullptr;
  size_t tokenLen = 0;
  if (!advanceClipCursorToToken(clippingText, startClipToken, cursor, token, tokenLen)) {
    return false;
  }

  uint16_t matchedTokens = 0;
  uint16_t lastWord = startPageWord;
  bool reachedClipEnd = false;
  bool stoppedByMismatch = false;

  forEachVisiblePageWord(page, [&](const uint16_t wordIndex, const PageLine&, const TextBlock& block, const size_t i) {
    if (wordIndex < startPageWord) {
      return true;
    }

    const char* word = block.wordText(static_cast<uint16_t>(i));
    if (!wordMatchesToken(word, token, tokenLen)) {
      stoppedByMismatch = true;
      return false;
    }

    matchedTokens++;
    lastWord = wordIndex;
    if (!nextClipToken(cursor, token, tokenLen)) {
      reachedClipEnd = true;
      return false;
    }
    return true;
  });

  if (matchedTokens == 0 || stoppedByMismatch) {
    return false;
  }

  const bool completeClipMatch = startClipToken == 0 && reachedClipEnd;
  if (!completeClipMatch && matchedTokens < minPartialMatch) {
    const bool startsAtClipBoundary = startClipToken == 0;
    const bool startsAtPageBoundary = startPageWord == 0;
    if (!startsAtClipBoundary && !startsAtPageBoundary) {
      return false;
    }
  }

  match.startWord = startPageWord;
  match.endWord = lastWord;
  return true;
}

bool findClippingTextOnPage(const Page& page, const std::string& clippingText, ClippingPageMatch& match) {
  if (clippingText.empty()) return false;

  const uint16_t tokenCount = countClipTokens(clippingText);
  if (tokenCount == 0) return false;
  const uint16_t minPartialMatch = std::min<uint16_t>(tokenCount, 3);

  bool found = false;

  forEachVisiblePageWord(page, [&](const uint16_t wordIndex, const PageLine&, const TextBlock& block, const size_t i) {
    const char* word = block.wordText(static_cast<uint16_t>(i));
    const char* cursor = clippingText.c_str();
    const char* token = nullptr;
    size_t tokenLen = 0;
    uint16_t tokenIndex = 0;
    while (nextClipToken(cursor, token, tokenLen)) {
      if (tokenIndex >= tokenCount) {
        break;
      }
      if (wordMatchesToken(word, token, tokenLen) &&
          matchClipRunFromPageWord(page, clippingText, wordIndex, tokenIndex, minPartialMatch, match)) {
        found = true;
        return false;
      }
      tokenIndex++;
    }
    return true;
  });

  return found;
}

uint16_t countVisiblePageWords(const Page& page) {
  uint16_t count = 0;
  forEachVisiblePageWord(page, [&](const uint16_t, const PageLine&, const TextBlock&, const size_t) {
    if (count == UINT16_MAX) return false;
    count++;
    return true;
  });
  return count;
}

bool findClippingStoredRangeOnPage(const Page& page, const Clipping& clipping, const uint16_t currentPage,
                                   const uint16_t currentPageCount, ClippingPageMatch& match) {
  if (clipping.wordCount == 0 || currentPageCount == 0 || clipping.pageCount != currentPageCount) {
    return false;
  }
  if (clipping.startPage > clipping.endPage || currentPage < clipping.startPage || currentPage > clipping.endPage) {
    return false;
  }

  const uint16_t pageWordCount = countVisiblePageWords(page);
  if (pageWordCount == 0) return false;

  uint16_t startWord = 0;
  uint16_t endWord = static_cast<uint16_t>(pageWordCount - 1);
  if (currentPage == clipping.startPage) {
    if (clipping.startWordIndex >= pageWordCount) return false;
    startWord = clipping.startWordIndex;
  }
  if (currentPage == clipping.endPage) {
    if (clipping.endWordIndex >= pageWordCount) return false;
    endWord = clipping.endWordIndex;
  }
  if (startWord > endWord) return false;

  match.startWord = startWord;
  match.endWord = endWord;
  return true;
}

uint16_t clampSectionPage(const uint32_t page, const uint16_t pageCount) {
  if (pageCount == 0) return 0;
  return static_cast<uint16_t>(std::min<uint32_t>(page, pageCount - 1));
}

uint16_t approximateRelayoutPage(const Clipping& clipping, const uint16_t currentPageCount) {
  if (currentPageCount == 0) return 0;
  if (clipping.pageCount <= 1) return 0;

  const uint32_t oldLastPage = static_cast<uint32_t>(clipping.pageCount - 1);
  const uint32_t newLastPage = static_cast<uint32_t>(currentPageCount - 1);
  const uint32_t scaledPage =
      (static_cast<uint32_t>(clipping.startPage) * newLastPage + oldLastPage / 2U) / oldLastPage;
  return clampSectionPage(scaledPage, currentPageCount);
}

bool pageContainsClippingText(Section& section, const std::string& clippingText, const uint16_t page) {
  section.currentPage = page;
  auto loadedPage = section.loadPage(page);
  if (!loadedPage) return false;

  ClippingPageMatch match;
  return findClippingTextOnPage(*loadedPage, clippingText, match);
}

bool findClippingPageNear(Section& section, const std::string& clippingText, const uint16_t center,
                          const uint16_t radius, uint16_t& outPage) {
  if (section.pageCount == 0) return false;

  const uint16_t pageCount = static_cast<uint16_t>(section.pageCount);
  const uint16_t clampedCenter = clampSectionPage(center, pageCount);
  if (pageContainsClippingText(section, clippingText, clampedCenter)) {
    outPage = clampedCenter;
    return true;
  }

  for (uint16_t distance = 1; distance <= radius; ++distance) {
    if (clampedCenter >= distance) {
      const uint16_t before = static_cast<uint16_t>(clampedCenter - distance);
      if (pageContainsClippingText(section, clippingText, before)) {
        outPage = before;
        return true;
      }
    }
    const uint32_t after = static_cast<uint32_t>(clampedCenter) + distance;
    if (after < pageCount && pageContainsClippingText(section, clippingText, static_cast<uint16_t>(after))) {
      outPage = static_cast<uint16_t>(after);
      return true;
    }
  }
  return false;
}

uint16_t resolveClippingJumpPage(Section& section, const Clipping& clipping, const std::string& clippingText,
                                 const uint16_t fallbackPage) {
  constexpr uint16_t SEARCH_RADIUS = 8;
  if (section.pageCount == 0) return fallbackPage;

  const uint16_t pageCount = static_cast<uint16_t>(section.pageCount);
  uint16_t resolvedPage = clampSectionPage(fallbackPage, pageCount);
  const uint16_t approximatePage = approximateRelayoutPage(clipping, pageCount);
  if (!clippingText.empty() &&
      findClippingPageNear(section, clippingText, approximatePage, SEARCH_RADIUS, resolvedPage)) {
    return resolvedPage;
  }

  if (clipping.paragraphIndex != UINT16_MAX) {
    const auto paragraphPage = section.getPageForParagraphIndex(clipping.paragraphIndex);
    if (paragraphPage.has_value() && !clippingText.empty() &&
        findClippingPageNear(section, clippingText, clampSectionPage(*paragraphPage, pageCount), SEARCH_RADIUS,
                             resolvedPage)) {
      return resolvedPage;
    }
  }

  if (!clippingText.empty()) {
    findClippingPageNear(section, clippingText, resolvedPage, SEARCH_RADIUS, resolvedPage);
  }
  return resolvedPage;
}

uint16_t resolveParagraphJumpPage(const Section& section, const uint16_t paragraphIndex, const uint16_t fallbackPage) {
  if (section.pageCount == 0 || paragraphIndex == UINT16_MAX) return fallbackPage;

  const uint16_t pageCount = static_cast<uint16_t>(section.pageCount);
  const uint16_t clampedFallback = clampSectionPage(fallbackPage, pageCount);
  const auto paragraphPage = section.getPageForParagraphIndex(paragraphIndex);
  if (!paragraphPage.has_value()) return clampedFallback;

  const uint16_t startPage = clampSectionPage(*paragraphPage, pageCount);
  if (clampedFallback < startPage) return startPage;

  if (paragraphIndex < UINT16_MAX - 1) {
    const auto nextParagraphPage = section.getPageForParagraphIndex(static_cast<uint16_t>(paragraphIndex + 1));
    if (nextParagraphPage.has_value() && *nextParagraphPage > startPage && clampedFallback >= *nextParagraphPage) {
      return static_cast<uint16_t>(*nextParagraphPage - 1);
    }
  }

  return clampedFallback;
}

struct ReaderViewportLayout {
  int marginTop = 0;
  int marginRight = 0;
  int marginBottom = 0;
  int marginLeft = 0;
};

// Match EpubReaderActivity::render / openDictionaryWordSelect margin math so WordRef
// coordinates align with the page the user is reading.
ReaderViewportLayout computeReaderViewportLayout(GfxRenderer& renderer, const bool automaticPageTurnActive) {
  ReaderViewportLayout layout{};
  renderer.getOrientedViewableTRBL(&layout.marginTop, &layout.marginRight, &layout.marginBottom, &layout.marginLeft);
  layout.marginTop += SETTINGS.screenMargin + ReaderUtils::kReaderTopChromeExtra;
  layout.marginLeft += SETTINGS.screenMargin;
  layout.marginRight += SETTINGS.screenMargin;

  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  const int hintStrip = UITheme::getInstance().getMetrics().buttonHintsHeight;
  // Mirror top air (screenMargin + chrome extra) and never less than status bar
  // or the dictionary front-button strip so tools never cover last lines.
  int bottomReserve = SETTINGS.screenMargin + ReaderUtils::kReaderBottomChromeExtra;
  bottomReserve = std::max(bottomReserve, static_cast<int>(statusBarHeight));
  bottomReserve = std::max(bottomReserve, hintStrip);
  if (automaticPageTurnActive &&
      (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight())) {
    bottomReserve = std::max(
        bottomReserve, static_cast<int>(statusBarHeight + UITheme::getInstance().getMetrics().statusBarVerticalMargin));
  }
  layout.marginBottom += bottomReserve + ReaderUtils::kReaderBottomChromePad;
  return layout;
}

// Harvest page words for clip selection. Returns false if nothing usable.
// Cap by free heap so vector::reserve / string growth never abort() under -fno-exceptions.
bool harvestSelectableWords(GfxRenderer& renderer, Section& section, Epub& epub, const int currentSpineIndex,
                            const bool automaticPageTurnActive, ReaderViewportLayout& layout, int& readerFontId,
                            int& startPage, std::vector<WordRef>& words, std::string* bookTitle, std::string* author,
                            std::string* chapterTitle, const char* logTag, const int maxPages = 3,
                            const size_t maxWordsCap = 240, const uint32_t headroomBytes = 16U * 1024U) {
  layout = computeReaderViewportLayout(renderer, automaticPageTurnActive);
  readerFontId = SETTINGS.getReaderFontId();
  const int lineHeight = renderer.getLineHeight(readerFontId);
  startPage = section.currentPage;
  if (section.pageCount <= 0 || startPage < 0 || startPage >= section.pageCount) {
    LOG_ERR(logTag, "No pages available for word selection (page=%d count=%d)", startPage, section.pageCount);
    return false;
  }
  const int pagesToLoad = std::min(maxPages, section.pageCount - startPage);
  std::array<uint16_t, 3> pageWordCounts{};
  const size_t requested = maxWordsCap > 0 ? maxWordsCap : 240;
  const uint32_t freeHeap = ESP.getFreeHeap();
  const uint32_t maxAlloc = ESP.getMaxAllocHeap();
  constexpr size_t kBytesPerWordRef = sizeof(WordRef) + 8;
  constexpr size_t kBytesPerWordExtra = 40;
  size_t byContig = 0;
  if (maxAlloc > headroomBytes) {
    byContig = static_cast<size_t>((maxAlloc - headroomBytes) / kBytesPerWordRef);
  }
  size_t byFree = 0;
  if (freeHeap > headroomBytes * 2) {
    byFree = static_cast<size_t>((freeHeap - headroomBytes * 2) / (kBytesPerWordRef + kBytesPerWordExtra));
  }
  size_t maxSelectableWords = requested;
  if (byContig > 0) maxSelectableWords = std::min(maxSelectableWords, byContig);
  if (byFree > 0) maxSelectableWords = std::min(maxSelectableWords, byFree);
  if (maxSelectableWords < 24) {
    LOG_ERR(logTag, "Low heap for word selection (%u free, %u max alloc); skipping", freeHeap, maxAlloc);
    section.currentPage = startPage;
    return false;
  }
  if (maxSelectableWords < requested) {
    LOG_DBG(logTag, "Word harvest capped to %u (heap free=%u maxAlloc=%u)", static_cast<unsigned>(maxSelectableWords),
            freeHeap, maxAlloc);
  }
  words.clear();
  words.reserve(maxSelectableWords);
  bool wordLimitLogged = false;

  for (int pageIdx = 0; pageIdx < pagesToLoad; ++pageIdx) {
    section.currentPage = startPage + pageIdx;
    auto page = section.loadPage(section.currentPage);
    if (!page) break;

    for (const auto& element : page->elements) {
      if (element->getTag() != TAG_PageLine) continue;
      const auto& line = static_cast<const PageLine&>(*element);
      if (!line.getBlock()) continue;

      const auto& block = *line.getBlock();
      const uint16_t count = block.wordCount();
      if (renderer.isSdCardFont(readerFontId) && count > 0) {
        for (uint16_t i = 0; i < count; ++i) {
          const uint8_t styleMask = static_cast<uint8_t>(1u << (static_cast<uint8_t>(block.wordStyle(i)) & 0x03));
          renderer.ensureSdCardFontReady(readerFontId, block.wordText(i), styleMask);
        }
      }
      for (uint16_t i = 0; i < count; ++i) {
        const char* wordText = block.wordText(i);
        if (!hasVisibleWordText(wordText)) continue;
        if (words.size() >= maxSelectableWords) {
          if (!wordLimitLogged) {
            LOG_ERR(logTag, "Selectable word cap hit (%u words); range truncated",
                    static_cast<unsigned>(maxSelectableWords));
            wordLimitLogged = true;
          }
          break;
        }

        const auto textStyle = static_cast<EpdFontFamily::Style>(block.wordStyle(i) & ~EpdFontFamily::UNDERLINE);
        int wordWidth = renderer.getTextAdvanceX(readerFontId, wordText, textStyle);
        if (wordWidth <= 0) continue;

        WordRef word;
        word.x = layout.marginLeft + line.xPos + block.wordXpos(i);
        word.y = layout.marginTop + line.yPos;
        if (i + 1 < count && block.wordXpos(i + 1) > block.wordXpos(i)) {
          wordWidth = std::min(wordWidth, static_cast<int>(block.wordXpos(i + 1) - block.wordXpos(i)));
        }
        word.w = wordWidth;
        word.h = lineHeight;
        word.pageIdx = pageIdx;
        word.pageWordIndex = pageWordCounts[pageIdx]++;
        word.text = wordText;
        word.style = textStyle;
        word.endsWithInsertedHyphen = false;  // Casper TextBlock does not track inserted hyphens
        word.lineIsRtl = block.getBlockStyle().isRtl;
        words.push_back(std::move(word));
      }
      if (words.size() >= maxSelectableWords) break;
    }
    if (words.size() >= maxSelectableWords) break;
  }

  section.currentPage = startPage;

  auto endsWithHyphen = [](const std::string& word) { return !word.empty() && word.back() == '-'; };
  const int indentThreshold = renderer.getLineHeight(readerFontId) / 2;
  int previousLineFirstIdx = -1;
  for (int i = 0; i < static_cast<int>(words.size()); ++i) {
    const bool newLine = i == 0 || words[i].pageIdx != words[i - 1].pageIdx || words[i].y != words[i - 1].y;
    if (!newLine) continue;

    const bool byEmSpace = hasEmSpacePrefix(words[i].text.c_str());
    const bool byIndent = !byEmSpace && previousLineFirstIdx >= 0 &&
                          words[i].x > words[previousLineFirstIdx].x + indentThreshold &&
                          !endsWithHyphen(words[i - 1].text);
    if (byEmSpace || byIndent) {
      words[i].paragraphStart = true;
    }
    previousLineFirstIdx = i;
  }

  if (chapterTitle) {
    const int tocIndex = epub.getTocIndexForSpineIndex(currentSpineIndex);
    if (tocIndex >= 0) {
      *chapterTitle = epub.getTocItem(tocIndex).title;
    }
  }
  if (bookTitle) {
    *bookTitle = epub.getTitle();
  }
  if (author) {
    *author = epub.getAuthor();
  }
  return !words.empty();
}

}  // namespace

void EpubReaderActivity::onEnter() {
  Activity::onEnter();

  if (!epub) {
    return;
  }

  ImageBlock::clearSessionRenderFailures();
  // Lazy image extraction: section builds only header-probe images, so the first
  // render of an image page pulls the file out of the EPUB through this hook.
  ImageBlock::setExtractor(epub.get(), [](void* ctx, const char* src, const char* dest) {
    return static_cast<Epub*>(ctx)->extractItemToFile(src, dest);
  });

  // Configure screen orientation based on settings
  // NOTE: This affects layout math and must be applied before any render calls.
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

  epub->setupCacheDir();

  HalFile f;
  if (Storage.openFileForRead("ERS", epub->getCachePath() + "/progress.bin", f)) {
    uint8_t data[6];
    int dataSize = f.read(data, 6);
    if (dataSize == 4 || dataSize == 6) {
      currentSpineIndex = data[0] + (data[1] << 8);
      nextPageNumber = data[2] + (data[3] << 8);
      if (nextPageNumber == UINT16_MAX) {
        // UINT16_MAX is an in-memory navigation sentinel for "open previous
        // chapter on its last page". It should never be treated as persisted
        // resume state after sleep or reopen.
        LOG_DBG("ERS", "Ignoring stale last-page sentinel from progress cache");
        nextPageNumber = 0;
      }
      cachedSpineIndex = currentSpineIndex;
      LOG_DBG("ERS", "Loaded cache: %d, %d", currentSpineIndex, nextPageNumber);
    }
    if (dataSize == 6) {
      cachedChapterTotalPageCount = data[4] + (data[5] << 8);
    }
  }
  // We may want a better condition to detect if we are opening for the first time.
  // This will trigger if the book is re-opened at Chapter 0.
  if (currentSpineIndex == 0) {
    int textSpineIndex = epub->getSpineIndexForTextReference();
    if (textSpineIndex != 0) {
      currentSpineIndex = textSpineIndex;
      LOG_DBG("ERS", "Opened for first time, navigating to text reference at index %d", textSpineIndex);
    }
  }

  // Per-book stats needed early for session/pace; keep on open path.
  // Global stats, recents, bookmarks, clippings: after first page (see loop).
  const uint32_t tStats = millis();
  readingStats = BookReadingStats::loadForBook(epub->getPath());
  LOG_DBG("ERS", "loadForBook stats %lums", static_cast<unsigned long>(millis() - tStats));
  readingSessionStartMs = millis();
  lastPageTurnTime = readingSessionStartMs;  // dwell baseline for first forward page
  pendingOpenSideWork = true;

  // Home Read sets open hints: FAST when greys settled, defer text AA for first ink.
  // Other entry paths (file browser, sleep resume) leave defaults → HALF + full AA.
  bool preferFast = false;
  bool deferAa = false;
  uint32_t openT0 = 0;
  ReaderActivity::takeOpenHints(preferFast, deferAa, openT0);
  openPreferFastFirstRefresh = preferFast;
  openDeferTextAa = deferAa && SETTINGS.textAntiAliasing;
  openWallStartMs = openT0 != 0 ? openT0 : millis();
  openFirstInkLogged = false;
  pendingDeferredOpenAa = false;

  if (openPreferFastFirstRefresh) {
    // pagesUntilFullRefresh > 1 → FAST in displayWithRefreshCycle (not HALF clean).
    const int freq = SETTINGS.getRefreshFrequency();
    pagesUntilFullRefresh = freq > 1 ? freq : 2;
    LOG_DBG("ERS", "Open: FAST first page (greys settled) deferAa=%d", openDeferTextAa ? 1 : 0);
  } else {
    // After multipass home / settings: force HALF so BW text does not differential-ghost.
    pagesUntilFullRefresh = 1;
    LOG_DBG("ERS", "Open: HALF first page deferAa=%d", openDeferTextAa ? 1 : 0);
  }

  // Trigger first update
  requestUpdate();
}

void EpubReaderActivity::runDeferredOpenSideWork() {
  if (!pendingOpenSideWork || !epub) return;
  pendingOpenSideWork = false;

  // Last-opened + recents (SD writes) after first ink — not needed for page 1.
  APP_STATE.openEpubPath = epub->getPath();
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath());

  loadCachedBookmarks();
  CLIPPINGS.loadForBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), "epub");

  if (SETTINGS.readingStatsTrackingEnabled()) {
    globalReadingStats = GlobalReadingStats::load();
  }
}

void EpubReaderActivity::onExit() {
  // Ensure recents/app-state still update if user leaves before first paint finished.
  runDeferredOpenSideWork();

  Activity::onExit();

  // The extractor holds a raw pointer to this activity's epub; drop it before
  // the activity (and the shared_ptr) goes away.
  ImageBlock::setExtractor(nullptr, nullptr);

  // Reset orientation back to portrait for the rest of the UI
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();

  // Commit session reading stats (fail-soft). Short sessions under 1 minute do not
  // count toward session totals; under 10 seconds do not add reading time.
  // Apply any outstanding menu/dict pause so it never counts as reading time.
  // Cap the final page dwell by Session Time so leaving the book open idle
  // does not inflate totals (CrossInk-style idle threshold).
  // Skip entirely when the user has disabled stat tracking.
  if (statsPauseStartMs != 0UL) {
    resumeReadingStatsClock();
  }
  if (SETTINGS.readingStatsTrackingEnabled() && epub && readingSessionStartMs != 0) {
    const unsigned long nowMs = millis();
    uint32_t elapsedSecs =
        nowMs >= readingSessionStartMs ? static_cast<uint32_t>((nowMs - readingSessionStartMs) / 1000UL) : 0u;
    const uint32_t idleCap = SETTINGS.getReadingSessionIdleSeconds();
    // If the last page was idle longer than the threshold, drop the idle tail
    // from the session total so "session" ends when reading stopped.
    if (lastPageTurnTime != 0UL && nowMs >= lastPageTurnTime) {
      const uint32_t tailSecs = static_cast<uint32_t>((nowMs - lastPageTurnTime) / 1000UL);
      if (tailSecs > idleCap && elapsedSecs > tailSecs - idleCap) {
        elapsedSecs -= (tailSecs - idleCap);
      }
    }
    if (elapsedSecs > idleCap * 2 && lastPageTurnTime != 0UL) {
      // Still bound extreme idle-open cases: never credit more wall time than
      // one idle window past the last page activity for this simple model.
      const uint32_t maxFromLastPage =
          static_cast<uint32_t>((lastPageTurnTime - readingSessionStartMs) / 1000UL) + idleCap;
      if (lastPageTurnTime >= readingSessionStartMs && elapsedSecs > maxFromLastPage) {
        elapsedSecs = maxFromLastPage;
      }
    }
    if (elapsedSecs >= 60) {
      if (readingStats.sessionCount < UINT16_MAX) {
        readingStats.sessionCount++;
      }
      if (globalReadingStats.totalSessions < UINT32_MAX) {
        globalReadingStats.totalSessions++;
      }
    }
    if (elapsedSecs >= 10) {
      if (readingStats.totalReadingSeconds <= UINT32_MAX - elapsedSecs) {
        readingStats.totalReadingSeconds += elapsedSecs;
      } else {
        readingStats.totalReadingSeconds = UINT32_MAX;
      }
      if (globalReadingStats.totalReadingSeconds <= UINT32_MAX - elapsedSecs) {
        globalReadingStats.totalReadingSeconds += elapsedSecs;
      } else {
        globalReadingStats.totalReadingSeconds = UINT32_MAX;
      }

      ReadingStatsDateTime localStart;
      if (getCurrentLocalReadingStatsDateTime(localStart)) {
        readingStats.recordReadingSpan(localStart, elapsedSecs);
        globalReadingStats.recordReadingSpan(localStart, elapsedSecs);
        if (elapsedSecs >= 120 && !readingStats.startDateManual && !readingStats.startDate.isValid()) {
          readingStats.startDate = localStart.date;
        }
      }
    }
    // Cache smoothed book ETA so the dashboard matches the reader after exit.
    if (smoothedBookTimeLeftSeconds > 0) {
      readingStats.estimatedTimeLeftSeconds = smoothedBookTimeLeftSeconds;
    } else if (section) {
      const int chapterPages = std::max(1, static_cast<int>(section->estimatedTotalPages()));
      const int currentPage1 = section->currentPage + 1;
      const float chapterProg =
          static_cast<float>(currentPage1) / static_cast<float>(chapterPages);
      const float bookProg = epub->calculateProgress(currentSpineIndex, chapterProg);
      const float chapterStart = epub->calculateProgress(currentSpineIndex, 0.0f);
      const float chapterEnd = epub->calculateProgress(currentSpineIndex, 1.0f);
      const float remainingPages =
          estimateRemainingBookPages(chapterPages, currentPage1, bookProg, chapterStart, chapterEnd);
      const uint32_t secPerPage =
          estimateSecondsPerPage(readingStats.avgSecondsPerForwardPage, readingStats.paceSampleCount,
                                 readingStats.totalReadingSeconds, readingStats.totalPagesTurned);
      uint32_t est = 0;
      if (estimateTimeLeftFromPages(remainingPages, secPerPage, est)) {
        readingStats.estimatedTimeLeftSeconds =
            smoothTimeLeftSeconds(readingStats.estimatedTimeLeftSeconds, est);
      } else {
        readingStats.estimatedTimeLeftSeconds = 0;
      }
    }
    // Persist progress % so Home dashboard never needs epub.load() for the column.
    // End-of-book screen (spine past last item) is always 100%.
    if (epub->getSpineItemsCount() > 0 && currentSpineIndex >= epub->getSpineItemsCount()) {
      readingStats.setProgressPercent(100.0f);
      readingStats.estimatedTimeLeftSeconds = 0;
      if (!readingStats.isCompleted) {
        // Reaching the end screen counts as finished for dashboard progress.
        readingStats.isCompleted = true;
        if (SETTINGS.readingStatsTrackingEnabled() && !readingStats.finishedDateManual) {
          ReadingStatsDateTime now;
          if (getCurrentLocalReadingStatsDateTime(now)) {
            readingStats.finishedDate = now.date;
          }
        }
        if (SETTINGS.readingStatsTrackingEnabled() && globalReadingStats.completedBooks < UINT32_MAX) {
          globalReadingStats.completedBooks++;
        }
      }
    } else {
      const float newPct = getCurrentBookProgressPercent();
      const float oldPct = readingStats.getProgressPercent();
      // Partial/watermark sections only know a few pages (often pageCount==1 right after
      // reopen). Chapter fraction then under-reports (e.g. spine 40 → ~60% instead of ~99%).
      // Never let that clobber a higher stored progress.
      const bool unreliableChapterFraction =
          section && section->isPartial() && static_cast<int>(section->pageCount) <= 2;
      if (newPct >= 0.0f) {
        if (unreliableChapterFraction && oldPct >= 0.0f && newPct + 1.0f < oldPct) {
          LOG_DBG("ERS", "Keep stored progress %.1f%% (unreliable partial section gave %.1f%%)",
                  static_cast<double>(oldPct), static_cast<double>(newPct));
        } else {
          readingStats.setProgressPercent(newPct);
        }
      }
    }
    if (readingStats.isCompleted) {
      readingStats.setProgressPercent(100.0f);
      readingStats.estimatedTimeLeftSeconds = 0;
    }
    readingStats.save(epub->getCachePath());
    globalReadingStats.save();
    readingSessionStartMs = 0;
  }

  // Leaving mid-footnote loses the in-RAM return stack on deep sleep; persist the
  // pre-footnote position so the book reopens at the link origin, not the footnote.
  if (footnoteDepth > 0 && epub) {
    const SavedPosition& origin = savedPositions[0];
    saveProgress(origin.spineIndex, origin.pageNumber, 0);
  }

  CLIPPINGS.unload();
  section.reset();
  if (pendingReadFolderMove && epub) {
    const std::string srcPath = epub->getPath();
    const std::string oldCachePath = epub->getCachePath();
    const std::string dstPath = buildReadFolderDestination(srcPath);
    epub.reset();  // release the Epub (and any open handles) before renaming on the SD card
    moveFinishedBookToReadFolder(srcPath, dstPath, oldCachePath);
  } else {
    epub.reset();
  }
}

void EpubReaderActivity::pauseReadingStatsClock() {
  if (statsPauseStartMs == 0UL) {
    statsPauseStartMs = millis();
  }
  // Do not attribute dict/menu dwell to page pace.
  lastPageTurnTime = 0UL;
}

void EpubReaderActivity::resumeReadingStatsClock() {
  if (statsPauseStartMs != 0UL) {
    const unsigned long nowMs = millis();
    if (nowMs >= statsPauseStartMs && readingSessionStartMs != 0UL) {
      // Shift session start forward so paused wall time is not counted.
      readingSessionStartMs += (nowMs - statsPauseStartMs);
    }
    statsPauseStartMs = 0UL;
  }
  lastPageTurnTime = millis();
}

void EpubReaderActivity::resetReadingPaceData() {
  if (!SETTINGS.readingStatsTrackingEnabled()) return;
  readingStats.avgSecondsPerForwardPage = 0;
  readingStats.paceSampleCount = 0;
  readingStats.estimatedTimeLeftSeconds = 0;
  lastPageTurnTime = millis();
  if (epub) {
    readingStats.save(epub->getCachePath());
  }
  LOG_DBG("ERS", "Reading pace reset");
}

void EpubReaderActivity::openReaderMenu() {
  const int currentPage = section ? section->currentPage + 1 : 0;
  const int totalPages = section ? section->estimatedTotalPages() : 0;
  float bookProgress = 0.0f;
  if (epub->getBookSize() > 0 && section && section->estimatedTotalPages() > 0) {
    const float chapterProgress =
        static_cast<float>(section->currentPage) / static_cast<float>(section->estimatedTotalPages());
    bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
  }
  const int bookProgressPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));
  pauseReadingStatsClock();
  startActivityForResult(std::make_unique<EpubReaderMenuActivity>(
                             renderer, mappedInput, epub->getTitle(), currentPage, totalPages, bookProgressPercent,
                             SETTINGS.orientation, !currentPageFootnotes.empty(), !cachedBookmarks.empty(),
                             CLIPPINGS.hasClippings(), currentPageBookmarked, readingStats.isCompleted),
                         [this](const ActivityResult& result) {
                           // Always apply orientation change even if the menu was cancelled
                           if (const auto* menu = std::get_if<MenuResult>(&result.data)) {
                             applyOrientation(menu->orientation);
                             toggleAutoPageTurn(menu->pageTurnOption);
                             // Clip selection / list pause/resume themselves; resume here would
                             // restart the stats clock for the whole nested session.
                             const auto action = static_cast<EpubReaderMenuActivity::MenuAction>(menu->action);
                             // Keep stats paused while a nested reader child is open.
                             const bool defersStatsResume =
                                 !result.isCancelled &&
                                 (action == EpubReaderMenuActivity::MenuAction::SAVE_CLIPPING ||
                                  action == EpubReaderMenuActivity::MenuAction::VIEW_CLIPPINGS ||
                                  action == EpubReaderMenuActivity::MenuAction::MANAGE_FONTS);
                             if (!defersStatsResume) {
                               resumeReadingStatsClock();
                             }
                             if (!result.isCancelled) {
                               onReaderMenuConfirm(action);
                             }
                             return;
                           }
                           resumeReadingStatsClock();
                         });
}

bool EpubReaderActivity::buildTickHeapGate() {
  const size_t freeHeap = ESP.getFreeHeap();
  const size_t maxBlock = ESP.getMaxAllocHeap();
  // Below the floors: just wait. The tick is deferrable — page-turn transients
  // free up between turns and the tick retries every loop pass. Track the
  // paused state so skipLoopDelay() stops pinning the CPU at full speed while
  // no build work is actually happening (the gate can stay closed for a long
  // stretch if the retained build context itself holds the heap down).
  buildHeapPaused = freeHeap < BACKGROUND_BUILD_MIN_FREE_HEAP || maxBlock < BACKGROUND_BUILD_MIN_MAX_ALLOC;
  return !buildHeapPaused;
}

void EpubReaderActivity::showBuildPopup() {
  // Mid-build indexing popup: only during onEnter's blocking build-to-target phase
  // (buildPopupPending), at most once, and only when the framebuffer isn't on loan.
  // If it fires while the loan is active (e.g. the parser's size-based call during
  // startBuild), pending stays set and the deadline check retries after the loan.
  if (!buildPopupPending || !renderer.hasFrameBuffer()) return;
  GUI.drawPopup(renderer, tr(STR_INDEXING));
  // HALF-clear the popup when the page replaces it, else "INDEXING" ghosts.
  pagesUntilFullRefresh = 1;
  buildPopupPending = false;
}

void EpubReaderActivity::handleClippingJump(const ClippingJumpResult& clipping) {
  RenderLock lock(*this);
  currentSpineIndex = clipping.spineIndex;
  pendingPageJump = clipping.page;
  pendingParagraphIndex = clipping.paragraphIndex;
  pendingClippingIndex = clipping.clippingIndex;
  section.reset();
}

void EpubReaderActivity::startClipSelection() {
  if (!section || !epub) {
    resumeReadingStatsClock();
    requestUpdate();
    return;
  }

  ReaderViewportLayout layout{};
  std::vector<WordRef> words;
  int readerFontId = 0;
  int startPage = 0;
  std::string bookTitle;
  std::string author;
  std::string chapterTitle;

  {
    RenderLock lock(*this);
    if (!section || !epub) {
      resumeReadingStatsClock();
      requestUpdate();
      return;
    }
    if (!harvestSelectableWords(renderer, *section, *epub, currentSpineIndex, automaticPageTurnActive, layout,
                                readerFontId, startPage, words, &bookTitle, &author, &chapterTitle, "CLIP")) {
      resumeReadingStatsClock();
      requestUpdate();
      return;
    }
  }

  if (words.empty()) {
    LOG_ERR("CLIP", "No selectable words on current EPUB page");
    resumeReadingStatsClock();
    requestUpdate();
    return;
  }

  auto clipSelection = makeUniqueNoThrow<ClipSelectionActivity>(
      renderer, mappedInput, std::move(words), readerFontId, *section, startPage, layout.marginTop, layout.marginLeft);
  if (!clipSelection) {
    LOG_ERR("CLIP", "OOM: failed to allocate clip selection activity");
    resumeReadingStatsClock();
    requestUpdate();
    return;
  }
  startActivityForResult(
      std::move(clipSelection), [this, bookTitle = std::move(bookTitle), author = std::move(author),
                                 chapterTitle = std::move(chapterTitle)](const ActivityResult& result) {
        if (!result.isCancelled) {
          const auto* clip = std::get_if<ClippingResult>(&result.data);
          if (clip && !clip->text.empty()) {
            const size_t clippingIndex = CLIPPINGS.clippingCount();
            const auto addResult =
                CLIPPINGS.addClipping(static_cast<uint16_t>(currentSpineIndex), clip->sectionPage, clip->endSectionPage,
                                      clip->sectionPageCount, clip->startPageWordIndex, clip->endPageWordIndex,
                                      clip->wordCount, chapterTitle.c_str(), clip->paragraphIndex, clip->text);
            bool exported = false;
            if (addResult == ClippingStore::AddResult::Added) {
              exported = ClippingsManager::saveClipping(bookTitle, author, chapterTitle,
                                                        static_cast<int>(clip->sectionPage) + 1, clip->text);
              if (!exported && !CLIPPINGS.removeClippingAt(clippingIndex)) {
                LOG_ERR("CLIP", "Failed to roll back clipping after export failure");
              }
            }
            const bool saved = addResult == ClippingStore::AddResult::Added && exported;
            BookActions::drawToast(renderer,
                                   addResult == ClippingStore::AddResult::LimitReached ? tr(STR_CLIPPING_LIMIT_REACHED)
                                   : saved                                             ? tr(STR_CLIPPING_SAVED)
                                                                                       : tr(STR_CLIPPING_FAILED));
            delay(1000);
          }
        }
        resumeReadingStatsClock();
        requestUpdate();
      });
}

void EpubReaderActivity::openDictionaryWordSelect() {
  std::vector<DictionaryEntry> installed;
  DictionaryRegistry::discover(installed);
  if (installed.empty()) {
    showDictionaryMessage = true;
    dictionaryMessageId = StrId::STR_DICT_NONE_INSTALLED;
    dictionaryMessageTime = millis();
    requestUpdate();
    return;
  }
  // First-time / cleared selection: enable every installed pack so English
  // definitions + Spanish bilingual packs cascade without a Settings visit.
  if (!SETTINGS.anyDictionaryEnabled()) {
    std::vector<std::string> names;
    names.reserve(installed.size());
    for (const auto& e : installed) {
      names.push_back(e.name);
    }
    SETTINGS.setEnabledDictionaries(names);
    SETTINGS.saveToFile();
  }
  if (!section) return;
  auto page = section->loadPage(section->currentPage);
  if (!page) return;

  // Word geometry must match render(): viewable-area + screen margin + top chrome.
  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  orientedMarginTop += SETTINGS.screenMargin + ReaderUtils::kReaderTopChromeExtra;
  orientedMarginLeft += SETTINGS.screenMargin;

  pauseReadingStatsClock();
  startActivityForResult(std::make_unique<DictionaryWordSelectActivity>(renderer, mappedInput, std::move(page),
                                                                        orientedMarginLeft, orientedMarginTop),
                         [this](const ActivityResult&) {
                           resumeReadingStatsClock();
                           requestUpdate();
                         });
}

void EpubReaderActivity::openBookStats() {
  if (!epub) {
    return;
  }
  const std::string savePath = epub->getCachePath();
  float bookProgress = 0.0f;
  if (section) {
    const float chapterProgress =
        section->pageCount > 0 ? static_cast<float>(section->currentPage) / section->pageCount : 0.0f;
    bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
  }
  // Same page-based ETA as the status bar (include live session time in sec/page).
  uint32_t liveTotal = readingStats.totalReadingSeconds;
  if (readingSessionStartMs != 0UL) {
    const unsigned long nowMs = millis();
    if (nowMs >= readingSessionStartMs) {
      const uint32_t sessionSecs = static_cast<uint32_t>((nowMs - readingSessionStartMs) / 1000UL);
      if (liveTotal <= UINT32_MAX - sessionSecs) liveTotal += sessionSecs;
    }
  }
  uint32_t estLeft = 0;
  bool hasEst = false;
  if (section) {
    const int chapterPages = std::max(1, static_cast<int>(section->estimatedTotalPages()));
    const int currentPage1 = section->currentPage + 1;
    const float chapterProg = static_cast<float>(currentPage1) / static_cast<float>(chapterPages);
    const float bookProg = epub->calculateProgress(currentSpineIndex, chapterProg);
    const float remainingPages = estimateRemainingBookPages(
        chapterPages, currentPage1, bookProg, epub->calculateProgress(currentSpineIndex, 0.0f),
        epub->calculateProgress(currentSpineIndex, 1.0f));
    const uint32_t secPerPage = estimateSecondsPerPage(readingStats.avgSecondsPerForwardPage,
                                                       readingStats.paceSampleCount, liveTotal,
                                                       readingStats.totalPagesTurned);
    hasEst = estimateTimeLeftFromPages(remainingPages, secPerPage, estLeft);
  }
  const GlobalReadingStats aggregated = GlobalReadingStats::loadAggregated(globalReadingStats);
  pauseReadingStatsClock();
  startActivityForResult(
      std::make_unique<BookStatsActivity>(renderer, mappedInput, epub->getTitle(), savePath, readingStats, bookProgress,
                                          hasEst, estLeft, globalReadingStats, aggregated, false),
      [this](const ActivityResult&) {
        resumeReadingStatsClock();
        if (epub) {
          readingStats = BookReadingStats::load(epub->getCachePath());
          globalReadingStats = GlobalReadingStats::load();
        }
        requestUpdate();
      });
}

void EpubReaderActivity::loop() {
  if (!epub) {
    // Should never happen
    finish();
    return;
  }

  // End-of-Book screen reached (currentSpineIndex == spine count) means the book is
  // finished. Two independent finished-book features key off this same condition.
  const bool atEndOfBook = currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount();

  // -------------------------------------------------------------------------
  // Input first. Background prewarm/section build (and deferred open SD work)
  // can monopolize the main task for 100s of ms; if Back is handled after that
  // work, a short press can be fully press+release between gpio.update() samples
  // and never register — feeling like "Back needs two presses to leave the reader".
  // -------------------------------------------------------------------------

  if (automaticPageTurnActive) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back) || ReaderUtils::isTouchMenuGesture(mappedInput)) {
      automaticPageTurnActive = false;
      // updates chapter title space to indicate page turn disabled
      requestUpdate();
      return;
    }

    if (!section) {
      requestUpdate();
      return;
    }

    // Skips page turn if renderingMutex is busy
    if (RenderLock::peek()) {
      lastPageTurnTime = millis();
      return;
    }

    if ((millis() - lastPageTurnTime) >= pageTurnDuration) {
      pageTurn(true);
      return;
    }
  }

  // While the end screen suggestion menu is showing it owns Confirm/Back/navigation
  // input. Anything it doesn't handle (e.g. long-press Back to the file browser) falls
  // through to the regular handlers below; page turns are absorbed by the end-of-book
  // block. A Confirm release after a long-press function (bookmark/sync) fired is left
  // to the regular Confirm handler below, which consumes it via ignoreNextConfirmRelease.
  if (atEndOfBook && endOfBookOptions.menuActive() &&
      !(ignoreNextConfirmRelease && mappedInput.wasReleased(MappedInputManager::Button::Confirm))) {
    std::string openPath;
    switch (endOfBookOptions.handleMenuInput(mappedInput, &openPath)) {
      case EndOfBookOptions::Action::OpenBook:
        activityManager.goToReader(openPath);
        return;
      case EndOfBookOptions::Action::GoHome:
        leaveReaderToHome();
        return;
      case EndOfBookOptions::Action::LastPage:
        currentSpineIndex = std::max(epub->getSpineItemsCount() - 1, 0);
        nextPageNumber = 0;
        pendingPageJump = std::numeric_limits<uint16_t>::max();
        requestUpdate();
        return;
      case EndOfBookOptions::Action::Redraw:
        requestUpdate();
        return;
      case EndOfBookOptions::Action::None:
        break;
    }
  }

  // Short press Back restores position when viewing a footnote (takes priority over navigation)
  if (footnoteDepth > 0 && mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() < ReaderUtils::GO_BACK_OR_HOME_MS) {
    restoreSavedPosition();
    return;
  }

  if (ReaderUtils::handleBackNavigation(
          mappedInput, activityManager, epub ? epub->getPath().c_str() : "",
          {this, [](void* ctx) { static_cast<EpubReaderActivity*>(ctx)->leaveReaderToHome(); }})) {
    return;
  }

  // After Back has been sampled: finish recents/bookmarks/clippings/global stats.
  // Must not run before handleBackNavigation — those SD writes blocked input and
  // made Back feel dead right after open.
  if (pendingOpenSideWork && lastRenderCompleteMs != 0) {
    runDeferredOpenSideWork();
  }

  // Open path #2: first ink was BW-only; now re-render with text AA while the
  // user can already read. One-shot so a page turn does not double-paint forever.
  if (pendingDeferredOpenAa && lastRenderCompleteMs != 0) {
    pendingDeferredOpenAa = false;
    requestUpdate();
    return;
  }

  // Enter reader menu on short-press Confirm / top-edge swipe.
  // Long-press handlers set ignoreNextConfirmRelease so the release after a hold
  // does not open the menu. If a child activity (dictionary, sync, …) ate that
  // release, wasReleased never fires here and the flag would stick — clear it
  // once Confirm is idle without a release event.
  if (ignoreNextConfirmRelease) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      ignoreNextConfirmRelease = false;
    } else if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
      ignoreNextConfirmRelease = false;
    }
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
             ReaderUtils::isTouchMenuGesture(mappedInput)) {
    openReaderMenu();
  }

  // Long-press Confirm runs the user-selected function (SETTINGS.longPressMenuFunction).
  if (mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
    switch (SETTINGS.longPressMenuFunction) {
      case CrossPointSettings::LP_MENU_BOOKMARK:
        // Hold ~0.4s drops a bookmark at the current page.
        if (mappedInput.getHeldTime() >= ReaderUtils::BOOKMARK_HOLD_MS && !showBookmarkMessage) {
          addBookmark();
          showBookmarkMessage = true;
          ignoreNextConfirmRelease = true;  // Prevent accidental menu open after adding bookmark
          bookmarkMessageTime = millis();
          requestUpdate();
        }
        break;
      case CrossPointSettings::LP_MENU_KOSYNC:
        // Hold ~1s launches KOReader sync. If sync can't run (no credentials stored), fall
        // through so the normal Confirm-release still opens the reader menu.
        if (mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS) {
          if (launchKOReaderSync()) {
            ignoreNextConfirmRelease = true;  // sync launched or error shown; suppress menu open
            return;
          }
        }
        break;
      case CrossPointSettings::LP_MENU_DICTIONARY:
        // Hold ~0.4s starts dictionary word selection on the current page.
        if (mappedInput.getHeldTime() >= ReaderUtils::BOOKMARK_HOLD_MS && !showDictionaryMessage) {
          ignoreNextConfirmRelease = true;  // Prevent menu open on the release that follows
          openDictionaryWordSelect();
          return;
        }
        break;
      case CrossPointSettings::LP_MENU_SLEEP:
        if (mappedInput.getHeldTime() >= ReaderUtils::BOOKMARK_HOLD_MS) {
          ignoreNextConfirmRelease = true;
          activityManager.goToSleep();
          return;
        }
        break;
      case CrossPointSettings::LP_MENU_FORCE_REFRESH:
        if (mappedInput.getHeldTime() >= ReaderUtils::BOOKMARK_HOLD_MS) {
          ignoreNextConfirmRelease = true;
          forcedRefreshPending = true;
          pagesUntilFullRefresh = 0;
          requestUpdate();
          return;
        }
        break;
      case CrossPointSettings::LP_MENU_FILE_BROWSER:
        if (mappedInput.getHeldTime() >= ReaderUtils::BOOKMARK_HOLD_MS) {
          ignoreNextConfirmRelease = true;
          activityManager.goToFileBrowser();
          return;
        }
        break;
      case CrossPointSettings::LP_MENU_SCREENSHOT:
        if (mappedInput.getHeldTime() >= ReaderUtils::BOOKMARK_HOLD_MS) {
          ignoreNextConfirmRelease = true;
          pendingScreenshot = true;
          requestUpdate();
          return;
        }
        break;
      case CrossPointSettings::LP_MENU_FOOTNOTES:
        if (mappedInput.getHeldTime() >= ReaderUtils::BOOKMARK_HOLD_MS) {
          ignoreNextConfirmRelease = true;
          if (footnoteDepth > 0) {
            restoreSavedPosition();
          } else if (currentPageFootnotes.size() == 1) {
            navigateToHref(currentPageFootnotes[0].href, true);
          } else if (currentPageFootnotes.size() > 1) {
            startActivityForResult(
                std::make_unique<EpubReaderFootnotesActivity>(renderer, mappedInput, currentPageFootnotes),
                [this](const ActivityResult& result) {
                  if (result.isCancelled) return;
                  if (const auto* fn = std::get_if<FootnoteResult>(&result.data)) {
                    navigateToHref(fn->href, true);
                  }
                });
          }
          return;
        }
        break;
      case CrossPointSettings::LP_MENU_FILE_TRANSFER:
        if (mappedInput.getHeldTime() >= ReaderUtils::BOOKMARK_HOLD_MS) {
          ignoreNextConfirmRelease = true;
          activityManager.goToFileTransfer();
          return;
        }
        break;
      case CrossPointSettings::LP_MENU_READING_STATS:
        if (!SETTINGS.readingStatsTrackingEnabled()) break;
        if (mappedInput.getHeldTime() >= ReaderUtils::BOOKMARK_HOLD_MS) {
          ignoreNextConfirmRelease = true;
          openBookStats();
          return;
        }
        break;
      case CrossPointSettings::LP_MENU_DISABLED:
      default:
        break;
    }
  }

  // Drop this book from the Recent Books list; if the reader then pages back into the book,
  // re-add it. So removal only sticks if the reader leaves while still on the End-of-Book
  // screen. Acts only on the transition (guarded by recentsEntryRemoved) — no per-frame writes.
  if (SETTINGS.removeReadBooksFromRecents) {
    if (atEndOfBook && !recentsEntryRemoved) {
      // Only treat the book as "removed by us" if it was actually in the list, so the
      // re-add branch below doesn't insert a book the feature never removed.
      recentsEntryRemoved = RECENT_BOOKS.removeByPath(epub->getPath());
    } else if (!atEndOfBook && recentsEntryRemoved) {
      // Re-add (goes to front of the list via addBook — accepted ordering side effect).
      RECENT_BOOKS.addBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath());
      recentsEntryRemoved = false;
    }
  }

  // Arm the move here so ANY exit path (Back, Home, file browser) relocates the book into
  // /Read/ in onExit(); paging back off the end screen disarms it (book not actually
  // finished). If removeReadBooksFromRecents also fired, RecentBooksStore::updatePath in the
  // move path becomes a safe no-op since the entry was already removed.
  if (atEndOfBook) {
    pendingReadFolderMove = SETTINGS.moveFinishedToReadFolder && !isInReadFolder(epub->getPath());
    // Dashboard progress is loaded from stats_v6 without reopening the EPUB — stamp 100%
    // as soon as the end screen is shown so a later partial reopen cannot leave ~60%.
    if (!readingStats.isCompleted) {
      readingStats.isCompleted = true;
      readingStats.setProgressPercent(100.0f);
      readingStats.estimatedTimeLeftSeconds = 0;
      if (SETTINGS.readingStatsTrackingEnabled() && !readingStats.finishedDateManual) {
        ReadingStatsDateTime now;
        if (getCurrentLocalReadingStatsDateTime(now) && !readingStats.finishedDate.isValid()) {
          readingStats.finishedDate = now.date;
        }
      }
      if (SETTINGS.readingStatsTrackingEnabled() && globalReadingStats.completedBooks < UINT32_MAX) {
        globalReadingStats.completedBooks++;
        globalReadingStats.save();
      }
      readingStats.save(epub->getCachePath());
    } else if (readingStats.getProgressPercent() < 99.5f) {
      readingStats.setProgressPercent(100.0f);
      readingStats.estimatedTimeLeftSeconds = 0;
      readingStats.save(epub->getCachePath());
    }
  } else {
    pendingReadFolderMove = false;
  }

  // Idle glyph prewarm for the likely next page (currentPage + 1). The scan
  // pass draws nothing (FCM scan mode suppresses pixels), so the displayed
  // framebuffer is untouched; endScanAndPrewarm loads only glyphs not already
  // cached. Debounced past rapid page-flipping, one attempt per position, and
  // deferred while a render/build owns the CPU or the heap is at the render
  // floor. Cross-chapter prewarm is deliberately out of scope (next spine's
  // section isn't loaded).
  // Runs after exit/nav input so a Back press is never delayed by this work.
  constexpr unsigned long IDLE_PREWARM_DEBOUNCE_MS = 400;
  if (section && !section->isBuilding() && !RenderLock::peek() && renderer.hasFrameBuffer() &&
      lastRenderCompleteMs != 0 && millis() - lastRenderCompleteMs > IDLE_PREWARM_DEBOUNCE_MS &&
      ESP.getFreeHeap() > RENDER_MIN_FREE_HEAP && ESP.getMaxAllocHeap() > BACKGROUND_BUILD_MIN_MAX_ALLOC &&
      (idlePrewarmSpine != currentSpineIndex || idlePrewarmPage != section->currentPage)) {
    RenderLock lock;  // the page table must not change under the scan
    // Re-check under the lock: peek() and acquisition are not atomic, so the render
    // task may have reset/replaced the section or moved the page in between.
    if (section && !section->isBuilding() &&
        (idlePrewarmSpine != currentSpineIndex || idlePrewarmPage != section->currentPage)) {
      idlePrewarmSpine = currentSpineIndex;
      idlePrewarmPage = section->currentPage;
      const int nextPage = section->currentPage + 1;
      if (nextPage < static_cast<int>(section->pageCount)) {
        if (const auto p = section->loadPage(nextPage)) {
          if (auto* fcm = renderer.getFontCacheManager()) {
            const auto t0 = millis();
            auto scope = fcm->createPrewarmScope();
            p->render(renderer, SETTINGS.getReaderFontId(), 0, 0);  // scan only, no pixels
            scope.endScanAndPrewarm();
            LOG_DBG("ERS", "Idle prewarm: page %d in %lums", nextPage, millis() - t0);
          }
        }
      }
    }
  }

  // Lazily resume a partial's extension build once the reader nears its watermark. Far from
  // it the rebuild is all cost (whole-chapter re-layout from page 0) and no benefit this
  // session, so reopening a partial deliberately does NOT start it (see the deferral in
  // render()); crossing this margin is the signal that the reader will actually need pages
  // past the watermark soon. Uses the last render's viewport so pagination matches the
  // partial being extended.
  if (section && !section->isBuilding() && section->isPartial() && !RenderLock::peek() && buildViewportWidth > 0 &&
      !partialRebuildStartFailed &&
      section->currentPage + PARTIAL_REBUILD_START_MARGIN >= static_cast<int>(section->pageCount)) {
    RenderLock lock;
    // Reuse the last render's viewport so the extension paginates identically to the partial.
    const ReaderRenderSpec buildSpec = SETTINGS.readerRenderSpec(buildViewportWidth, buildViewportHeight);
    if (!section->startBuild(buildSpec)) {
      // Not fatal: the partial keeps serving its pages; crossing the watermark falls back to
      // the blocking extension in render(). Don't retry every tick.
      partialRebuildStartFailed = true;
      LOG_ERR("ERS", "Failed to start deferred partial extension build");
    } else {
      LOG_DBG("ERS", "Reader near partial watermark (%d/%d), resuming extension build", section->currentPage,
              section->pageCount);
    }
  }

  // Drive any in-progress incremental section build forward, off the page-turn critical path,
  // but only within a small window ahead of the reader: an unbounded build monopolized the
  // RenderLock and locked out page turns. The build follows the reader instead, and instant
  // reopen comes from suspendBuild() persisting the laid-out pages as a partial on exit.
  // Skip while the render mutex is busy so we never delay a pending render; re-check
  // isBuilding() under the lock since render() may have just finished it.
  // While extending a partial (rebuild from a previous session), pageCount is pinned at the
  // partial's watermark until the build catches up, so the window check would wrongly read
  // "far enough ahead" and stall the build at 0 pages -- then the first turn past the
  // watermark re-parses the whole chapter synchronously. Keep ticking until it finalizes.
  if (section && section->isBuilding() && !RenderLock::peek() &&
      (section->isPartial() || static_cast<int>(section->pageCount) < section->currentPage + BUILD_WINDOW_AHEAD) &&
      buildTickHeapGate()) {
    RenderLock lock;
    // Re-check under the lock: render() (which also holds the RenderLock) may have finalized the
    // build between the outer isBuilding() check and acquiring the lock here, in which case
    // buildSomeMore() would fail and wrongly reset the section. The heap gate must be re-read
    // too: a render that won the lock race can expand retained glyph buffers, invalidating the
    // pre-lock heap reading. cppcheck can't see the cross-task mutation, so it flags this as
    // always true.
    // cppcheck-suppress knownConditionTrueFalse
    if (section->isBuilding() && buildTickHeapGate()) {
      if (!section->buildSomeMore(BACKGROUND_BUILD_PAGES_PER_TICK)) {
        LOG_ERR("ERS", "Background section build failed");
        section.reset();
        requestUpdate();
      } else if (section->isBuildComplete() && applyDeferredReposition()) {
        // The chapter re-paginated since the saved progress (settings changed): we now know the
        // real page count, so re-render at the remapped page. No-op for an unchanged resume.
        requestUpdate();
      }
    }
  }

  if (showBookmarkMessage && (millis() - bookmarkMessageTime) >= ReaderUtils::BOOKMARK_MESSAGE_DURATION_MS) {
    showBookmarkMessage = false;
    requestUpdate();
  }

  if (showDictionaryMessage && (millis() - dictionaryMessageTime) >= ReaderUtils::BOOKMARK_MESSAGE_DURATION_MS) {
    showDictionaryMessage = false;
    requestUpdate();
  }

  const auto touch = ReaderUtils::detectTouchPageTurn(renderer, mappedInput);

  // auto [prevTriggered, nextTriggered] = ReaderUtils::detectPageTurn(mappedInput);

  // Handle short/long power button press for footnotes (reader-local; not sleep/refresh).
  if (mappedInput.wasReleased(MappedInputManager::Button::Power) &&
      !mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    const unsigned long held = mappedInput.getHeldTime();
    const auto pwrAction = held < SETTINGS.getPowerButtonLongPressDuration()
                               ? static_cast<CrossPointSettings::SHORT_PWRBTN>(SETTINGS.shortPwrBtn)
                               : static_cast<CrossPointSettings::SHORT_PWRBTN>(SETTINGS.longPwrBtn);
    if (pwrAction == CrossPointSettings::SHORT_PWRBTN::FOOTNOTES) {
      if (footnoteDepth > 0) {
        restoreSavedPosition();
      } else if (currentPageFootnotes.size() == 1) {
        navigateToHref(currentPageFootnotes[0].href, true);
      } else if (currentPageFootnotes.size() > 1) {
        startActivityForResult(
            std::make_unique<EpubReaderFootnotesActivity>(renderer, mappedInput, currentPageFootnotes),
            [this](const ActivityResult& result) {
              if (!result.isCancelled) {
                const auto& footnoteResult = std::get<FootnoteResult>(result.data);
                navigateToHref(footnoteResult.href, true);
              }
              requestUpdate();
            });
      }
      return;
    }
  }

  auto [prevTriggered, nextTriggered, fromTilt] = ReaderUtils::detectPageTurn(mappedInput);
  prevTriggered = prevTriggered || touch.prev;
  nextTriggered = nextTriggered || touch.next;
  const bool fromTouch = touch.prev || touch.next;
  if (!pageTurnLatch.accept(prevTriggered, nextTriggered, fromTilt, fromTouch, mappedInput)) {
    return;
  }

  // At end of the book with no suggestion menu, forward button goes home and back
  // button returns to last page
  if (currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount()) {
    if (endOfBookOptions.menuActive()) {
      // Selection movement was handled above; absorb leftover page-turn triggers so
      // e.g. "previous" at the top of the list doesn't jump back into the book
      return;
    }
    if (nextTriggered) {
      leaveReaderToHome();
    } else {
      currentSpineIndex = epub->getSpineItemsCount() - 1;
      nextPageNumber = 0;
      pendingPageJump = std::numeric_limits<uint16_t>::max();
      requestUpdate();
    }
    return;
  }

  const unsigned long heldMs = (touch.prev || touch.next) ? touch.heldMs : mappedInput.getHeldTime();
  const bool longPress = !fromTilt && heldMs > ReaderUtils::SKIP_HOLD_MS;

  // Don't skip chapter after screenshot
  if (gpio.wasReleased(HalGPIO::BTN_POWER) && gpio.wasReleased(HalGPIO::BTN_DOWN)) {
    return;
  }

  if (longPress && SETTINGS.longPressButtonBehavior == SETTINGS.CHAPTER_SKIP) {
    if (!nextTriggered && section && section->currentPage > 0) {
      section->currentPage = 0;
      requestUpdate();
      return;
    }

    // We don't want to delete the section mid-render, so grab the semaphore
    {
      RenderLock lock(*this);
      nextPageNumber = 0;
      if (nextTriggered) {
        currentSpineIndex++;
      } else if (currentSpineIndex > 0) {
        currentSpineIndex--;
      }
      section.reset();
    }
    requestUpdate();
    return;
  }

  if (longPress && SETTINGS.longPressButtonBehavior == SETTINGS.ORIENTATION_CHANGE) {
    const uint8_t newOrientation =
        nextTriggered ? (SETTINGS.orientation - 1 + SETTINGS.ORIENTATION_COUNT) % SETTINGS.ORIENTATION_COUNT
                      : (SETTINGS.orientation + 1) % SETTINGS.ORIENTATION_COUNT;
    applyOrientation(newOrientation);
    requestUpdate();
    return;
  }

  // No current section, attempt to rerender the book
  if (!section) {
    requestUpdate();
    return;
  }

  if (prevTriggered) {
    pageTurn(false);
  } else {
    pageTurn(true);
  }
}

// Translate an absolute percent into a spine index plus a normalized position
// within that spine so we can jump after the section is loaded.
void EpubReaderActivity::jumpToPercent(int percent) {
  if (!epub) {
    return;
  }

  const size_t bookSize = epub->getBookSize();
  if (bookSize == 0) {
    return;
  }

  // Normalize input to 0-100 to avoid invalid jumps.
  percent = clampPercent(percent);

  // Convert percent into a byte-like absolute position across the spine sizes.
  // Use an overflow-safe computation: (bookSize / 100) * percent + (bookSize % 100) * percent / 100
  size_t targetSize =
      (bookSize / 100) * static_cast<size_t>(percent) + (bookSize % 100) * static_cast<size_t>(percent) / 100;
  if (percent >= 100) {
    // Ensure the final percent lands inside the last spine item.
    targetSize = bookSize - 1;
  }

  const int spineCount = epub->getSpineItemsCount();
  if (spineCount == 0) {
    return;
  }

  int targetSpineIndex = spineCount - 1;
  size_t prevCumulative = 0;

  for (int i = 0; i < spineCount; i++) {
    const size_t cumulative = epub->getCumulativeSpineItemSize(i);
    if (targetSize <= cumulative) {
      // Found the spine item containing the absolute position.
      targetSpineIndex = i;
      prevCumulative = (i > 0) ? epub->getCumulativeSpineItemSize(i - 1) : 0;
      break;
    }
  }

  const size_t cumulative = epub->getCumulativeSpineItemSize(targetSpineIndex);
  const size_t spineSize = (cumulative > prevCumulative) ? (cumulative - prevCumulative) : 0;
  // Store a normalized position within the spine so it can be applied once loaded.
  pendingSpineProgress =
      (spineSize == 0) ? 0.0f : static_cast<float>(targetSize - prevCumulative) / static_cast<float>(spineSize);
  if (pendingSpineProgress < 0.0f) {
    pendingSpineProgress = 0.0f;
  } else if (pendingSpineProgress > 1.0f) {
    pendingSpineProgress = 1.0f;
  }

  // Reset state so render() reloads and repositions on the target spine.
  {
    RenderLock lock(*this);
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = 0;
    pendingPercentJump = true;
    section.reset();
  }
}

void EpubReaderActivity::onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action) {
  auto progressChangeResultHandler = [this](const ActivityResult& result) {
    loadCachedBookmarks();
    if (!result.isCancelled) {
      const auto& sync = std::get<ProgressChangeResult>(result.data);
      int targetSpineIndex = sync.spineIndex;
      int targetPage = sync.page;
      const int activeTotalPages = section ? section->estimatedTotalPages() : 0;
      const bool cachedPageMatchesActiveSection = section && sync.totalPages > 0 &&
                                                  currentSpineIndex == sync.spineIndex && sync.page >= 0 &&
                                                  sync.page < sync.totalPages && activeTotalPages == sync.totalPages;

      if (!cachedPageMatchesActiveSection && sync.hasSavedProgress) {
        const int totalPages = section ? section->estimatedTotalPages() : cachedChapterTotalPageCount;
        CrossPointPosition fallback =
            ProgressMapper::toCrossPoint(epub, {sync.xpath, sync.percentage}, renderer, currentSpineIndex, totalPages);
        targetSpineIndex = fallback.spineIndex;
        targetPage = fallback.pageNumber;
      }

      if (currentSpineIndex != targetSpineIndex) {
        RenderLock lock(*this);
        currentSpineIndex = targetSpineIndex;
        nextPageNumber = targetPage;
        section.reset();
      } else if (section && section->currentPage != targetPage) {
        RenderLock lock(*this);
        const int clampedTargetPage = std::max(0, targetPage);
        section->currentPage = clampedTargetPage;
      } else if (!section) {
        nextPageNumber = targetPage;
      }
    }
  };

  switch (action) {
    case EpubReaderMenuActivity::MenuAction::SELECT_CHAPTER: {
      const int spineIdx = currentSpineIndex;
      const std::string path = epub->getPath();
      startActivityForResult(
          std::make_unique<EpubReaderChapterSelectionActivity>(renderer, mappedInput, epub, path, spineIdx),
          [this](const ActivityResult& result) {
            if (!result.isCancelled) {
              const auto& chapterResult = std::get<ChapterResult>(result.data);
              RenderLock lock(*this);

              currentSpineIndex = chapterResult.spineIndex;

              // If anchor is not empty, it will be used later to calculate the page number.
              pendingAnchor = chapterResult.anchor;

              // Otherwise page 0 will be used.
              nextPageNumber = 0;

              section.reset();
            }
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::FOOTNOTES: {
      startActivityForResult(std::make_unique<EpubReaderFootnotesActivity>(renderer, mappedInput, currentPageFootnotes),
                             [this](const ActivityResult& result) {
                               if (!result.isCancelled) {
                                 const auto& footnoteResult = std::get<FootnoteResult>(result.data);
                                 navigateToHref(footnoteResult.href, true);
                               }
                               requestUpdate();
                             });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::GO_TO_PERCENT: {
      float bookProgress = 0.0f;
      if (epub && epub->getBookSize() > 0 && section && section->pageCount > 0) {
        const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
        bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
      }
      const int initialPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));
      startActivityForResult(
          std::make_unique<EpubReaderPercentSelectionActivity>(renderer, mappedInput, initialPercent),
          [this](const ActivityResult& result) {
            if (!result.isCancelled) {
              jumpToPercent(std::get<PercentResult>(result.data).percent);
            }
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::MANAGE_FONTS: {
      // Child of the reader (not Settings/home): Back returns here and reflows in place.
      startActivityForResult(
          std::make_unique<TextSettingsActivity>(renderer, mappedInput, &sdFontSystem.registry(),
                                                 TextSettingsActivity::Tab::Family),
          [this](const ActivityResult&) {
            SETTINGS.saveToFile();
            {
              RenderLock lock(*this);
              if (section) {
                cachedSpineIndex = currentSpineIndex;
                cachedChapterTotalPageCount = section->pageCount;
                nextPageNumber = section->currentPage;
              }
              // Drop laid-out chapter so render() rebuilds with new type metrics.
              section.reset();
            }
            resumeReadingStatsClock();
            requestUpdate();
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::DICTIONARY: {
      openDictionaryWordSelect();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::READING_STATS: {
      openBookStats();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::DISPLAY_QR: {
      if (section && section->currentPage >= 0 && section->currentPage < section->pageCount) {
        std::string fullText = section->getTextFromSectionFile();
        if (!fullText.empty()) {
          startActivityForResult(std::make_unique<QrDisplayActivity>(renderer, mappedInput, fullText),
                                 [this](const ActivityResult& result) {});
          break;
        }
      }
      // If no text or page loading failed, just close menu
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::GO_HOME: {
      leaveReaderToHome();
      return;
    }
    case EpubReaderMenuActivity::MenuAction::DELETE_CACHE: {
      {
        RenderLock lock(*this);
        if (epub && section) {
          uint16_t backupSpine = currentSpineIndex;
          uint16_t backupPage = section->currentPage;
          uint16_t backupPageCount = section->pageCount;
          section.reset();
          epub->clearCache();
          epub->setupCacheDir();
          if (!saveProgress(backupSpine, backupPage, backupPageCount)) {
            LOG_ERR("ERS", "Failed to save progress before cache clear");
          }
        }
      }
      onGoHome();
      return;
    }
    case EpubReaderMenuActivity::MenuAction::SCREENSHOT: {
      {
        RenderLock lock(*this);
        pendingScreenshot = true;
      }
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::SYNC: {
      launchKOReaderSync();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::BOOKMARKS: {
      startActivityForResult(
          std::make_unique<EpubReaderBookmarksActivity>(renderer, mappedInput, epub, epub->getPath()),
          progressChangeResultHandler);
      break;
    }
    case EpubReaderMenuActivity::MenuAction::TOGGLE_BOOKMARK: {
      addBookmark();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::TOGGLE_COMPLETED: {
      setBookCompleted(!readingStats.isCompleted);
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::RESET_READING_PACE: {
      resetReadingPaceData();
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::DELETE_STATS: {
      startActivityForResult(
          std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_DELETE_BOOK_STATS),
                                                 epub ? epub->getTitle() : std::string{}),
          [this](const ActivityResult& result) {
            if (!result.isCancelled && epub) {
              const std::string path = epub->getCachePath();
              if (BookReadingStats::remove(path)) {
                readingStats = BookReadingStats{};
                // Keep session baseline so onExit doesn't double-count this session.
                readingSessionStartMs = millis();
                lastPageTurnTime = readingSessionStartMs;
                LOG_DBG("ERS", "Deleted book stats at %s", path.c_str());
              } else {
                LOG_ERR("ERS", "Failed to delete book stats");
              }
            }
            requestUpdate();
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::DELETE_BOOKMARKS: {
      startActivityForResult(
          std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_DELETE_BOOKMARKS),
                                                 epub ? epub->getTitle() : std::string{}),
          [this](const ActivityResult& result) {
            if (!result.isCancelled && epub) {
              cachedBookmarks.clear();
              currentPageBookmarked = false;
              if (!BookmarkFile::save(epub->getPath(), cachedBookmarks)) {
                LOG_ERR("ERS", "Failed to clear bookmarks");
              }
            }
            requestUpdate();
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::SAVE_CLIPPING: {
      startClipSelection();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::VIEW_CLIPPINGS: {
      startActivityForResult(std::make_unique<EpubReaderClippingListActivity>(renderer, mappedInput),
                             [this](const ActivityResult& result) {
                               if (!result.isCancelled) {
                                 if (const auto* clipping = std::get_if<ClippingJumpResult>(&result.data)) {
                                   handleClippingJump(*clipping);
                                 }
                               }
                               resumeReadingStatsClock();
                               requestUpdate();
                             });
      break;
    }
  }
}

float EpubReaderActivity::getCurrentBookProgressPercent() const {
  if (!epub || epub->getBookSize() == 0) {
    return -1.0f;
  }
  float chapterProgress = 0.0f;
  if (section && section->estimatedTotalPages() > 0) {
    chapterProgress =
        static_cast<float>(section->currentPage) / static_cast<float>(section->estimatedTotalPages());
  }
  return epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
}

bool EpubReaderActivity::launchLeaveKoSync(const bool uploadOnly) {
  if (!epub) {
    return false;
  }

  const float bookPercent = getCurrentBookProgressPercent();
  const int currentPage = section ? section->currentPage : nextPageNumber;
  const int totalPages = section ? section->estimatedTotalPages() : cachedChapterTotalPageCount;
  std::optional<uint16_t> paragraphIndex;
  if (section && currentPage >= 0 && currentPage < section->pageCount) {
    const uint16_t paragraphPage =
        currentPage > 0 ? static_cast<uint16_t>(currentPage - 1) : static_cast<uint16_t>(currentPage);
    if (const auto pIdx = section->getParagraphIndexForPage(paragraphPage)) {
      paragraphIndex = *pIdx;
    }
  }

  CrossPointPosition localPos = getCurrentPosition();
  SavedProgressPosition localKoPos = ProgressMapper::toSavedProgress(epub, localPos);
  const int tocIdx = epub->getTocIndexForSpineIndex(currentSpineIndex);
  std::string localChapterName = (tocIdx >= 0) ? epub->getTocItem(tocIdx).title : "";
  const std::string savedEpubPath = epub->getPath();

  if (!saveProgress(currentSpineIndex, currentPage, totalPages)) {
    LOG_ERR("KOSync", "Aborting leave-sync because current progress could not be saved");
    pendingSyncSaveError = true;
    requestUpdate();
    return false;
  }

  LOG_DBG("KOSync", "Starting leave sync (uploadOnly=%d heap=%u percent=%.1f)", uploadOnly ? 1 : 0,
          static_cast<unsigned>(ESP.getFreeHeap()), static_cast<double>(bookPercent));

  {
    RenderLock lock(*this);
    if (section) {
      nextPageNumber = section->currentPage;
    }
    ImageBlock::setExtractor(nullptr, nullptr);
    section.reset();
    epub.reset();
  }

  activityManager.replaceActivity(std::make_unique<KOReaderSyncActivity>(
      renderer, mappedInput, savedEpubPath, currentSpineIndex, currentPage, totalPages, std::move(localKoPos),
      std::move(localChapterName), paragraphIndex, /*autoUploadOnly=*/uploadOnly, bookPercent,
      /*leaveToHome=*/true));
  return true;
}

bool EpubReaderActivity::tryStartAutoKoUpload() {
  if (!epub) {
    return false;
  }
  if (!KOREADER_STORE.hasCredentials()) {
    return false;
  }

  const KOReaderSyncBehavior behavior = KOREADER_STORE.getSyncBehavior();
  // Feature fully off: leave goes home with no prompt, upload, or network use.
  if (behavior == KOReaderSyncBehavior::OFF) {
    return false;
  }

  const float bookPercent = getCurrentBookProgressPercent();
  const std::string& bookPath = epub->getPath();

  // Ask every time: only *prompt* on leave — never auto-start a full sync.
  if (behavior == KOReaderSyncBehavior::ASK_EVERY_TIME) {
    pauseReadingStatsClock();
    startActivityForResult(
        std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_SYNC_PROGRESS), tr(STR_KOREADER_SYNC)),
        [this](const ActivityResult& res) {
          resumeReadingStatsClock();
          if (res.isCancelled) {
            onGoHome();
            return;
          }
          // Confirmed: full Ask-style sync (user picks Apply/Upload; remote-ahead also asks).
          if (!launchLeaveKoSync(/*uploadOnly=*/false)) {
            onGoHome();
          }
        });
    return true;
  }

  // Smart: auto furthest-ahead on leave (including apply remote when remote is ahead).
  if (behavior == KOReaderSyncBehavior::SMART) {
    return launchLeaveKoSync(/*uploadOnly=*/false);
  }

  // Percent / Time: gated quiet upload; if remote is further, sync UI asks how to proceed.
  if (behavior == KOReaderSyncBehavior::PERCENT || behavior == KOReaderSyncBehavior::TIME) {
    const AutoUploadDecision decision = KOREADER_STORE.evaluateAutoUpload(bookPath.c_str(), bookPercent);
    if (decision != AutoUploadDecision::Upload) {
      char toast[64];
      toast[0] = '\0';
      switch (decision) {
        case AutoUploadDecision::SkipTimeNotElapsed: {
          const unsigned mins = KOREADER_STORE.getAutoUploadIntervalMinutes();
          if (mins >= 60 && mins % 60 == 0) {
            snprintf(toast, sizeof(toast), tr(STR_AUTO_UPLOAD_SKIP_HOURS), static_cast<unsigned>(mins / 60u));
          } else {
            snprintf(toast, sizeof(toast), tr(STR_AUTO_UPLOAD_SKIP_MINUTES), mins);
          }
          break;
        }
        case AutoUploadDecision::SkipPercentNotMet:
          snprintf(toast, sizeof(toast), tr(STR_AUTO_UPLOAD_SKIP_PERCENT),
                   static_cast<unsigned>(KOREADER_STORE.getAutoUploadPercentThreshold()));
          break;
        case AutoUploadDecision::SkipNoCredentials:
          snprintf(toast, sizeof(toast), "%s", tr(STR_AUTO_UPLOAD_SKIP_CREDS));
          break;
        default:
          break;
      }
      if (toast[0] != '\0') {
        BookActions::drawToast(renderer, toast);
        delay(900);
      }
      LOG_DBG("KOSync", "Auto-upload skipped (decision=%u percent=%.1f)", static_cast<unsigned>(decision),
              static_cast<double>(bookPercent));
      return false;
    }
    return launchLeaveKoSync(/*uploadOnly=*/true);
  }

  return false;
}

void EpubReaderActivity::leaveReaderToHome() {
  if (tryStartAutoKoUpload()) {
    return;
  }
  onGoHome();
}

bool EpubReaderActivity::launchKOReaderSync() {
  if (!KOREADER_STORE.hasCredentials()) return false;  // no-op: nothing to launch
  // Sync Behavior → Disabled: ignore manual Sync Progress / long-press KOSync too.
  if (KOREADER_STORE.getSyncBehavior() == KOReaderSyncBehavior::OFF) return false;

  const int currentPage = section ? section->currentPage : nextPageNumber;
  const int totalPages = section ? section->estimatedTotalPages() : cachedChapterTotalPageCount;
  std::optional<uint16_t> paragraphIndex;
  if (section && currentPage >= 0 && currentPage < section->pageCount) {
    const uint16_t paragraphPage =
        currentPage > 0 ? static_cast<uint16_t>(currentPage - 1) : static_cast<uint16_t>(currentPage);
    if (const auto pIdx = section->getParagraphIndexForPage(paragraphPage)) {
      paragraphIndex = *pIdx;
    }
  }

  // Pre-compute local KO position and chapter name while Epub is still in RAM.
  CrossPointPosition localPos = getCurrentPosition();
  SavedProgressPosition localKoPos = ProgressMapper::toSavedProgress(epub, localPos);
  const int tocIdx = epub->getTocIndexForSpineIndex(currentSpineIndex);
  std::string localChapterName = (tocIdx >= 0) ? epub->getTocItem(tocIdx).title : "";
  const std::string savedEpubPath = epub->getPath();

  // Persist current position so the reader resumes at the right page on return.
  // goToReader() depends on this file, so abort the sync if the write fails.
  if (!saveProgress(currentSpineIndex, currentPage, totalPages)) {
    LOG_ERR("KOSync", "Aborting sync because current progress could not be saved");
    pendingSyncSaveError = true;
    requestUpdate();
    return true;  // acted: surfaced a save error to the user
  }

  // Release Epub and Section to free ~65KB RAM for the TLS handshake.
  LOG_DBG("KOSync", "Releasing epub for sync (heap before: %u)", (unsigned)ESP.getFreeHeap());
  {
    RenderLock lock(*this);
    if (section) {
      nextPageNumber = section->currentPage;
    }
    // The image extractor holds a raw pointer into this epub (see onEnter);
    // clear it before the early release, mirroring onExit(), or a later image
    // render would call through a dangling context.
    ImageBlock::setExtractor(nullptr, nullptr);
    section.reset();
    epub.reset();
  }
  LOG_DBG("KOSync", "Epub released (heap after: %u)", (unsigned)ESP.getFreeHeap());

  activityManager.replaceActivity(std::make_unique<KOReaderSyncActivity>(
      renderer, mappedInput, savedEpubPath, currentSpineIndex, currentPage, totalPages, std::move(localKoPos),
      std::move(localChapterName), paragraphIndex));
  return true;  // acted: launched the sync activity
}

void EpubReaderActivity::applyOrientation(const uint8_t orientation) {
  // No-op if the selected orientation matches current settings.
  if (SETTINGS.orientation == orientation) {
    return;
  }

  // Preserve current reading position so we can restore after reflow.
  {
    RenderLock lock(*this);
    if (section) {
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = section->pageCount;
      nextPageNumber = section->currentPage;
    }

    // Persist the selection so the reader keeps the new orientation on next launch.
    SETTINGS.orientation = orientation;
    SETTINGS.saveToFile();

    // Update renderer orientation to match the new logical coordinate system.
    ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

    // Reset section to force re-layout in the new orientation.
    section.reset();
  }
}

void EpubReaderActivity::toggleAutoPageTurn(const uint8_t selectedPageTurnOption) {
  if (selectedPageTurnOption == 0 || selectedPageTurnOption >= std::size(PAGE_TURN_RATES)) {
    automaticPageTurnActive = false;
    return;
  }

  lastPageTurnTime = millis();
  // calculates page turn duration by dividing by number of pages
  pageTurnDuration = (1UL * 60 * 1000) / PAGE_TURN_RATES[selectedPageTurnOption];
  automaticPageTurnActive = true;

  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  // resets cached section so that space is reserved for auto page turn indicator when None or progress bar only
  if (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight()) {
    // Preserve current reading position so we can restore after reflow.
    RenderLock lock(*this);
    if (section) {
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = section->pageCount;
      nextPageNumber = section->currentPage;
    }
    section.reset();
  }
}

void EpubReaderActivity::pageTurn(bool isForwardTurn) {
  // User owns navigation now — do not remap this position when a background section
  // build later finalizes with a different page count (settings-change reposition).
  cachedChapterTotalPageCount = 0;
  pendingForwardPastEnd = false;

  // Pace / pages-per-min: count forward turns with a sane dwell (2s–session idle).
  // Session Time setting is the idle gap: longer dwells are treated as not reading.
  // Pages Turned is not shown on Dashboard, but totalPagesTurned still feeds pages/min.
  // Skip when tracking is off — no background counters even in RAM for this session.
  if (SETTINGS.readingStatsTrackingEnabled() && isForwardTurn && lastPageTurnTime != 0UL) {
    const unsigned long nowMs = millis();
    const uint32_t dwellSecs =
        nowMs >= lastPageTurnTime ? static_cast<uint32_t>((nowMs - lastPageTurnTime) / 1000UL) : 0u;
    const uint32_t idleCap = SETTINGS.getReadingSessionIdleSeconds();
    if (dwellSecs >= 2 && dwellSecs <= idleCap) {
      readingStats.recordForwardPageRead(dwellSecs);
      if (readingStats.totalPagesTurned < UINT32_MAX) {
        readingStats.totalPagesTurned++;
      }
      if (globalReadingStats.totalPagesTurned < UINT32_MAX) {
        globalReadingStats.totalPagesTurned++;
      }
    }
  }

  if (!section) {
    lastPageTurnTime = millis();
    requestUpdate();
    return;
  }

  if (isForwardTurn) {
    // Advance within the section while there are (or may still be) more pages: either a built
    // page ahead, or the section is still building (windowed), in which case more pages exist
    // beyond the current watermark and render()'s ensure-built pump will lay them out. Only when
    // the section is fully built AND we're on its last page do we move to the next spine -- using
    // the live pageCount alone would mistake the build watermark for the end of a giant spine.
    if (section->currentPage < static_cast<int>(section->pageCount) - 1) {
      section->currentPage++;
    } else if (section->isBuilding() && !section->isBuildComplete()) {
      // Optimistic step past the watermark. If the chapter ends without more pages,
      // render() advances to the next spine instead of re-drawing this last page.
      section->currentPage++;
      pendingForwardPastEnd = true;
    } else {
      // We don't want to delete the section mid-render, so grab the semaphore
      {
        RenderLock lock(*this);
        nextPageNumber = 0;
        currentSpineIndex++;
        section.reset();
      }
    }
  } else {
    if (section->currentPage > 0) {
      section->currentPage--;
    } else if (currentSpineIndex > 0) {
      // We don't want to delete the section mid-render, so grab the semaphore
      {
        RenderLock lock(*this);
        nextPageNumber = 0;
        pendingPageJump = std::numeric_limits<uint16_t>::max();
        currentSpineIndex--;
        section.reset();
      }
    }
  }
  lastPageTurnTime = millis();
  requestUpdate();
}

// TODO: Failure handling
void EpubReaderActivity::render(RenderLock&& lock) {
  if (!epub) {
    return;
  }

  const auto showPendingSyncSaveError = [this]() {
    if (!pendingSyncSaveError) return;
    pendingSyncSaveError = false;
    GUI.drawPopup(renderer, tr(STR_SAVE_PROGRESS_FAILED));
  };

  // A section build failure (e.g. an invalid/corrupt EPUB that fails XML parsing) leaves the
  // "Indexing" popup on screen with no way forward. Surface an explicit error instead of hanging.
  // clearScreen first so the error popup doesn't overlay the stale "Indexing" popup.
  const auto showBuildError = [this]() {
    renderer.clearScreen();
    GUI.drawPopup(renderer, tr(STR_INDEX_FAILED));
    automaticPageTurnActive = false;
  };

  // edge case handling for sub-zero spine index
  if (currentSpineIndex < 0) {
    currentSpineIndex = 0;
  }
  // based bounds of book, show end of book screen
  if (currentSpineIndex > epub->getSpineItemsCount()) {
    currentSpineIndex = epub->getSpineItemsCount();
  }

  // Show end of book screen
  if (currentSpineIndex == epub->getSpineItemsCount()) {
    // Sole load site: runs on the render task (serialized by RenderLock); the main
    // task only reads the suggestions once the loaded flag is published
    endOfBookOptions.loadOnce(epub->getPath());
    renderer.clearScreen();
    endOfBookOptions.render(renderer, mappedInput);
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    return;
  }

  // Apply screen viewable areas and additional padding
  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  // Permanent top chrome inset for battery (top-right) + clock (top-center).
  orientedMarginTop += SETTINGS.screenMargin + ReaderUtils::kReaderTopChromeExtra;
  orientedMarginLeft += SETTINGS.screenMargin;
  orientedMarginRight += SETTINGS.screenMargin;

  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  // Bottom: match top air + never less than status bar / dictionary button strip.
  const int hintStrip = UITheme::getInstance().getMetrics().buttonHintsHeight;
  int bottomReserve = SETTINGS.screenMargin + ReaderUtils::kReaderBottomChromeExtra;
  bottomReserve = std::max(bottomReserve, static_cast<int>(statusBarHeight));
  bottomReserve = std::max(bottomReserve, hintStrip);
  if (automaticPageTurnActive &&
      (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight())) {
    bottomReserve = std::max(
        bottomReserve, static_cast<int>(statusBarHeight + UITheme::getInstance().getMetrics().statusBarVerticalMargin));
  }
  orientedMarginBottom += bottomReserve + ReaderUtils::kReaderBottomChromePad;

  const uint16_t viewportWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
  const uint16_t viewportHeight = renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom;
  // Capture for loop()'s lazy partial-extension start (must match this render's layout params).
  buildViewportWidth = viewportWidth;
  buildViewportHeight = viewportHeight;

  const ReaderRenderSpec renderSpec = SETTINGS.readerRenderSpec(viewportWidth, viewportHeight);

  if (!section) {
    const auto filepath = epub->getSpineItem(currentSpineIndex).href;
    LOG_DBG("ERS", "Loading file: %s, index: %d", filepath.c_str(), currentSpineIndex);
    section = std::unique_ptr<Section>(new Section(epub, currentSpineIndex, renderer));
    // Fresh section, fresh chance: a failed lazy extension start in a previous
    // section must not suppress watermark-triggered rebuilds for this one.
    partialRebuildStartFailed = false;

    // A finalized cache serves every page as-is. A partial cache (suspended build from a
    // previous session) serves its pages instantly too, but a build must still run to lay
    // out the rest -- it re-parses from the top in the background (HTML already cached,
    // pages are deterministic) and finalizes, so the partial machinery retires itself.
    const bool cacheLoaded = section->loadSectionFile(renderSpec);
    if (cacheLoaded) {
      // Matching render params means identical pagination, so the saved page number is valid
      // as-is: consume any pending settings-change reposition. Without this, a chapter total
      // saved while the section was still building (i.e. a watermark, not the real count)
      // would remap the resume page against the finalized count and teleport the reader.
      cachedChapterTotalPageCount = 0;
    }
    const bool cacheComplete = cacheLoaded && !section->isPartial();
    if (!cacheComplete) {
      if (section->isPartial()) {
        LOG_DBG("ERS", "Partial cache found (%d pages), resuming build...", section->pageCount);
      } else {
        LOG_DBG("ERS", "Cache not found, building...");
      }

      // Jumps that need the final pagination or the anchor map -- explicit page jumps,
      // fragment anchors, percent jumps, and cross-setting progress repositioning -- can't
      // resolve their landing page until the whole chapter is laid out, so they take the full
      // (blocking) build with the indexing popup. Everything else -- plain forward reads, resume,
      // and explicit page jumps -- only needs a specific page, so it builds incrementally to that
      // page and finishes the rest in loop(). The settings-change reposition (cachedChapterTotal*)
      // is NOT a full-build trigger: it's deferred to applyDeferredReposition() once the real page
      // count is known, so it never blocks the first page.
      // Only a percent jump truly needs the whole chapter up front (percent -> page needs the final
      // page count). Anchor jumps (TOC / chapter select / footnotes) resolve incrementally below --
      // the anchor is recorded as its page is laid out, so a chapter-top anchor lands on page 0
      // without indexing the whole chapter.
      const bool needsFullBuild = pendingPercentJump;
      if (needsFullBuild) {
        GUI.drawPopup(renderer, tr(STR_INDEXING));
        // The popup's own refresh is a plain FAST, so force the page that replaces it onto the HALF
        // ghost-cleanup path -- otherwise the "INDEXING" text ghosts under the rendered page.
        pagesUntilFullRefresh = 1;
        // No popup redraws while the framebuffer is lent to the build below;
        // the panel holds the popup displayed above (e-ink is persistent).
        const auto popupFn = [this]() {
          if (renderer.hasFrameBuffer()) GUI.drawPopup(renderer, tr(STR_INDEXING));
        };
        // Lend the framebuffer's 48 KB to the blocking full build; restored
        // (white) at scope exit, and the page render below redraws everything.
        GfxRenderer::FrameBufferLoan loan(renderer);
        if (!section->createSectionFile(renderSpec, popupFn)) {
          LOG_ERR("ERS", "Failed to persist page data to SD");
          section.reset();
          loan.end();  // restore before anything draws
          showBuildError();
          return;
        }
        loan.end();
      } else {
        // Lay out just enough to show the landing page; loop() builds the rest behind it. Show the
        // indexing popup up front only when the build will actually be slow: a large spine (its
        // whole HTML must be inflated before page 1 can lay out -- the giant single-spine case), or
        // a deep resume/jump that must lay out many pages to reach the landing page. Tiny sections
        // build in a blink and stay popup-free.
        const int target = pendingPageJump.has_value() ? *pendingPageJump : (nextPageNumber < 0 ? 0 : nextPageNumber);
        const bool anchorJump = !pendingAnchor.empty();

        // Landing well inside a partial: the page (or anchor, via the on-disk map) is already
        // servable, so don't restart the extension build now -- it re-lays out the WHOLE chapter
        // from page 0 (minutes of background CPU + SD writes on a giant spine), pure waste when
        // the reader never nears the watermark this session. loop() starts it lazily once the
        // reader is within PARTIAL_REBUILD_START_MARGIN pages of the watermark.
        if (section->isPartial() &&
            (anchorJump ? section->getPageForAnchor(pendingAnchor).has_value()
                        : target + PARTIAL_REBUILD_START_MARGIN < static_cast<int>(section->pageCount))) {
          LOG_DBG("ERS", "Partial covers target %d of %d; deferring extension build", target, section->pageCount);
        } else {
          const size_t spineBytes =
              epub->getCumulativeSpineItemSize(currentSpineIndex) -
              (currentSpineIndex > 0 ? epub->getCumulativeSpineItemSize(currentSpineIndex - 1) : 0);
          // Popup only when the build will actually be slow: a big spine whose HTML still needs
          // inflating (the multi-second cost), or a deep page target. A reopen with cached HTML builds
          // fast, so no popup -- that's what made an already-indexed book look like it was reindexing.
          // A partial cache that already covers the target page shows it instantly: never popup.
          const bool willInflate = !section->hasHtmlCache();
          bool showPopup;
          if (anchorJump) {
            // An anchor jump's cost is bounded by the anchor's page, not `target`. An anchor already
            // in the on-disk map (partial or finalized cache) lands instantly: no popup. Otherwise it
            // lies beyond the indexed watermark and the build may lay out the whole spine to find it,
            // so gate on spine size alone -- laying out a big spine takes seconds even with cached
            // HTML. Ordinary chapter-top TOC jumps resolve on page 0 and stay popup-free.
            showPopup = !section->findAnchor(pendingAnchor).has_value() && spineBytes > BUILD_POPUP_BYTE_THRESHOLD;
          } else {
            const bool targetAvailable = target < static_cast<int>(section->pageCount);
            showPopup = !targetAvailable && ((spineBytes > BUILD_POPUP_BYTE_THRESHOLD && willInflate) ||
                                             target > BUILD_POPUP_PAGE_THRESHOLD);
          }
          if (showPopup) {
            GUI.drawPopup(renderer, tr(STR_INDEXING));
            // HALF-clear the popup when the page replaces it, else "INDEXING" ghosts under the page.
            pagesUntilFullRefresh = 1;
          }
          // Mid-build popup surfacing for slow builds the predictive gates can't
          // see (image extraction/probing inside a single page, or any chunk
          // overrunning the deadline). The parser fires the callback before the
          // first image probe; buildPopupPending gates it to this blocking phase
          // so a background build in loop() can never draw over a displayed page.
          buildPopupPending = !showPopup;
          const unsigned long buildStartMs = millis();
          bool started;
          {
            // Lend the framebuffer's 48 KB to startBuild only (the spine HTML
            // inflation peak). The chunk loop below runs without it so the popup
            // can draw mid-build; background chunks never had the loan either.
            GfxRenderer::FrameBufferLoan loan(renderer);
            started = section->startBuild(renderSpec, [this] { showBuildPopup(); });
          }
          if (!started) {
            LOG_ERR("ERS", "Failed to start section build");
            section.reset();
            buildPopupPending = false;
            showBuildError();
            return;
          }
          while (!section->isBuildComplete() &&
                 (anchorJump ? !section->findAnchor(pendingAnchor) : static_cast<int>(section->pageCount) <= target)) {
            // Anchor jump: build until the anchor's page is laid out (usually page 0), checking a
            // partial's on-disk anchor map too so an already-indexed anchor resolves immediately.
            // Otherwise: build until the target page exists. loop() builds the rest behind it.
            if (buildPopupPending && millis() - buildStartMs >= BUILD_POPUP_DEADLINE_MS) {
              // The predictive gates guessed fast but the build blew the silent budget.
              showBuildPopup();
            }
            if (!section->buildSomeMore(BUILD_PAGES_PER_CHUNK)) {
              LOG_ERR("ERS", "Failed during incremental section build");
              section.reset();
              buildPopupPending = false;
              showBuildError();
              return;
            }
          }
          buildPopupPending = false;
        }
      }
    } else {
      LOG_DBG("ERS", "Cache found, skipping build...");
    }

    if (pendingPageJump.has_value()) {
      section->currentPage = *pendingPageJump;
      if (pendingClippingIndex != UINT16_MAX && pendingClippingIndex < CLIPPINGS.clippingCount()) {
        const Clipping* clipping = CLIPPINGS.clippingAt(pendingClippingIndex);
        const uint16_t fallbackPage = static_cast<uint16_t>(std::max(0, section->currentPage));
        std::string clippingText;
        clippingText.reserve(CLIPPING_TEXT_MAX);
        if (clipping) CLIPPINGS.readClippingText(*clipping, clippingText);
        section->currentPage =
            clipping ? resolveClippingJumpPage(*section, *clipping, clippingText, fallbackPage) : fallbackPage;
        LOG_DBG("ERS", "Resolved clipping %u to page %d", pendingClippingIndex, section->currentPage);
      } else if (pendingParagraphIndex != UINT16_MAX) {
        const uint16_t fallbackPage = static_cast<uint16_t>(std::max(0, section->currentPage));
        section->currentPage = resolveParagraphJumpPage(*section, pendingParagraphIndex, fallbackPage);
        LOG_DBG("ERS", "Resolved paragraph %u to page %d", pendingParagraphIndex, section->currentPage);
      }
      pendingClippingIndex = UINT16_MAX;
      pendingParagraphIndex = UINT16_MAX;
      pendingPageJump.reset();
    } else {
      section->currentPage = nextPageNumber;
      if (section->currentPage < 0) {
        section->currentPage = 0;
      }
    }

    if (!pendingAnchor.empty()) {
      // Resolve from the pages laid out so far and/or the on-disk map (finalized or partial).
      const auto page = section->findAnchor(pendingAnchor);
      if (page) {
        section->currentPage = *page;
        LOG_DBG("ERS", "Resolved anchor '%s' to page %d", pendingAnchor.c_str(), *page);
      } else {
        LOG_DBG("ERS", "Anchor '%s' not found in section %d", pendingAnchor.c_str(), currentSpineIndex);
      }
      pendingAnchor.clear();
    }

    if (pendingPercentJump && section->pageCount > 0) {
      // Apply the pending percent jump now that we know the new section's page count.
      int newPage = static_cast<int>(pendingSpineProgress * static_cast<float>(section->pageCount));
      if (newPage >= section->pageCount) {
        newPage = section->pageCount - 1;
      }
      section->currentPage = newPage;
      if (pendingClippingIndex != UINT16_MAX && pendingClippingIndex < CLIPPINGS.clippingCount()) {
        const Clipping* clipping = CLIPPINGS.clippingAt(pendingClippingIndex);
        const uint16_t fallbackPage = static_cast<uint16_t>(std::max(0, section->currentPage));
        std::string clippingText;
        clippingText.reserve(CLIPPING_TEXT_MAX);
        if (clipping) CLIPPINGS.readClippingText(*clipping, clippingText);
        section->currentPage =
            clipping ? resolveClippingJumpPage(*section, *clipping, clippingText, fallbackPage) : fallbackPage;
        LOG_DBG("ERS", "Resolved clipping %u to page %d", pendingClippingIndex, section->currentPage);
      } else if (pendingParagraphIndex != UINT16_MAX) {
        const uint16_t fallbackPage = static_cast<uint16_t>(std::max(0, section->currentPage));
        section->currentPage = resolveParagraphJumpPage(*section, pendingParagraphIndex, fallbackPage);
        LOG_DBG("ERS", "Resolved paragraph %u to page %d", pendingParagraphIndex, section->currentPage);
      }
      pendingClippingIndex = UINT16_MAX;
      pendingParagraphIndex = UINT16_MAX;
      pendingPercentJump = false;
    }
  }

  // Extend the build to the requested page if needed (for partials and in-progress builds).
  // This runs every render, so it covers both the first page and any forward turn that gets
  // ahead of the background builder; pages already built do no work here.
  //
  // Crossing a partial's watermark before the extension rebuild has caught up means a
  // synchronous wait spanning the remaining prefix re-layout -- potentially tens of
  // seconds on a giant spine. Show the indexing popup so it isn't a silent freeze
  // (the page that replaces it takes the HALF ghost-cleanup path). Ordinary window
  // catch-ups on a non-partial build are a page or two and stay popup-free.
  if (section->isPartial() && section->currentPage >= static_cast<int>(section->pageCount)) {
    GUI.drawPopup(renderer, tr(STR_INDEXING));
    pagesUntilFullRefresh = 1;
  }
  while (section->isPartial() && section->currentPage >= static_cast<int>(section->pageCount)) {
    // Start a build to extend a partial toward the requested page.
    if (!section->isBuilding() && !section->startBuild(renderSpec)) {
      LOG_ERR("ERS", "Failed to start partial extension build");
      section.reset();
      showBuildError();
      return;
    }
    // Extend until either the target page exists or the build completes.
    while (!section->isBuildComplete() && section->currentPage >= static_cast<int>(section->pageCount)) {
      if (!section->buildSomeMore(BUILD_PAGES_PER_CHUNK)) {
        LOG_ERR("ERS", "Failed during incremental section build");
        section.reset();
        showBuildError();
        return;
      }
    }
  }
  // For an in-progress incremental build, make sure the page we're about to show has been laid out.
  if (section->isBuilding()) {
    while (!section->isBuildComplete() && section->currentPage >= static_cast<int>(section->pageCount)) {
      if (!section->buildSomeMore(BUILD_PAGES_PER_CHUNK)) {
        LOG_ERR("ERS", "Failed during incremental section build");
        section.reset();
        showBuildError();
        return;
      }
    }
  }

  // The requested page is now as built as it will get. If it still lands past the end:
  // - After a forward page-turn while the chapter was still building (pendingForwardPastEnd),
  //   treat it as end-of-chapter and open the next spine — clamping back to the last page
  //   re-draws the same content with a different refresh and feels like a silent reformat.
  // - Otherwise clamp: UINT16_MAX "last page" sentinel, explicit jump past end, stale save.
  // Guarded on !isBuilding() because a still-building section's pageCount is only the current
  // watermark (not the final count) and has already been driven far enough by the loops above.
  if (!section->isBuilding() && section->pageCount > 0 &&
      section->currentPage >= static_cast<int>(section->pageCount)) {
    if (pendingForwardPastEnd) {
      pendingForwardPastEnd = false;
      nextPageNumber = 0;
      currentSpineIndex++;
      section.reset();
      requestUpdate();
      return;
    }
    section->currentPage = section->pageCount - 1;
    pendingForwardPastEnd = false;
  } else if (!section->isBuilding()) {
    // Landed on a real in-range page; drop the past-end hint. Keep it while building
    // so a later finalize can still promote to the next spine.
    pendingForwardPastEnd = false;
  }

  // Apply a deferred settings-change reposition now that the real page count is known (a no-op for
  // a plain resume / unchanged pagination). If still building, this defers to loop() on completion.
  applyDeferredReposition();

  renderer.clearScreen();

  if (section->pageCount == 0) {
    LOG_DBG("ERS", "No pages to render");
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_CHAPTER), true, EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    return;
  }

  if (section->currentPage < 0 || section->currentPage >= section->pageCount) {
    LOG_DBG("ERS", "Page out of bounds: %d (max %d)", section->currentPage, section->pageCount);
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_OUT_OF_BOUNDS), true, EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    return;
  }

  updateBookmarkFlag();

  {
    // Unified page read: the in-progress build's in-RAM table if it has reached the page,
    // otherwise the on-disk file (finalized section, or a partial from a previous session).
    auto p = section->loadPage(section->currentPage);
    if (!p) {
      LOG_ERR("ERS", "Failed to load page from SD - clearing section cache");
      automaticPageTurnActive = false;
      // Retrying rebuilds a transiently corrupt section and usually recovers, but a page that keeps
      // failing would loop forever on a blank screen, so bound the retries before giving up.
      const bool giveUp = ++pageLoadRetryCount > MAX_PAGE_LOAD_RETRIES;
      // Abandon (not suspend) any active build BEFORE clearing: clearCache deletes the files,
      // and the destructor's suspend would otherwise commit tables into a deleted handle.
      section->abandonBuild();
      section->clearCache();
      section.reset();
      if (giveUp) {
        LOG_ERR("ERS", "Page load retry limit reached, aborting");
        pageLoadRetryCount = 0;  // Reset so a later user-initiated navigation can try afresh
        renderer.clearScreen();
        renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_PAGE_LOAD_ERROR), true, EpdFontFamily::BOLD);
        renderer.displayBuffer();
        showPendingSyncSaveError();
        return;
      }
      requestUpdate();  // Try again after clearing cache
      showPendingSyncSaveError();
      return;
    }
    pageLoadRetryCount = 0;  // Reset the retry counter once a page loads cleanly

    // Collect footnotes from the loaded page
    currentPageFootnotes = std::move(p->footnotes);

    const auto start = millis();
    renderContents(std::move(p), orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
    LOG_DBG("ERS", "Rendered page in %dms", millis() - start);
    lastRenderCompleteMs = millis();
  }
  // Only persist when the position actually changed. render() also runs on menu,
  // bookmark and screenshot re-renders, and writeAtomic is several FAT ops for 6 bytes.
  // Every real page turn changes currentPage, so progress durability is unaffected.
  if (currentSpineIndex != lastSavedSpineIndex || section->currentPage != lastSavedPage ||
      section->pageCount != lastSavedPageCount) {
    if (saveProgress(currentSpineIndex, section->currentPage, section->estimatedTotalPages())) {
      lastSavedSpineIndex = currentSpineIndex;
      lastSavedPage = section->currentPage;
      lastSavedPageCount = section->estimatedTotalPages();
    }
  }

  showPendingSyncSaveError();

  if (pendingScreenshot) {
    pendingScreenshot = false;
    ScreenshotUtil::takeScreenshot(renderer);
  }

  if (showBookmarkMessage) {
    GUI.drawPopup(renderer, bookmarkRemoved ? tr(STR_BOOKMARK_REMOVED) : tr(STR_BOOKMARK_ADDED));
  }

  if (showDictionaryMessage) {
    GUI.drawPopup(renderer, I18N.get(dictionaryMessageId));
  }
}

void EpubReaderActivity::drawClippingHighlights(const Page& page, const int fontId, const int orientedMarginTop,
                                                const int orientedMarginLeft) const {
  if (!section || !CLIPPINGS.hasClippings()) {
    return;
  }

  std::array<ClippingPageMatch, CLIPPING_MAX_PAGE_MATCHES> matches;
  uint16_t matchCount = 0;
  const bool canUseStoredRanges = section->pageCount > 0 && section->pageCount <= UINT16_MAX &&
                                  section->currentPage >= 0 && section->currentPage < section->pageCount;
  const uint16_t currentPage = canUseStoredRanges ? static_cast<uint16_t>(section->currentPage) : 0;
  const uint16_t currentPageCount = canUseStoredRanges ? static_cast<uint16_t>(section->pageCount) : 0;
  std::string clippingText;
  clippingText.reserve(CLIPPING_TEXT_MAX);
  for (const Clipping& clipping : CLIPPINGS.getClippings()) {
    if (clipping.spineIndex != static_cast<uint16_t>(currentSpineIndex)) {
      continue;
    }
    ClippingPageMatch match;
    const bool matchedStoredRange =
        canUseStoredRanges && findClippingStoredRangeOnPage(page, clipping, currentPage, currentPageCount, match);
    const bool shouldSearchText = !canUseStoredRanges || clipping.pageCount != currentPageCount ||
                                  (currentPage >= clipping.startPage && currentPage <= clipping.endPage);
    bool matchedText = false;
    if (!matchedStoredRange && shouldSearchText) {
      clippingText.clear();
      if (CLIPPINGS.readClippingText(clipping, clippingText)) {
        matchedText = findClippingTextOnPage(page, clippingText, match);
      }
    }
    if (matchedStoredRange || matchedText) {
      matches[matchCount++] = match;
      if (matchCount >= matches.size()) {
        break;
      }
    }
  }
  if (matchCount == 0) {
    return;
  }

  const auto isHighlightedWord = [&matches, matchCount](const uint16_t pageWordIndex) {
    for (uint16_t matchIndex = 0; matchIndex < matchCount; ++matchIndex) {
      if (pageWordIndex >= matches[matchIndex].startWord && pageWordIndex <= matches[matchIndex].endWord) {
        return true;
      }
    }
    return false;
  };

  forEachVisiblePageWord(page, [&](const uint16_t pageWordIndex, const PageLine& line, const TextBlock& block,
                                   const size_t i) {
    if (!isHighlightedWord(pageWordIndex)) {
      return true;
    }

    if (i >= block.wordCount()) {
      return true;
    }

    const auto wordIndex = static_cast<uint16_t>(i);
    const char* wordText = block.wordText(wordIndex);
    const bool hasEmSpace = hasEmSpacePrefix(wordText);
    const char* visibleText = wordText + (hasEmSpace ? 3 : 0);
    const auto textStyle = static_cast<EpdFontFamily::Style>(block.wordStyle(wordIndex) & ~EpdFontFamily::UNDERLINE);
    const int skipX = hasEmSpace ? renderer.getTextAdvanceX(fontId, "\xe2\x80\x83", textStyle) : 0;
    const int wordX = orientedMarginLeft + line.xPos + block.wordXpos(wordIndex) + skipX;
    const int wordY = orientedMarginTop + line.yPos;
    int wordW = renderer.getTextAdvanceX(fontId, wordText, textStyle) - skipX;
    const int wordH = renderer.getLineHeight(fontId);
    if (wordIndex + 1 < block.wordCount()) {
      const uint16_t nextIndex = static_cast<uint16_t>(wordIndex + 1);
      const char* nextWordText = block.wordText(nextIndex);
      const bool nextHasEmSpace = hasEmSpacePrefix(nextWordText);
      const auto nextTextStyle =
          static_cast<EpdFontFamily::Style>(block.wordStyle(nextIndex) & ~EpdFontFamily::UNDERLINE);
      const int nextSkipX = nextHasEmSpace ? renderer.getTextAdvanceX(fontId, "\xe2\x80\x83", nextTextStyle) : 0;
      const int nextWordX = orientedMarginLeft + line.xPos + block.wordXpos(nextIndex) + nextSkipX;
      if (isHighlightedWord(static_cast<uint16_t>(pageWordIndex + 1)) && nextWordX > wordX + wordW) {
        wordW = nextWordX - wordX;
      } else if (nextWordX > wordX && wordW > nextWordX - wordX) {
        wordW = nextWordX - wordX;
      }
    }
    if (wordW > 0) {
      renderer.fillRectDither(wordX, wordY, wordW, wordH, Color::LightGray);
      renderer.drawText(fontId, wordX, wordY, visibleText, true, textStyle);
    }
    return true;
  });
}

bool EpubReaderActivity::applyDeferredReposition() {
  if (cachedChapterTotalPageCount == 0 || !section || section->isBuilding()) {
    return false;
  }
  bool changed = false;
  // Only remap when the chapter actually re-paginated (e.g. after a settings change). A plain
  // resume has identical pagination, so section->pageCount == cachedChapterTotalPageCount and
  // nothing moves.
  if (currentSpineIndex == cachedSpineIndex && section->pageCount != cachedChapterTotalPageCount) {
    const float progress = static_cast<float>(section->currentPage) / static_cast<float>(cachedChapterTotalPageCount);
    int newPage = static_cast<int>(progress * static_cast<float>(section->pageCount));
    if (newPage < 0) newPage = 0;
    if (section->pageCount > 0 && newPage >= static_cast<int>(section->pageCount)) {
      newPage = section->pageCount - 1;
    }
    if (newPage != section->currentPage) {
      section->currentPage = newPage;
      changed = true;
    }
  }
  cachedChapterTotalPageCount = 0;  // consumed; don't read cached progress again
  return changed;
}

bool EpubReaderActivity::saveProgress(int spineIndex, int currentPage, int pageCount) {
  return EpubReaderUtils::saveProgress(*epub, spineIndex, currentPage, pageCount);
}
void EpubReaderActivity::renderContents(std::unique_ptr<Page> page, const int orientedMarginTop,
                                        const int orientedMarginRight, const int orientedMarginBottom,
                                        const int orientedMarginLeft) {
  const auto t0 = millis();
  const int fontId = SETTINGS.getReaderFontId();

  // The image pixel-cache RAM slot lives for exactly one page render (it feeds
  // the BW double-refresh and every grayscale band pass); release it on every
  // exit so nothing stays resident across page turns.
  struct PxcSlotGuard {
    ~PxcSlotGuard() { ImageBlock::releaseRenderCache(); }
  } pxcSlotGuard;

  // Font prewarm: scan pass accumulates text, then prewarm, then real render
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);  // scan pass
  scope.endScanAndPrewarm();
  const auto tPrewarm = millis();

  const bool pageHasImages = page->hasImages();
  const bool pageHasImagesNeedingDecode = pageHasImages && page->hasImagesNeedingDecode();
  const bool manualRefreshPending = forcedRefreshPending;
  forcedRefreshPending = false;
  // Open path #2: first ink skips text AA so BW refresh is the critical path.
  // Images still need their grey path when present; only pure text AA is deferred.
  const bool skipTextAaThisFrame = openDeferTextAa && !pageHasImages;
  const bool needsTextGrayscale = SETTINGS.textAntiAliasing && !skipTextAaThisFrame;
  const bool needsAnyGrayscale = needsTextGrayscale || pageHasImages;
  const bool tiledGrayscale = needsAnyGrayscale && renderer.supportsStripGrayscale();
  // Whole-plane buffering only pays when the BW refresh genuinely runs async
  // underneath it; on blocking panels (X3) it would just spend ~50 KB for the
  // identical serial timing. Image pages take the blocking double-FAST path
  // below (no async refresh is ever started), so they'd spend the buffers with
  // nothing in flight to overlap.
  const bool overlapRefresh = tiledGrayscale && renderer.supportsAsyncRefresh() && !pageHasImages;
  auto renderGrayscalePass = [&]() {
    if (needsTextGrayscale) {
      page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);
    } else {
      page->renderImages(renderer, fontId, orientedMarginLeft, orientedMarginTop);
    }
  };

  if (pageHasImagesNeedingDecode) {
    page->renderWithImagePlaceholders(renderer, fontId, orientedMarginLeft, orientedMarginTop);
    renderStatusBar();
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    renderer.clearScreen();
  }

  page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);
  drawClippingHighlights(*page, fontId, orientedMarginTop, orientedMarginLeft);
  renderStatusBar();
  const auto tBwRender = millis();

  if (pageHasImages) {
    // Double FAST_REFRESH with selective image blanking (pablohc's technique):
    // HALF_REFRESH sets particles too firmly for the grayscale LUT to adjust.
    // Instead, blank only the image area and do two fast refreshes.
    // Step 1: Display page with image area blanked (text appears, image area white)
    // Step 2: Re-render with images and display again (images appear clean)
    int16_t imgX, imgY, imgW, imgH;
    if (page->getImageBoundingBox(imgX, imgY, imgW, imgH)) {
      // Image pages intentionally bypass the regular refresh cadence. Preserve
      // the manual clean pass before their double-FAST grayscale pipeline.
      if (manualRefreshPending) {
        renderer.displayBuffer(HalDisplay::HALF_REFRESH);
      }
      renderer.fillRect(imgX + orientedMarginLeft, imgY + orientedMarginTop, imgW, imgH, false);
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);

      // Re-render page content to restore images into the blanked area
      // Status bar is not re-rendered here to avoid reading stale dynamic values (e.g. battery %)
      page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);
      drawClippingHighlights(*page, fontId, orientedMarginTop, orientedMarginLeft);
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    } else {
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    }
    // The image's own page is handled above and doesn't count toward the full
    // refresh cadence. But the grayscale pass below leaves gray charge in the
    // image region that a plain fast diff on the *next* page can't clear, so
    // text there ghosts gray (#2190). Force the next ordinary page onto the
    // HALF ghost-cleanup path, which drives every pixel to its target
    // regardless of residue.
    pagesUntilFullRefresh = 1;
  } else {
    // Async form: start the waveform and return so the grayscale plane rendering
    // below overlaps the panel's refresh time instead of following it.
    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh, overlapRefresh);
  }
  const auto tDisplay = millis();

  // First readable ink is on the panel after the BW (or image) refresh above.
  // Log once per open; schedule AA catch-up when this frame skipped text greys.
  if (!openFirstInkLogged) {
    openFirstInkLogged = true;
    const uint32_t wall = openWallStartMs != 0 ? (millis() - openWallStartMs) : (millis() - t0);
    LOG_DBG("ERS",
            "OPEN first_ink %lums (bw_render=%lums display=%lums) fast=%d defer_aa=%d aa_skipped=%d",
            static_cast<unsigned long>(wall), static_cast<unsigned long>(tBwRender - tPrewarm),
            static_cast<unsigned long>(tDisplay - tBwRender), openPreferFastFirstRefresh ? 1 : 0,
            openDeferTextAa ? 1 : 0, skipTextAaThisFrame ? 1 : 0);
  }
  if (skipTextAaThisFrame) {
    openDeferTextAa = false;
    if (SETTINGS.textAntiAliasing) {
      pendingDeferredOpenAa = true;
    }
  }

  // Tiled grayscale: render each plane band-by-band, leaving the BW
  // framebuffer intact so no full-frame storeBwBuffer is needed; controller
  // RAM is re-synced from the live framebuffer afterward. The page is
  // re-rendered ceil(H/STRIP_ROWS) times per plane, but renderCharImpl culls
  // out-of-band glyphs before decode so the cost stays close to one render.
  // Both text (drawPixel) and images (DirectPixelWriter) honor the active
  // strip target. When the BW refresh above went out async, the plane
  // rendering below overlaps the panel's refresh time; only the controller
  // RAM writes wait for BUSY.
  if (tiledGrayscale) {
    constexpr int STRIP_ROWS = 80;
    const int gh = renderer.getDisplayHeight();
    const int gwBytes = renderer.getDisplayWidthBytes();
    const size_t planeBytes = static_cast<size_t>(gwBytes) * gh;

    // Render one plane band-by-band into a whole-plane buffer without touching
    // the controller, so it can run while the refresh is still in flight.
    auto renderPlaneToBuffer = [&](const bool lsbPlane, uint8_t* buf) {
      renderer.setRenderMode(lsbPlane ? GfxRenderer::GRAYSCALE_LSB : GfxRenderer::GRAYSCALE_MSB);
      for (int y = 0; y < gh; y += STRIP_ROWS) {
        const int rows = (gh - y < STRIP_ROWS) ? (gh - y) : STRIP_ROWS;
        renderer.beginStripTarget(buf + static_cast<size_t>(y) * gwBytes, y, rows);
        renderer.clearScreen(0x00);
        renderGrayscalePass();
        renderer.endStripTarget();
      }
    };

    // Tiered on heap pressure: two plane buffers hide both plane renders
    // inside the refresh wait; one hides the LSB render (its buffer is reused
    // for MSB after streaming); none falls back to the strip-scratch flow with
    // no overlap. Each buffer is only attempted when it leaves ~60 KB free so
    // the pass never starves concurrent allocations: the next page re-render
    // allocates through throwing std::string paths that abort() on OOM under
    // -fno-exceptions, so a plane buffer that "fits" but eats the render
    // headroom is worse than the strip fallback. Blocking panels skip the
    // buffers entirely (nothing to overlap).
    constexpr size_t PLANE_BUF_HEADROOM = 60000;
    // Free-heap alone ignores fragmentation: taking the largest block for a
    // plane can leave only slivers behind even when total headroom looks fine.
    // Require the block to fit the plane with 16 KB contiguous to spare, which
    // also keeps the advance-table batch scratch viable mid-render (same
    // rationale as BACKGROUND_BUILD_MIN_MAX_ALLOC).
    constexpr size_t PLANE_BUF_MAX_ALLOC_RESERVE = 16 * 1024;
    const auto planeBufFits = [planeBytes] {
      return ESP.getFreeHeap() >= planeBytes + PLANE_BUF_HEADROOM &&
             ESP.getMaxAllocHeap() >= planeBytes + PLANE_BUF_MAX_ALLOC_RESERVE;
    };
    auto lsbPlaneBuf = (overlapRefresh && planeBufFits()) ? makeUniqueNoThrow<uint8_t[]>(planeBytes) : nullptr;
    auto msbPlaneBuf = (lsbPlaneBuf && planeBufFits()) ? makeUniqueNoThrow<uint8_t[]>(planeBytes) : nullptr;

    if (lsbPlaneBuf) {
      renderPlaneToBuffer(true, lsbPlaneBuf.get());
      if (msbPlaneBuf) renderPlaneToBuffer(false, msbPlaneBuf.get());
      const auto tGrayRender = millis();

      renderer.waitRefreshComplete();
      const auto tWait = millis();

      renderer.writeGrayscalePlaneStrip(true, lsbPlaneBuf.get(), 0, gh);
      if (msbPlaneBuf) {
        renderer.writeGrayscalePlaneStrip(false, msbPlaneBuf.get(), 0, gh);
      } else {
        renderPlaneToBuffer(false, lsbPlaneBuf.get());
        renderer.writeGrayscalePlaneStrip(false, lsbPlaneBuf.get(), 0, gh);
      }
      const auto tGrayWrite = millis();

      renderer.setRenderMode(GfxRenderer::BW);
      renderer.displayGrayBuffer();
      const auto tGrayDisplay = millis();

      // BW framebuffer is intact; re-sync controller RAM for the next
      // differential page turn directly from it.
      renderer.cleanupGrayscaleWithFrameBuffer();
      const auto tEnd = millis();

      LOG_DBG("ERS",
              "Page render (tiled async): prewarm=%lums bw_render=%lums display=%lums gray_render=%lums "
              "wait=%lums gray_write=%lums gray_display=%lums cleanup=%lums total=%lums (planes buffered: %d)",
              tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tGrayRender - tDisplay, tWait - tGrayRender,
              tGrayWrite - tWait, tGrayDisplay - tGrayWrite, tEnd - tGrayDisplay, tEnd - t0, msbPlaneBuf ? 2 : 1);
    } else {
      // Per-strip scratch tier: blocking panels (X3) and the OOM fallback.
      // The strip writes below need the panel idle, so wait out any pending
      // async refresh first (no-op on blocking panels).
      auto scratch = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(gwBytes) * STRIP_ROWS);
      renderer.waitRefreshComplete();
      if (!scratch) {
        LOG_ERR("ERS", "OOM: grayscale strip scratch (%d bytes); skipping AA this page", gwBytes * STRIP_ROWS);
        if (overlapRefresh) {
          // The BW refresh ran the shadow-free async path, so controller RAM's
          // differential baseline was never rebuilt. Even with AA skipped it must
          // be re-synced from the intact BW framebuffer, or the next differential
          // update diffs against stale contents.
          renderer.cleanupGrayscaleWithFrameBuffer();
        }
      } else {
        // Bands may be streamed in any order: X4 windows each via setRamArea,
        // X3 via PTL.
        renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
        for (int y = 0; y < gh; y += STRIP_ROWS) {
          const int rows = (gh - y < STRIP_ROWS) ? (gh - y) : STRIP_ROWS;
          renderer.beginStripTarget(scratch.get(), y, rows);
          renderer.clearScreen(0x00);
          renderGrayscalePass();
          renderer.endStripTarget();
          renderer.writeGrayscalePlaneStrip(true, scratch.get(), y, rows);
        }
        const auto tGrayLsb = millis();

        // MSB plane.
        renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
        for (int y = 0; y < gh; y += STRIP_ROWS) {
          const int rows = (gh - y < STRIP_ROWS) ? (gh - y) : STRIP_ROWS;
          renderer.beginStripTarget(scratch.get(), y, rows);
          renderer.clearScreen(0x00);
          renderGrayscalePass();
          renderer.endStripTarget();
          renderer.writeGrayscalePlaneStrip(false, scratch.get(), y, rows);
        }
        const auto tGrayMsb = millis();

        renderer.setRenderMode(GfxRenderer::BW);
        renderer.displayGrayBuffer();
        const auto tGrayDisplay = millis();

        // BW framebuffer is intact; re-sync controller RAM for the next
        // differential page turn directly from it.
        renderer.cleanupGrayscaleWithFrameBuffer();
        const auto tCleanup = millis();

        const auto tEnd = millis();
        LOG_DBG("ERS",
                "Page render (tiled): prewarm=%lums bw_render=%lums display=%lums gray_lsb=%lums "
                "gray_msb=%lums gray_display=%lums cleanup=%lums total=%lums",
                tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tGrayLsb - tDisplay, tGrayMsb - tGrayLsb,
                tGrayDisplay - tGrayMsb, tCleanup - tGrayDisplay, tEnd - t0);
      }
    }
  } else {
    // Fallback path for a controller without strip support. grayscale rendering
    // TODO: Only do this if font supports it
    if (needsAnyGrayscale) {
      // Save the BW frame before the grayscale passes overwrite it, restore
      // after. Only needed when grayscale actually renders.
      if (!renderer.storeBwBuffer()) {
        LOG_ERR("ERS", "Failed to store BW buffer for grayscale render; skipping grayscale this page");
        const auto tEnd = millis();
        LOG_DBG("ERS", "Page render: prewarm=%lums bw_render=%lums display=%lums total=%lums", tPrewarm - t0,
                tBwRender - tPrewarm, tDisplay - tBwRender, tEnd - t0);
        return;
      }
      const auto tBwStore = millis();

      renderer.clearScreen(0x00);
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
      renderGrayscalePass();
      renderer.copyGrayscaleLsbBuffers();
      const auto tGrayLsb = millis();

      // Render and copy to MSB buffer
      renderer.clearScreen(0x00);
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
      renderGrayscalePass();
      renderer.copyGrayscaleMsbBuffers();
      const auto tGrayMsb = millis();

      // display grayscale part
      renderer.displayGrayBuffer();
      const auto tGrayDisplay = millis();
      renderer.setRenderMode(GfxRenderer::BW);
      renderer.restoreBwBuffer();
      // Match the tiled path: re-sync controller RAM from the BW framebuffer so
      // the next differential (especially with AA off) does not ghost against
      // stale gray planes — that can look like the same page "reformatted".
      renderer.cleanupGrayscaleWithFrameBuffer();
      const auto tBwRestore = millis();

      const auto tEnd = millis();
      LOG_DBG("ERS",
              "Page render: prewarm=%lums bw_render=%lums display=%lums bw_store=%lums "
              "gray_lsb=%lums gray_msb=%lums gray_display=%lums bw_restore=%lums total=%lums",
              tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tBwStore - tDisplay, tGrayLsb - tBwStore,
              tGrayMsb - tGrayLsb, tGrayDisplay - tGrayMsb, tBwRestore - tGrayDisplay, tEnd - t0);
    } else {
      // No text AA and no images: BW frame already displayed above, no grayscale
      // to render, so no save/restore.
      const auto tEnd = millis();
      LOG_DBG("ERS", "Page render: prewarm=%lums bw_render=%lums display=%lums total=%lums", tPrewarm - t0,
              tBwRender - tPrewarm, tDisplay - tBwRender, tEnd - t0);
    }
  }
}

bool EpubReaderActivity::formatTimeLeftLabel(char* buf, const size_t len, const bool bookEstimate) const {
  if (!buf || len == 0) {
    return false;
  }
  if (!section || !epub) {
    return false;
  }

  // Live total reading time: persisted history + current session (excluding menu/dict pause).
  uint32_t liveTotalSeconds = readingStats.totalReadingSeconds;
  if (readingSessionStartMs != 0UL) {
    const unsigned long nowMs = millis();
    unsigned long effectiveNow = nowMs;
    if (statsPauseStartMs != 0UL && statsPauseStartMs <= nowMs) {
      effectiveNow = statsPauseStartMs;  // freeze during pause
    }
    if (effectiveNow >= readingSessionStartMs) {
      const uint32_t sessionSecs = static_cast<uint32_t>((effectiveNow - readingSessionStartMs) / 1000UL);
      if (liveTotalSeconds <= UINT32_MAX - sessionSecs) {
        liveTotalSeconds += sessionSecs;
      } else {
        liveTotalSeconds = UINT32_MAX;
      }
    }
  }

  const uint32_t secPerPage =
      estimateSecondsPerPage(readingStats.avgSecondsPerForwardPage, readingStats.paceSampleCount, liveTotalSeconds,
                             readingStats.totalPagesTurned);
  if (secPerPage == 0) {
    snprintf(buf, len, "%s", tr(STR_TIME_LEFT_CALCULATING));
    return true;
  }

  const int currentPage1 = section->currentPage + 1;
  const int chapterPages = std::max(1, static_cast<int>(section->estimatedTotalPages()));
  const float sectionChapterProg =
      static_cast<float>(currentPage1) / static_cast<float>(chapterPages);
  const float bookProg = epub->calculateProgress(currentSpineIndex, sectionChapterProg);  // 0..1
  const float chapterStartProg = epub->calculateProgress(currentSpineIndex, 0.0f);
  const float chapterEndProg = epub->calculateProgress(currentSpineIndex, 1.0f);

  float remainingPages = 0.0f;
  if (bookEstimate) {
    remainingPages =
        estimateRemainingBookPages(chapterPages, currentPage1, bookProg, chapterStartProg, chapterEndProg);
  } else {
    remainingPages = static_cast<float>(std::max(0, chapterPages - currentPage1));
  }

  uint32_t seconds = 0;
  if (!estimateTimeLeftFromPages(remainingPages, secPerPage, seconds)) {
    snprintf(buf, len, "%s", tr(STR_TIME_LEFT_CALCULATING));
    return true;
  }

  // Book ETA only: damp noise so a few pages cannot leap multi-hour estimates.
  // Chapter time-left stays raw (short horizon; jumps are less alarming).
  if (bookEstimate) {
    // Seed from last cached book ETA when available (dashboard / prior session).
    if (smoothedBookTimeLeftSeconds == 0 && readingStats.estimatedTimeLeftSeconds > 0) {
      smoothedBookTimeLeftSeconds = readingStats.estimatedTimeLeftSeconds;
    }
    seconds = smoothTimeLeftSeconds(smoothedBookTimeLeftSeconds, seconds);
    smoothedBookTimeLeftSeconds = seconds;
  }

  const char* suffix = bookEstimate ? tr(STR_TIME_LEFT_IN_BOOK) : tr(STR_TIME_LEFT_IN_CHAPTER);
  if (seconds < 60) {
    snprintf(buf, len, "<1m %s", suffix);
  } else if (seconds < 3600) {
    snprintf(buf, len, "%lum %s", static_cast<unsigned long>((seconds + 30) / 60), suffix);
  } else {
    const unsigned long hours = seconds / 3600;
    const unsigned long mins = (seconds % 3600) / 60;
    if (mins == 0) {
      snprintf(buf, len, "%luh %s", hours, suffix);
    } else {
      snprintf(buf, len, "%luh %lum %s", hours, mins, suffix);
    }
  }
  return true;
}

void EpubReaderActivity::renderStatusBar() const {
  // Calculate progress in book. Use the estimated total while a giant spine is still building so
  // "page X of Y" and the progress bar don't read off the small build watermark.
  const int currentPage = section->currentPage + 1;
  const int chapterPageCount = std::max(1, static_cast<int>(section->estimatedTotalPages()));
  const float sectionChapterProg =
      static_cast<float>(currentPage) / static_cast<float>(chapterPageCount);
  const float bookProgress01 = epub->calculateProgress(currentSpineIndex, sectionChapterProg);
  const float bookProgress = bookProgress01 * 100;

  // Whole-book page estimate from this chapter's density (same model as time-left).
  int bookPage = currentPage;
  int bookPageCount = chapterPageCount;
  bool bookPageEstimated = true;
  {
    const float chapterStart = epub->calculateProgress(currentSpineIndex, 0.0f);
    const float chapterEnd = epub->calculateProgress(currentSpineIndex, 1.0f);
    const float chapterSpan = chapterEnd - chapterStart;
    if (chapterPageCount > 0 && chapterSpan > 0.001f) {
      const float pagesPerBookFrac = static_cast<float>(chapterPageCount) / chapterSpan;
      bookPageCount = std::max(1, static_cast<int>(pagesPerBookFrac + 0.5f));
      bookPage = std::max(1, std::min(bookPageCount, static_cast<int>(bookProgress01 * pagesPerBookFrac + 0.5f)));
    }
  }

  std::string bookTitle;
  std::string chapterTitle;

  int textYOffset = 0;
  const auto sb = SETTINGS.statusBarSpec();

  if (automaticPageTurnActive) {
    // Show auto-turn on any title slot so the status still conveys the mode.
    const std::string autoMsg = tr(STR_AUTO_TURN_ENABLED) + std::to_string(60 * 1000 / pageTurnDuration);
    bookTitle = autoMsg;
    chapterTitle = autoMsg;

    // calculates textYOffset when rendering title in status bar
    const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();

    // offsets text if no status bar or progress bar only
    if (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight()) {
      textYOffset += UITheme::getInstance().getMetrics().statusBarVerticalMargin;
    }

  } else {
    if (sb.wantsBookTitle) {
      bookTitle = epub->getTitle();
    }
    if (sb.wantsChapterTitle) {
      chapterTitle = tr(STR_UNNAMED);
      const int tocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
      if (tocIndex != -1) {
        chapterTitle = epub->getTocItem(tocIndex).title;
      }
    }
  }

  char timeLeftBook[40] = {};
  char timeLeftChapter[40] = {};
  const char* bookTl =
      (sb.wantsTimeLeftBook && formatTimeLeftLabel(timeLeftBook, sizeof(timeLeftBook), true)) ? timeLeftBook : nullptr;
  const char* chapTl =
      (sb.wantsTimeLeftChapter && formatTimeLeftLabel(timeLeftChapter, sizeof(timeLeftChapter), false))
          ? timeLeftChapter
          : nullptr;

  // TOC chapter index (1-based): "Ch. 5/40". Fallback to spine when TOC is missing.
  int chapterIndex = 0;
  int chapterTotal = 0;
  if (epub) {
    chapterTotal = epub->getTocItemsCount();
    if (chapterTotal > 0) {
      const int tocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
      if (tocIndex >= 0) {
        chapterIndex = tocIndex + 1;
      } else {
        // Spine without a TOC entry: nearest prior TOC chapter, else 1.
        chapterIndex = 1;
        for (int s = currentSpineIndex; s >= 0; --s) {
          const int t = epub->getTocIndexForSpineIndex(s);
          if (t >= 0) {
            chapterIndex = t + 1;
            break;
          }
        }
      }
    } else {
      // No TOC: use spine position as a rough chapter index.
      chapterTotal = std::max(1, epub->getSpineItemsCount());
      chapterIndex = std::min(chapterTotal, std::max(1, currentSpineIndex + 1));
    }
  }

  GUI.drawStatusBar(renderer, bookProgress, currentPage, chapterPageCount, bookTitle, 0, textYOffset, true,
                    currentPageBookmarked, section->isBuilding(), bookTl, chapTl, /*drawTopBattery=*/true, bookPage,
                    bookPageCount, bookPageEstimated, chapterIndex, chapterTotal, chapterTitle);
}

void EpubReaderActivity::navigateToHref(const std::string& hrefStr, const bool savePosition) {
  if (!epub) return;

  // Push current position onto saved stack
  if (savePosition && section && footnoteDepth < MAX_FOOTNOTE_DEPTH) {
    savedPositions[footnoteDepth] = {currentSpineIndex, section->currentPage};
    footnoteDepth++;
    LOG_DBG("ERS", "Saved position [%d]: spine %d, page %d", footnoteDepth, currentSpineIndex, section->currentPage);
  }

  // Extract fragment anchor (e.g. "#note1" or "chapter2.xhtml#note1")
  std::string anchor;
  const auto hashPos = hrefStr.find('#');
  if (hashPos != std::string::npos && hashPos + 1 < hrefStr.size()) {
    anchor = hrefStr.substr(hashPos + 1);
  }

  // Check for same-file anchor reference (#anchor only)
  bool sameFile = !hrefStr.empty() && hrefStr[0] == '#';

  int targetSpineIndex;
  if (sameFile) {
    targetSpineIndex = currentSpineIndex;
  } else {
    targetSpineIndex = epub->resolveHrefToSpineIndex(hrefStr);
  }

  if (targetSpineIndex < 0) {
    LOG_DBG("ERS", "Could not resolve href: %s", hrefStr.c_str());
    if (savePosition && footnoteDepth > 0) footnoteDepth--;  // undo push
    return;
  }

  {
    RenderLock lock(*this);
    pendingAnchor = std::move(anchor);
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = 0;
    section.reset();
  }
  requestUpdate();
  LOG_DBG("ERS", "Navigated to spine %d for href: %s", targetSpineIndex, hrefStr.c_str());
}

void EpubReaderActivity::restoreSavedPosition() {
  if (footnoteDepth <= 0) return;
  footnoteDepth--;
  const auto& pos = savedPositions[footnoteDepth];
  LOG_DBG("ERS", "Restoring position [%d]: spine %d, page %d", footnoteDepth, pos.spineIndex, pos.pageNumber);

  {
    RenderLock lock(*this);
    currentSpineIndex = pos.spineIndex;
    nextPageNumber = pos.pageNumber;
    section.reset();
  }
  requestUpdate();
}

void EpubReaderActivity::loadCachedBookmarks() {
  cachedBookmarks.clear();
  if (cachedBookmarks.capacity() < initialBookmarkCacheCapacity) {
    cachedBookmarks.reserve(initialBookmarkCacheCapacity);
  }
  if (!epub) {
    currentPageBookmarked = false;
    return;
  }

  BookmarkFile::load(epub->getPath(), cachedBookmarks);
  updateBookmarkFlag();
}

void EpubReaderActivity::addBookmark() {
  if (!section || !epub) {
    return;
  }
  LOG_DBG("ERS", "Toggle bookmark at spine %d, page %d", currentSpineIndex, section ? section->currentPage : -1);
  int currentPage;
  int pageCount;
  {
    RenderLock lock(*this);
    pageCount = section->estimatedTotalPages();
    currentPage = section->currentPage;
  }

  SavedProgressPosition progress = ProgressMapper::toSavedProgress(epub, getCurrentPosition());
  const ProgressRange pageRange = getPageProgressRange(epub, currentSpineIndex, currentPage, pageCount);

  const size_t bookmarkCountBeforeToggle = cachedBookmarks.size();
  cachedBookmarks.erase(std::remove_if(cachedBookmarks.begin(), cachedBookmarks.end(),
                                       [&](const BookmarkEntry& b) {
                                         return bookmarkMatchesProgress(b, currentSpineIndex, currentPage, pageCount,
                                                                        pageRange);
                                       }),
                        cachedBookmarks.end());
  if (cachedBookmarks.size() != bookmarkCountBeforeToggle) {
    bookmarkRemoved = true;
    currentPageBookmarked = false;
  } else {
    std::string pageText;
    if (currentPage >= 0 && currentPage < pageCount) {
      pageText = section->getTextFromSectionFile();
    }
    BookmarkEntry entry;
    entry.percentage = progress.percentage;
    entry.xpath = progress.xpath;
    entry.summary = BookmarkUtil::sanitizeBookmarkSummary(pageText);
    entry.computedSpineIndex = currentSpineIndex;
    entry.computedChapterPageCount = pageCount;
    entry.computedChapterProgress = currentPage;
    cachedBookmarks.insert(cachedBookmarks.begin(), entry);
    bookmarkRemoved = false;
    currentPageBookmarked = true;
  }

  if (!BookmarkFile::save(epub->getPath(), cachedBookmarks)) {
    LOG_ERR("ERS", "Failed to save bookmarks");
  }
  requestUpdate();
}

void EpubReaderActivity::updateBookmarkFlag() {
  if (!section || !epub || cachedBookmarks.empty()) {
    currentPageBookmarked = false;
    return;
  }
  const int pageCount = section->estimatedTotalPages();
  const ProgressRange pageRange = getPageProgressRange(epub, currentSpineIndex, section->currentPage, pageCount);
  currentPageBookmarked = std::any_of(cachedBookmarks.begin(), cachedBookmarks.end(), [&](const BookmarkEntry& b) {
    return bookmarkMatchesProgress(b, currentSpineIndex, section->currentPage, pageCount, pageRange);
  });
}

void EpubReaderActivity::setBookCompleted(bool isCompleted) {
  if (!epub || readingStats.isCompleted == isCompleted) {
    return;
  }

  readingStats.isCompleted = isCompleted;
  // Finished date is optional metadata; only stamp it when tracking is on.
  if (isCompleted && SETTINGS.readingStatsTrackingEnabled() && !readingStats.finishedDateManual) {
    ReadingStatsDateTime now;
    if (getCurrentLocalReadingStatsDateTime(now)) {
      readingStats.finishedDate = now.date;
    }
  }
  if (isCompleted) {
    if (SETTINGS.removeReadBooksFromRecents) {
      RECENT_BOOKS.removeByPath(epub->getPath());
      recentsEntryRemoved = true;
    }
    if (SETTINGS.moveFinishedToReadFolder && !isInReadFolder(epub->getPath())) {
      pendingReadFolderMove = true;
    }
    // Lifetime completed-book count is tracking; skip when tracking is off.
    if (SETTINGS.readingStatsTrackingEnabled() && globalReadingStats.completedBooks < UINT32_MAX) {
      globalReadingStats.completedBooks++;
    }
  } else {
    if (SETTINGS.removeReadBooksFromRecents) {
      RECENT_BOOKS.addBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath());
      recentsEntryRemoved = false;
    }
    pendingReadFolderMove = false;
    if (SETTINGS.readingStatsTrackingEnabled() && globalReadingStats.completedBooks > 0) {
      globalReadingStats.completedBooks--;
    }
  }

  // Always persist completion flag so Mark Finished / finished-folder work without tracking.
  readingStats.save(epub->getCachePath());
  if (SETTINGS.readingStatsTrackingEnabled()) {
    globalReadingStats.save();
  }
}

ScreenshotInfo EpubReaderActivity::getScreenshotInfo() const {
  ScreenshotInfo info;
  info.readerType = ScreenshotInfo::ReaderType::Epub;
  if (epub) {
    snprintf(info.title, sizeof(info.title), "%s", epub->getTitle().c_str());
    info.spineIndex = currentSpineIndex;
  }
  if (section) {
    info.currentPage = section->currentPage + 1;
    info.totalPages = section->estimatedTotalPages();
    if (epub && epub->getBookSize() > 0 && info.totalPages > 0) {
      const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(info.totalPages);
      int pct = static_cast<int>(epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f + 0.5f);
      if (pct < 0) pct = 0;
      if (pct > 100) pct = 100;
      info.progressPercent = pct;
    }
  }
  return info;
}

CrossPointPosition EpubReaderActivity::getCurrentPosition() const {
  const int currentPage = section ? section->currentPage : nextPageNumber;
  const int totalPages = section ? section->estimatedTotalPages() : cachedChapterTotalPageCount;
  std::optional<uint16_t> paragraphIndex;
  if (section && currentPage >= 0 && currentPage < section->pageCount) {
    const uint16_t paragraphPage =
        currentPage > 0 ? static_cast<uint16_t>(currentPage - 1) : static_cast<uint16_t>(currentPage);
    if (const auto pIdx = section->getParagraphIndexForPage(paragraphPage)) {
      paragraphIndex = *pIdx;
    }
  }

  CrossPointPosition localPos = {currentSpineIndex, currentPage, totalPages};
  if (paragraphIndex.has_value()) {
    localPos.paragraphIndex = *paragraphIndex;
    localPos.hasParagraphIndex = true;
  }
  return localPos;
}
