#include "RivuletReaderActivity.h"

#include <Epub/blocks/ImageBlock.h>
#include <Epub/converters/ImageDecoderFactory.h>
#include <Epub/converters/ImageDimsProbe.h>
#include <Epub/converters/PngToFramebufferConverter.h>
#include <Esp.h>
#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "BookStatsActivity.h"
#include "ChapterLoader.h"
#include "ClippingStore.h"
#include "CasperSettings.h"
#include "CasperState.h"
#include "DictionaryWordSelectActivity.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderSyncActivity.h"
#include "ProgressMapper.h"
#include "ReaderRenderKey.h"
#include "clippings/ClippingsManager.h"
#include "EpubReaderBookmarksActivity.h"
#include "EpubReaderChapterSelectionActivity.h"
#include "EpubReaderClippingListActivity.h"
#include "EpubReaderFootnotesActivity.h"
#include "EpubReaderMenuActivity.h"
#include "EpubReaderPercentSelectionActivity.h"

#include "MappedInputManager.h"
#include "ProgressFile.h"

#include "QrDisplayActivity.h"
#include "ReaderActivity.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "casper/CasperStats.h"
#include "SdCardFontSystem.h"
#include "activities/ActivityManager.h"
#include "activities/ActivityResult.h"  // ClippingJumpResult, MenuResult, …
#include "activities/settings/StatusBarSettingsActivity.h"
#include "activities/settings/TextSettingsActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "components/themes/BaseTheme.h"
#include "components/themes/penumbra/PenumbraTheme.h"
#include "fontIds.h"
#include "activities/home/BookActions.h"
#include "util/SystemLog.h"
#include "util/QrTimingLog.h"
#include "ReadingStatsUtils.h"
#include "util/BookCacheUtils.h"
#include "util/BookmarkFile.h"
#include "util/BookmarkUtil.h"
#include "util/CasperBookStore.h"

#include "util/CasperPaths.h"
#include "util/DictionaryRegistry.h"
#include "util/FinishedBooks.h"
#include "util/ScreenshotInfo.h"
#include "util/ScreenshotUtil.h"
#include "util/UiGhostPolicy.h"

#include "Epub/hyphenation/Hyphenator.h"

#include <climits>
#include <cmath>
#include <cctype>
#include <cstring>
#include <functional>

namespace {

constexpr size_t kInitialBookmarkCacheCapacity = 16;
constexpr float kBookmarkProgressEpsilon = 0.0025f;

bool bookmarkMatchesPage(const BookmarkEntry& bookmark, const int spineIndex, const int page,
                         const int pageCount, const float pageProgress01) {
  // Exact match when chapter page map still matches what was stored.
  if (bookmark.computedSpineIndex == spineIndex && bookmark.computedChapterPageCount == pageCount &&
      bookmark.computedChapterProgress == page) {
    return true;
  }
  // Layout may differ (classic vs Rivulet page counts): same spine + nearby %.
  if (bookmark.computedSpineIndex != spineIndex) return false;
  const float bp = std::clamp(bookmark.percentage, 0.0f, 1.0f);
  return std::fabs(bp - pageProgress01) <= kBookmarkProgressEpsilon;
}

}  // namespace

RivuletReaderActivity::RivuletReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                             std::shared_ptr<Epub> epub)
    : Activity("RivuletReader", renderer, mappedInput), epub_(std::move(epub)) {}

void RivuletReaderActivity::configureRenderKey() {
  // Shared with the Home background indexer — see ReaderRenderKey.h for why this
  // must not be reader-private (maps built under a different key are discarded).
  const readerkey::Layout layout = readerkey::compute(renderer);
  marginX_ = layout.marginL;
  marginY_ = layout.marginT;
  marginR_ = layout.marginR;
  marginB_ = layout.marginB;
  engine_.setRenderKey(layout.key);
  engine_.setLineCompression(layout.lineCompression);
  LOG_DBG("RVR", "viewport %ux%u margins LTRB=%d,%d,%d,%d", layout.key.viewportW, layout.key.viewportH, marginX_,
          marginY_, marginR_, marginB_);
}

bool RivuletReaderActivity::saveProgress() const {
  if (!epub_ || casperBookDir_.empty()) return false;
  // Allow save while chapter IR is released for a child UI (menu / fonts) —
  // heldSpine/heldPage are the source of truth then. Block only when we have
  // neither a live chapter nor a held place (avoids writing spine 0 by accident).
  if (!ready_ && !heavyReleasedForUi_) return false;
  // After releaseHeavyForUi the engine is empty — use held spine/page so menu
  // leave / sleep never rewrites progress.bin as page 0.
  const int spine = heavyReleasedForUi_ ? heldSpineForUi_ : spineIndex_;
  const int page = heavyReleasedForUi_ ? heldPageForUi_ : engine_.currentPage();
  const int pageCount =
      heavyReleasedForUi_ ? std::max(page + 1, 1) : std::max(page + 1, engine_.chapterPageCount(&renderer));
  if (spine < 0 || spine > 0xFFFF || page < 0 || page > 0xFFFF || pageCount < 0 || pageCount > 0xFFFF) {
    return false;
  }
  // Nothing moved since the last write — skip the FAT round trip. Reported as
  // success because the on-disk state already reflects this position, which is
  // what every caller actually cares about.
  if (spine == lastSavedSpine_ && page == lastSavedPage_ && pageCount == lastSavedPageCount_) {
    return true;
  }
  uint8_t data[6];
  data[0] = static_cast<uint8_t>(spine & 0xFF);
  data[1] = static_cast<uint8_t>((spine >> 8) & 0xFF);
  data[2] = static_cast<uint8_t>(page & 0xFF);
  data[3] = static_cast<uint8_t>((page >> 8) & 0xFF);
  data[4] = static_cast<uint8_t>(pageCount & 0xFF);
  data[5] = static_cast<uint8_t>((pageCount >> 8) & 0xFF);
  if (!ProgressFile::writeAtomic(casperBookDir_, data, sizeof(data))) {
    LOG_ERR("RVR", "progress save fail %s", casperBookDir_.c_str());
    return false;
  }
  lastSavedSpine_ = spine;
  lastSavedPage_ = page;
  lastSavedPageCount_ = pageCount;
  LOG_INF("RVR", "progress saved casper spine=%d page=%d dir=%s", spine, page, casperBookDir_.c_str());
  return true;
}

void RivuletReaderActivity::persistProgressForSleep() {
  // Called while still foreground — before SleepActivity tears us down. Guarantees
  // progress.bin hits SD even if onExit is skipped or fails mid-teardown.
  if (!epub_ || !ready_) return;
  APP_STATE.openEpubPath = epub_->getPath();
  // Always write on sleep — do not trust the no-op skip cache. A stale
  // lastSavedPage_ (or an estimate-only pageCount mismatch) must not leave
  // progress.bin on an older page across Quick Resume.
  lastSavedSpine_ = -1;
  lastSavedPage_ = -1;
  lastSavedPageCount_ = -1;
  (void)saveProgress();
  persistHomeProgress(/*writeToDisk=*/true);
}

void RivuletReaderActivity::loadProgress(int& outSpine, int& outPage) {
  outSpine = -1;
  outPage = 0;

  auto tryRead = [&](const std::string& dir, const char* sourceTag) -> bool {
    if (dir.empty()) return false;
    HalFile f;
    if (!Storage.openFileForRead("RVR", dir + "/progress.bin", f)) return false;
    uint8_t data[6];
    const int n = f.read(data, 6);
    f.close();
    if (n != 4 && n != 6) return false;
    outSpine = data[0] + (data[1] << 8);
    outPage = data[2] + (data[3] << 8);
    // UINT16_MAX is an in-memory "last page of prev chapter" sentinel — never resume.
    if (outPage == 0xFFFF) outPage = 0;
    LOG_INF("RVR", "Loaded progress.bin (%s): spine=%d page=%d dir=%s", sourceTag, outSpine, outPage, dir.c_str());
    return true;
  };

  // v0.1.8 layout: progress.bin lives in the same folder as package cache
  // (/.crosspoint/epub_<hash>/). casperBookDir_ is that path.
  if (tryRead(casperBookDir_, "cache")) return;
  if (epub_ && tryRead(epub_->getCachePath(), "epub-cache")) return;
}

void RivuletReaderActivity::persistHomeProgress(const bool writeToDisk) {
  if (!epub_) return;
  const float oldPct = readingStats_.getProgressPercent();
  const float newPct = bookProgress01() * 100.0f;
  if (newPct >= 0.0f) {
    readingStats_.setProgressPercent(newPct);
  }
  if (readingStats_.isCompleted) {
    readingStats_.setProgressPercent(100.0f);
  }
  const float saved = readingStats_.getProgressPercent();
  PenumbraThemeUi::updateRecentsProgressForPath(epub_->getPath().c_str(), saved);
  CasperStats::setHomeProgress(epub_->getPath(), saved);
  if (!writeToDisk || casperBookDir_.empty()) return;
  if (oldPct >= 0.0f && saved >= 0.0f && std::fabs(oldPct - saved) < 0.05f && !readingStats_.isCompleted) {
    return;
  }
  CasperStats::saveBook(epub_->getPath(), readingStats_);
  LOG_INF("RVR", "Home progress %.1f%% → CasperStats %s", static_cast<double>(saved), casperBookDir_.c_str());
}

void RivuletReaderActivity::noteForwardPageTurn() {
  if (!SETTINGS.readingStatsTrackingEnabled()) {
    lastPageTurnTime_ = millis();
    return;
  }
  const unsigned long now = millis();
  if (lastPageTurnTime_ != 0UL && now >= lastPageTurnTime_) {
    const uint32_t dwell = static_cast<uint32_t>((now - lastPageTurnTime_) / 1000UL);
    const uint32_t idleCap = SETTINGS.getReadingSessionIdleSeconds();
    // Count pace for sane dwells (2s … idle threshold).
    if (dwell >= 2 && dwell <= idleCap) {
      readingStats_.recordForwardPageRead(dwell);
      if (globalReadingStats_.totalPagesTurned < UINT32_MAX) {
        globalReadingStats_.totalPagesTurned++;
      }
    }
  }
  lastPageTurnTime_ = now;
}

void RivuletReaderActivity::jumpToPercent(const int percent) {
  if (!epub_ || epub_->getBookSize() == 0) return;
  const float target = std::clamp(percent, 0, 100) / 100.0f;
  const int n = epub_->getSpineItemsCount();
  if (n <= 0) return;

  int spine = 0;
  for (int i = 0; i < n; ++i) {
    const float end = epub_->calculateProgress(i, 1.0f);
    spine = i;
    if (end >= target) break;
  }
  const float startP = epub_->calculateProgress(spine, 0.0f);
  const float endP = epub_->calculateProgress(spine, 1.0f);
  float frac = 0.0f;
  if (endP > startP + 0.0001f) {
    frac = std::clamp((target - startP) / (endP - startP), 0.0f, 0.999f);
  }
  LOG_INF("RVR", "jumpToPercent %d%% → spine=%d frac=%.3f", percent, spine, static_cast<double>(frac));
  if (!loadSpine(spine)) {
    // Walk forward if landing spine empty.
    for (int i = spine; i < n; ++i) {
      if (loadSpine(i)) break;
    }
  }
  if (!ready_) return;
  const int pages = std::max(1, engine_.chapterPageCount(&renderer));
  const int page = std::min(pages - 1, static_cast<int>(frac * static_cast<float>(pages)));
  if (page > 0) {
    (void)engine_.goToPage(renderer, page, /*maxWalkPages=*/512);
  }
  firstPaint_ = true;
  (void)saveProgress();
  persistHomeProgress(true);
}

void RivuletReaderActivity::openBookStats() {
  if (!epub_) return;
  const float pct = readingStats_.getProgressPercent();
  const bool hasEta = readingStats_.estimatedTimeLeftSeconds > 0;
  startActivityForResult(
      std::make_unique<BookStatsActivity>(renderer, mappedInput, epub_->getTitle(), casperBookDir_, readingStats_, pct,
                                          hasEta, readingStats_.estimatedTimeLeftSeconds, globalReadingStats_),
      [this](const ActivityResult&) {
        // Reload in case user edited dates/completed.
        if (!casperBookDir_.empty()) {
          readingStats_ = BookReadingStats::load(casperBookDir_);
        }
        requestUpdate();
      });
}

// Build selectable word boxes from the laid-out page (dictionary + clippings).
static bool buildPageWordBoxes(GfxRenderer& renderer, const rivulet::LaidOutPage& page, const int marginX,
                               const int marginY, const int baseFontId,
                               std::vector<DictionaryWordSelectActivity::WordBox>& boxes,
                               std::vector<std::string>& pool) {
  boxes.clear();
  pool.clear();
  boxes.reserve(128);
  pool.reserve(128);

  // Portrait: reserve bottom hint strip. Landscape: reserve the logical side
  // where drawButtonHints paints the front-button chrome.
  const Rect safe =
      UITheme::getInstance().getScreenSafeArea(renderer, /*hasFrontButtonHints=*/true, /*hasSideButtonHints=*/false);
  const int maxWordBottom = safe.y + safe.height - 2;
  const int minWordLeft = safe.x + 2;
  const int maxWordRight = safe.x + safe.width - 2;

  uint16_t row = 0;
  int lastY = INT_MIN;
  for (const auto& sp : page.spans) {
    if ((sp.epdStyle & static_cast<uint8_t>(EpdFontFamily::DROP_CAP)) != 0) continue;
    if (sp.dropScale >= 2) continue;
    if (sp.text.empty()) continue;
    const int spanFont = sp.fontId != 0 ? sp.fontId : baseFontId;
    const int lh = std::max(1, renderer.getLineHeight(spanFont));
    const int screenY = marginY + sp.y;
    if (screenY + lh > maxWordBottom) continue;
    if (lastY != INT_MIN && sp.y != lastY) ++row;
    lastY = sp.y;

    const char* s = sp.text.c_str();
    int localX = marginX + sp.x;
    const auto style = static_cast<EpdFontFamily::Style>(sp.epdStyle & 0x03);
    while (*s) {
      while (*s == ' ' || *s == '\t') {
        localX += renderer.getSpaceWidth(spanFont, style);
        ++s;
      }
      if (!*s) break;
      const char* w0 = s;
      while (*s && *s != ' ' && *s != '\t') ++s;
      const size_t wlen = static_cast<size_t>(s - w0);
      if (wlen == 0) continue;
      bool alnum = false;
      for (size_t i = 0; i < wlen; ++i) {
        const unsigned char c = static_cast<unsigned char>(w0[i]);
        if (std::isalnum(c) || c >= 0x80) {
          alnum = true;
          break;
        }
      }
      if (!alnum) {
        std::string tmp(w0, wlen);
        localX += renderer.getTextAdvanceX(spanFont, tmp.c_str(), style);
        continue;
      }
      if (localX >= maxWordRight) {
        // Rest of this span is under landscape side chrome.
        break;
      }
      pool.emplace_back(w0, wlen);
      DictionaryWordSelectActivity::WordBox box{};
      box.x = static_cast<int16_t>(std::max(localX, minWordLeft));
      box.y = static_cast<int16_t>(screenY);
      int w = renderer.getTextAdvanceX(spanFont, pool.back().c_str(), style);
      if (box.x + w > maxWordRight) w = std::max(0, maxWordRight - box.x);
      box.width = static_cast<int16_t>(w);
      box.row = row;
      box.text = pool.back().c_str();
      box.style = style;
      boxes.push_back(box);
      localX += renderer.getTextAdvanceX(spanFont, pool.back().c_str(), style);
    }
  }
  return !boxes.empty();
}

void RivuletReaderActivity::openDictionary() {
  // Menu release path: restore chapter before building word boxes / FB snapshot.
  if (heavyReleasedForUi_ && !restoreAfterUi()) return;

  std::vector<DictionaryEntry> installed;
  DictionaryRegistry::discover(installed);
  if (installed.empty()) {
    BookActions::drawToast(renderer, tr(STR_DICT_NONE_INSTALLED));
    delay(800);
    requestUpdate();
    return;
  }
  if (!SETTINGS.anyDictionaryEnabled()) {
    std::vector<std::string> names;
    names.reserve(installed.size());
    for (const auto& e : installed) names.push_back(e.name);
    SETTINGS.setEnabledDictionaries(names);
    SETTINGS.saveToFile();
  }
  if (!engine_.ensureLaidOut(renderer)) {
    BookActions::drawToast(renderer, tr(STR_DICT_NOT_FOUND));
    delay(600);
    requestUpdate();
    return;
  }

  std::vector<DictionaryWordSelectActivity::WordBox> boxes;
  std::vector<std::string> pool;
  if (!buildPageWordBoxes(renderer, engine_.page(), marginX_, marginY_, engine_.renderKey().fontId, boxes, pool)) {
    BookActions::drawToast(renderer, tr(STR_DICT_NOT_FOUND));
    delay(600);
    requestUpdate();
    return;
  }

  // Always repaint the page into the FB before snapshot. Opening via the book
  // menu leaves the menu (or chrome) in the buffer — capturing that made the
  // dictionary/clip tool show a blank/wrong plate with "disappearing" words.
  // Capture uninverted black-on-white so highlight polarity is stable; dark mode
  // is applied at display time in DictionaryWordSelectActivity.
  {
    RenderLock lock(*this);
    renderer.clearScreen(0xFF);
    auto* fcm = renderer.getFontCacheManager();
    if (fcm) {
      auto scope = fcm->createPrewarmScope(/*clearOnEnter=*/false, /*clearOnExit=*/false);
      engine_.paint(renderer, marginX_, marginY_);
      scope.endScanAndPrewarm();
    }
    engine_.paint(renderer, marginX_, marginY_);
    paintPageImages();
  }

  const size_t fbBytes = renderer.getBufferSize();
  auto pageFb = makeUniqueNoThrow<uint8_t[]>(fbBytes);
  if (pageFb && renderer.getFrameBuffer()) {
    std::memcpy(pageFb.get(), renderer.getFrameBuffer(), fbBytes);
  } else {
    pageFb.reset();
  }

  startActivityForResult(
      std::make_unique<DictionaryWordSelectActivity>(renderer, mappedInput, std::move(boxes), std::move(pool),
                                                     engine_.renderKey().fontId, std::move(pageFb),
                                                     pageFb ? fbBytes : 0, marginX_, marginY_),
      [this](const ActivityResult&) { requestUpdate(); });
}

void RivuletReaderActivity::ensureClippingsLoaded() {
  if (!epub_ || clippingsLoaded_) return;
  (void)CLIPPINGS.loadForBook(epub_->getPath(), epub_->getTitle(), epub_->getAuthor(), "epub");
  clippingsLoaded_ = true;
}

void RivuletReaderActivity::openClippingList() {
  if (!epub_) return;
  ensureClippingsLoaded();
  startActivityForResult(std::make_unique<EpubReaderClippingListActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) {
                           // Jump result: spine + page from clipping.
                           if (!result.isCancelled) {
                             if (const auto* jump = std::get_if<ClippingJumpResult>(&result.data)) {
                               if (loadSpine(static_cast<int>(jump->spineIndex))) {
                                 (void)engine_.goToPage(renderer, static_cast<int>(jump->page), 256);
                                 firstPaint_ = true;
                               }
                             }
                           }
                           requestUpdate();
                         });
}

void RivuletReaderActivity::openClippingTool() {
  if (heavyReleasedForUi_ && !restoreAfterUi()) return;
  if (!epub_ || !engine_.ensureLaidOut(renderer)) {
    BookActions::drawToast(renderer, "Nothing to clip");
    delay(600);
    requestUpdate();
    return;
  }

  std::vector<DictionaryWordSelectActivity::WordBox> boxes;
  std::vector<std::string> pool;
  if (!buildPageWordBoxes(renderer, engine_.page(), marginX_, marginY_, engine_.renderKey().fontId, boxes, pool)) {
    BookActions::drawToast(renderer, "Nothing to clip");
    delay(600);
    requestUpdate();
    return;
  }

  // Same as dictionary: menu overwrote the FB — repaint page then snapshot.
  {
    RenderLock lock(*this);
    renderer.clearScreen(0xFF);
    auto* fcm = renderer.getFontCacheManager();
    if (fcm) {
      auto scope = fcm->createPrewarmScope(/*clearOnEnter=*/false, /*clearOnExit=*/false);
      engine_.paint(renderer, marginX_, marginY_);
      scope.endScanAndPrewarm();
    }
    engine_.paint(renderer, marginX_, marginY_);
    paintPageImages();
  }

  const size_t fbBytes = renderer.getBufferSize();
  auto pageFb = makeUniqueNoThrow<uint8_t[]>(fbBytes);
  if (pageFb && renderer.getFrameBuffer()) {
    std::memcpy(pageFb.get(), renderer.getFrameBuffer(), fbBytes);
  } else {
    pageFb.reset();
  }

  ensureClippingsLoaded();

  const std::string bookTitle = epub_->getTitle();
  const std::string author = epub_->getAuthor();
  char chapterTitle[CLIPPING_CHAPTER_TITLE_MAX] = {};
  std::snprintf(chapterTitle, sizeof(chapterTitle), "%s", bookTitle.c_str());
  const uint16_t page = static_cast<uint16_t>(std::max(0, engine_.currentPage()));
  const uint16_t spine = static_cast<uint16_t>(spineIndex_);

  startActivityForResult(
      std::make_unique<DictionaryWordSelectActivity>(
          renderer, mappedInput, std::move(boxes), std::move(pool), engine_.renderKey().fontId, std::move(pageFb),
          pageFb ? fbBytes : 0, marginX_, marginY_, DictionaryWordSelectActivity::Mode::Clip),
      [this, bookTitle, author, chapterTitle = std::string(chapterTitle), page, spine](const ActivityResult& result) {
        if (!result.isCancelled) {
          if (const auto* clip = std::get_if<ClippingResult>(&result.data)) {
            if (!clip->text.empty()) {
              std::string text = clip->text;
              if (text.size() > CLIPPING_TEXT_MAX) text.resize(CLIPPING_TEXT_MAX);
              ensureClippingsLoaded();
              const size_t clippingIndex = CLIPPINGS.clippingCount();
              const auto addResult =
                  CLIPPINGS.addClipping(spine, page, page, 1, clip->startPageWordIndex, clip->endPageWordIndex,
                                        clip->wordCount, chapterTitle.c_str(), UINT16_MAX, text);
              bool exported = false;
              if (addResult == ClippingStore::AddResult::Added) {
                exported = ClippingsManager::saveClipping(bookTitle, author, chapterTitle,
                                                          static_cast<int>(page) + 1, text);
                if (!exported && !CLIPPINGS.removeClippingAt(clippingIndex)) {
                  LOG_ERR("RVR", "Failed to roll back clipping after export failure");
                }
              }
              const bool saved = addResult == ClippingStore::AddResult::Added && exported;
              BookActions::drawToast(renderer, addResult == ClippingStore::AddResult::LimitReached
                                                   ? "Clipping limit reached"
                                               : saved ? tr(STR_CLIPPING_SAVED)
                                                       : "Could not save clipping");
              delay(900);
            }
          }
        }
        requestUpdate();
      });
}

std::string RivuletReaderActivity::currentPagePlainText(const size_t maxChars) const {
  // Stack gather — std::string += / reserve can abort() under -fno-exceptions
  // (chapter-22 crash was operator new in a growing HTML buf). QR / bookmarks
  // only need a short snippet.
  char buf[256];
  const size_t cap = std::min(maxChars, sizeof(buf) - 1);
  size_t n = 0;
  for (const auto& sp : engine_.page().spans) {
    if (sp.text.empty()) continue;
    if (n > 0 && buf[n - 1] != ' ') {
      if (n + 1 >= cap) break;
      buf[n++] = ' ';
    }
    const size_t room = (n < cap) ? (cap - n) : 0;
    if (room == 0) break;
    const size_t take = std::min(sp.text.size(), room);
    std::memcpy(buf + n, sp.text.data(), take);
    n += take;
    if (take < sp.text.size()) break;
  }
  buf[n] = '\0';
  return std::string(buf, n);
}

void RivuletReaderActivity::ensureChapterFootnotes() {
  if (!epub_ || irDir_.empty()) return;
  if (footnoteCacheSpine_ == spineIndex_) return;
  // Paint must never scan. loadSpine sets footnoteScanDeferred_ so first ink
  // after a chapter convert (maxA≈31KB on the aborting device log) cannot
  // enter this function. Idle clears the flag when heap recovers.
  if (footnoteScanDeferred_) return;

  chapterFootnotes_.clear();
  currentPageFootnotes_.clear();

  if (ESP.getMaxAllocHeap() < 12 * 1024 || ESP.getFreeHeap() < 20 * 1024) {
    LOG_ERR("RVR", "footnote scan skip low heap free=%u maxA=%u", static_cast<unsigned>(ESP.getFreeHeap()),
            static_cast<unsigned>(ESP.getMaxAllocHeap()));
    footnoteScanDeferred_ = true;
    return;
  }

  char htmlPath[192];
  std::snprintf(htmlPath, sizeof(htmlPath), "%s/s%d.html", irDir_.c_str(), spineIndex_);
  if (!Storage.exists(htmlPath)) {
    footnoteCacheSpine_ = spineIndex_;
    footnoteScanDeferred_ = false;
    return;
  }

  HalFile f;
  if (!Storage.openFileForRead("RVR", htmlPath, f)) {
    footnoteCacheSpine_ = spineIndex_;
    footnoteScanDeferred_ = false;
    return;
  }

  // Once per spine: collect real footnote-style markers only.
  // Earlier we took any short internal <a> label (≤24 chars). TOC chapter
  // titles ("One", "Cover", …) matched and got painted underlines, while longer
  // titles did not — so Contents looked half-underlined. Classic EPUB underlines
  // all links; Rivulet only fakes note underlines at paint time, so the filter
  // must stay tight to marker shapes (1, [12], *, a) with a fragment href.
  auto isFootnoteMarkerLabel = [](const char* clean, const size_t n) -> bool {
    if (!clean || n == 0 || n > 8) return false;
    // Multi-word = TOC / cross-ref prose, not a note number.
    for (size_t i = 0; i < n; ++i) {
      if (clean[i] == ' ') return false;
    }

    size_t b = 0;
    size_t e = n;
    // Strip [1] / (1) wrappers used by many note styles.
    if (e - b >= 2 && ((clean[b] == '[' && clean[e - 1] == ']') || (clean[b] == '(' && clean[e - 1] == ')'))) {
      ++b;
      --e;
    }
    if (b >= e || e - b > 6) return false;

    auto isDigit = [](unsigned char c) { return c >= '0' && c <= '9'; };
    auto isAlpha = [](unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };

    // "*", "**", …
    bool allStar = true;
    for (size_t i = b; i < e; ++i) {
      if (clean[i] != '*') {
        allStar = false;
        break;
      }
    }
    if (allStar) return true;

    // "1", "12", "256"
    bool allDigit = true;
    for (size_t i = b; i < e; ++i) {
      if (!isDigit(static_cast<unsigned char>(clean[i]))) {
        allDigit = false;
        break;
      }
    }
    if (allDigit) return true;

    // "12a" (digit run + one trailing letter)
    if (e - b >= 2 && isAlpha(static_cast<unsigned char>(clean[e - 1]))) {
      bool digitsThenLetter = true;
      for (size_t i = b; i + 1 < e; ++i) {
        if (!isDigit(static_cast<unsigned char>(clean[i]))) {
          digitsThenLetter = false;
          break;
        }
      }
      if (digitsThenLetter) return true;
    }

    // Single letter noteref: "a", "b" (not "One" / "Cover")
    if (e - b == 1 && isAlpha(static_cast<unsigned char>(clean[b]))) return true;

    return false;
  };

  constexpr size_t kMaxScan = 64 * 1024;
  constexpr size_t kMaxFn = 48;
  constexpr size_t kWin = 4096;
  auto win = makeUniqueNoThrow<char[]>(kWin);
  if (!win) {
    LOG_ERR("RVR", "footnote scan OOM window");
    footnoteScanDeferred_ = true;
    return;
  }
  if (chapterFootnotes_.capacity() < kMaxFn) {
    chapterFootnotes_.reserve(kMaxFn);
  }
  size_t winLen = 0;
  size_t total = 0;

  auto startsWith = [](const char* s, size_t n, const char* pfx) -> bool {
    const size_t pl = std::strlen(pfx);
    return n >= pl && std::memcmp(s, pfx, pl) == 0;
  };

  while (total < kMaxScan && chapterFootnotes_.size() < kMaxFn) {
    if (winLen >= kWin) {
      const size_t keep = kWin / 2;
      std::memmove(win.get(), win.get() + (winLen - keep), keep);
      winLen = keep;
    }
    const size_t room = kWin - winLen;
    const int n = f.read(win.get() + winLen, room);
    if (n <= 0) break;
    winLen += static_cast<size_t>(n);
    total += static_cast<size_t>(n);

    size_t pos = 0;
    while (pos + 3 < winLen && chapterFootnotes_.size() < kMaxFn) {
      size_t aPos = static_cast<size_t>(-1);
      for (size_t i = pos; i + 1 < winLen; ++i) {
        if (win[i] == '<' && (win[i + 1] == 'a' || win[i + 1] == 'A')) {
          aPos = i;
          break;
        }
      }
      if (aPos == static_cast<size_t>(-1)) break;

      size_t tagEnd = static_cast<size_t>(-1);
      for (size_t i = aPos; i < winLen; ++i) {
        if (win[i] == '>') {
          tagEnd = i;
          break;
        }
      }
      if (tagEnd == static_cast<size_t>(-1)) {
        if (aPos > 0) {
          std::memmove(win.get(), win.get() + aPos, winLen - aPos);
          winLen -= aPos;
        }
        break;
      }
      size_t closeA = static_cast<size_t>(-1);
      for (size_t i = tagEnd + 1; i + 2 < winLen; ++i) {
        if (win[i] == '<' && win[i + 1] == '/' && (win[i + 2] == 'a' || win[i + 2] == 'A')) {
          closeA = i;
          break;
        }
      }
      if (closeA == static_cast<size_t>(-1)) {
        if (aPos > 0) {
          std::memmove(win.get(), win.get() + aPos, winLen - aPos);
          winLen -= aPos;
        }
        break;
      }
      pos = closeA + 3;

      const char* tag = win.get() + aPos;
      const size_t tagLen = tagEnd - aPos + 1;
      const char* hrefKey = nullptr;
      for (size_t i = 0; i + 5 < tagLen; ++i) {
        if ((tag[i] == 'h' || tag[i] == 'H') && (tag[i + 1] == 'r' || tag[i + 1] == 'R') &&
            (tag[i + 2] == 'e' || tag[i + 2] == 'E') && (tag[i + 3] == 'f' || tag[i + 3] == 'F') && tag[i + 4] == '=') {
          hrefKey = tag + i + 5;
          break;
        }
      }
      if (!hrefKey) continue;
      char quote = 0;
      if (*hrefKey == '"' || *hrefKey == '\'') quote = *hrefKey++;
      const char* hrefEnd = hrefKey;
      while (hrefEnd < tag + tagLen && *hrefEnd && *hrefEnd != quote && *hrefEnd != ' ' && *hrefEnd != '\t' &&
             *hrefEnd != '>') {
        ++hrefEnd;
      }
      const size_t hrefN = static_cast<size_t>(hrefEnd - hrefKey);
      if (hrefN == 0 || hrefN >= 96) continue;
      if (startsWith(hrefKey, hrefN, "http://") || startsWith(hrefKey, hrefN, "https://") ||
          startsWith(hrefKey, hrefN, "mailto:") || startsWith(hrefKey, hrefN, "javascript:")) {
        continue;
      }
      bool hasHash = false;
      for (size_t i = 0; i < hrefN; ++i) {
        if (hrefKey[i] == '#') {
          hasHash = true;
          break;
        }
      }
      if (!hasHash) continue;

      char clean[12];
      size_t cleanN = 0;
      bool inTag = false;
      for (size_t i = tagEnd + 1; i < closeA && cleanN + 1 < sizeof(clean); ++i) {
        char c = win[i];
        if (c == '<') {
          inTag = true;
          continue;
        }
        if (c == '>') {
          inTag = false;
          continue;
        }
        if (inTag) continue;
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
        if (c == ' ' && cleanN > 0 && clean[cleanN - 1] == ' ') continue;
        clean[cleanN++] = c;
      }
      while (cleanN > 0 && clean[0] == ' ') {
        std::memmove(clean, clean + 1, --cleanN);
      }
      while (cleanN > 0 && clean[cleanN - 1] == ' ') --cleanN;
      clean[cleanN] = '\0';
      if (!isFootnoteMarkerLabel(clean, cleanN)) continue;

      bool dup = false;
      for (const auto& e : chapterFootnotes_) {
        if (e.href[0] && std::strncmp(e.href, hrefKey, hrefN) == 0 && e.href[hrefN] == '\0') {
          dup = true;
          break;
        }
      }
      if (dup) continue;

      FootnoteEntry entry{};
      std::snprintf(entry.number, sizeof(entry.number), "%s", clean);
      const size_t hrefCopy = hrefN < sizeof(entry.href) - 1 ? hrefN : sizeof(entry.href) - 1;
      std::memcpy(entry.href, hrefKey, hrefCopy);
      entry.href[hrefCopy] = '\0';
      chapterFootnotes_.push_back(entry);
    }
  }
  // Pin only after a finished attempt so a low-heap skip can retry on idle.
  footnoteCacheSpine_ = spineIndex_;
  footnoteScanDeferred_ = false;
  LOG_DBG("RVR", "chapter footnotes spine=%d n=%zu", spineIndex_, chapterFootnotes_.size());
}

void RivuletReaderActivity::refreshPageFootnotes() {
  currentPageFootnotes_.clear();
  if (!epub_ || !ready_) return;
  ensureChapterFootnotes();
  if (chapterFootnotes_.empty()) return;

  // Search live spans — do not concatenate the page into a std::string (paint
  // path, -fno-exceptions: += abort). Marker labels are short (1, [12], *).
  auto pageHasMarker = [this](const char* label) -> bool {
    if (!label || label[0] == '\0') return false;
    const size_t n = std::strlen(label);
    for (const auto& sp : engine_.page().spans) {
      if (sp.text.size() < n) continue;
      if (sp.text.find(label) != std::string::npos) return true;
    }
    return false;
  };

  constexpr size_t kMaxFn = 12;
  if (currentPageFootnotes_.capacity() < kMaxFn) {
    currentPageFootnotes_.reserve(kMaxFn);
  }
  for (const auto& e : chapterFootnotes_) {
    if (e.number[0] == '\0') continue;
    if (!pageHasMarker(e.number)) continue;
    currentPageFootnotes_.push_back(e);
    if (currentPageFootnotes_.size() >= kMaxFn) break;
  }
  LOG_DBG("RVR", "page footnotes n=%zu", currentPageFootnotes_.size());
}

void RivuletReaderActivity::paintFootnoteMarkers() {
  if (!ready_) return;
  // firstPaint_ is cleared at the top of render() before this runs, so it
  // cannot gate the scan. footnoteScanDeferred_ is the real first-ink skip.
  if (footnoteScanDeferred_) return;
  refreshPageFootnotes();
  if (currentPageFootnotes_.empty()) return;

  // legacy: footnote refs are underlined in the body. Pure paint — never changes
  // layout metrics / Book's Style spacing.
  std::vector<DictionaryWordSelectActivity::WordBox> boxes;
  std::vector<std::string> pool;
  if (!buildPageWordBoxes(renderer, engine_.page(), marginX_, marginY_, engine_.renderKey().fontId, boxes, pool)) {
    return;
  }

  auto labelsMatch = [](const char* word, const char* label) -> bool {
    if (!word || !label || label[0] == '\0') return false;
    // Exact match (common: "1", "12", "a").
    if (std::strcmp(word, label) == 0) return true;
    // Bracketed / starred variants: "[1]", "(1)", "*1", "1)".
    const size_t wl = std::strlen(word);
    const size_t ll = std::strlen(label);
    if (ll == 0 || wl < ll || wl > ll + 2) return false;
    // word is label with optional surrounding punctuation.
    size_t i = 0;
    if (word[0] == '[' || word[0] == '(' || word[0] == '*') i = 1;
    if (i + ll > wl) return false;
    if (std::strncmp(word + i, label, ll) != 0) return false;
    const size_t j = i + ll;
    if (j == wl) return true;
    if (j + 1 == wl && (word[j] == ']' || word[j] == ')' || word[j] == '.' || word[j] == ',')) return true;
    return false;
  };

  const int baseFont = engine_.renderKey().fontId;
  for (size_t bi = 0; bi < boxes.size(); ++bi) {
    const auto& b = boxes[bi];
    if (!b.text || b.width <= 0) continue;
    bool hit = false;
    for (const auto& fn : currentPageFootnotes_) {
      if (labelsMatch(b.text, fn.number)) {
        hit = true;
        break;
      }
    }
    if (!hit) continue;

    // Underline just under the glyph box (classic TextBlock: ascender + 2 from line top).
    const int asc = std::max(6, renderer.getFontAscenderSize(baseFont));
    const int y = b.y + asc + 2;
    const int x1 = b.x;
    const int x2 = b.x + std::max(2, static_cast<int>(b.width));
    renderer.drawLine(x1, y, x2, y, /*thickness=*/2, true);
  }
}

void RivuletReaderActivity::paintClippingHighlights() {
  if (!epub_ || !ready_) return;
  // First open: do not block on SD clippings load — highlights appear after idle/next paint.
  if (!clippingsLoaded_) {
    if (firstPaint_ && ReaderActivity::hasOpenHints()) return;
    ensureClippingsLoaded();
  }
  if (!CLIPPINGS.hasClippings()) return;

  std::vector<DictionaryWordSelectActivity::WordBox> boxes;
  std::vector<std::string> pool;
  if (!buildPageWordBoxes(renderer, engine_.page(), marginX_, marginY_, engine_.renderKey().fontId, boxes, pool)) {
    return;
  }
  if (boxes.empty()) return;

  const uint16_t curPage = static_cast<uint16_t>(std::max(0, engine_.currentPage()));
  const int pageCount = std::max(1, engine_.chapterPageCount(&renderer));
  const uint16_t curPageCount = static_cast<uint16_t>(std::min(pageCount, 65535));
  const uint16_t curSpine = static_cast<uint16_t>(spineIndex_);

  // Word indices to highlight (capped).
  bool hl[256] = {};
  const size_t nWords = std::min(boxes.size(), static_cast<size_t>(256));

  auto markRange = [&](uint16_t a, uint16_t b) {
    if (a > b) std::swap(a, b);
    for (uint16_t i = a; i <= b && i < nWords; ++i) hl[i] = true;
  };

  std::string clipText;
  clipText.reserve(CLIPPING_TEXT_MAX);
  CLIPPINGS.warmTextCache();

  for (const Clipping& c : CLIPPINGS.getClippings()) {
    if (c.spineIndex != curSpine) continue;

    // Fast path: same chapter pagination as when clipped.
    if (c.pageCount == curPageCount && curPage >= c.startPage && curPage <= c.endPage) {
      if (c.startPage == c.endPage && c.startPage == curPage) {
        markRange(c.startWordIndex, c.endWordIndex);
        continue;
      }
      // Multi-page clip: whole page if middle; partial ends not tracked well — mark all words.
      if (curPage > c.startPage && curPage < c.endPage) {
        for (size_t i = 0; i < nWords; ++i) hl[i] = true;
        continue;
      }
      if (curPage == c.startPage) {
        markRange(c.startWordIndex, static_cast<uint16_t>(nWords > 0 ? nWords - 1 : 0));
        continue;
      }
      if (curPage == c.endPage) {
        markRange(0, c.endWordIndex);
        continue;
      }
    }

    // Fallback: text search on this page's word sequence.
    clipText.clear();
    if (!CLIPPINGS.readClippingText(c, clipText) || clipText.empty()) continue;
    // Normalize to lowercase alphanumeric runs for fuzzy match.
    auto normalize = [](const std::string& s) {
      std::string o;
      o.reserve(s.size());
      for (unsigned char ch : s) {
        if (std::isalnum(ch)) o.push_back(static_cast<char>(std::tolower(ch)));
        else if (!o.empty() && o.back() != ' ') o.push_back(' ');
      }
      while (!o.empty() && o.back() == ' ') o.pop_back();
      return o;
    };
    const std::string needle = normalize(clipText);
    if (needle.size() < 4) continue;
    std::string hay;
    hay.reserve(512);
    std::vector<size_t> wordEndInHay;
    wordEndInHay.reserve(nWords);
    for (size_t i = 0; i < nWords; ++i) {
      if (!hay.empty()) hay.push_back(' ');
      for (unsigned char ch : pool[i]) {
        if (std::isalnum(ch)) hay.push_back(static_cast<char>(std::tolower(ch)));
      }
      wordEndInHay.push_back(hay.size());
    }
    const size_t at = hay.find(needle);
    if (at == std::string::npos) continue;
    const size_t endAt = at + needle.size();
    size_t startW = 0;
    size_t endW = nWords > 0 ? nWords - 1 : 0;
    for (size_t i = 0; i < nWords; ++i) {
      const size_t wStart = (i == 0) ? 0 : wordEndInHay[i - 1] + 1;
      if (wStart <= at && wordEndInHay[i] > at) startW = i;
      if (wStart < endAt) endW = i;
    }
    markRange(static_cast<uint16_t>(startW), static_cast<uint16_t>(endW));
  }

  for (size_t i = 0; i < nWords; ++i) {
    if (!hl[i]) continue;
    const auto& b = boxes[i];
    int w = b.width;
    // Extend to next word if also highlighted (continuous band).
    if (i + 1 < nWords && hl[i + 1] && boxes[i + 1].y == b.y) {
      const int gapEnd = boxes[i + 1].x;
      if (gapEnd > b.x + w) w = gapEnd - b.x;
    }
    if (w <= 0) continue;
    // Same pitch as body lines (Tight/Normal/Wide) so marks don't straddle rows.
    const int h = std::max(1, renderer.getLineHeight(engine_.renderKey().fontId, SETTINGS.getReaderLineCompression()));
    renderer.fillRectDither(b.x, b.y, w, h, Color::LightGray);
    renderer.drawText(engine_.renderKey().fontId, b.x, b.y, b.text, true, b.style);
  }
}

void RivuletReaderActivity::openFootnotesMenu() {
  refreshPageFootnotes();
  if (currentPageFootnotes_.empty()) {
    GUI.drawPopup(renderer, tr(STR_FOOTNOTES), BaseTheme::kPopupCenterY, true);
    delay(400);
    requestUpdate();
    return;
  }
  if (currentPageFootnotes_.size() == 1) {
    navigateToHref(currentPageFootnotes_[0].href, true);
    return;
  }
  startActivityForResult(std::make_unique<EpubReaderFootnotesActivity>(renderer, mappedInput, currentPageFootnotes_),
                         [this](const ActivityResult& result) {
                           if (!result.isCancelled) {
                             if (const auto* fn = std::get_if<FootnoteResult>(&result.data)) {
                               navigateToHref(fn->href, true);
                             }
                           }
                           requestUpdate();
                         });
}

void RivuletReaderActivity::navigateToHref(const std::string& hrefStr, const bool savePosition) {
  if (!epub_ || hrefStr.empty()) return;

  if (savePosition && ready_ && footnoteDepth_ < kMaxFootnoteDepth) {
    footnoteStack_[footnoteDepth_] = {spineIndex_, engine_.currentPage()};
    footnoteDepth_++;
  }

  std::string anchor;
  const auto hashPos = hrefStr.find('#');
  if (hashPos != std::string::npos && hashPos + 1 < hrefStr.size()) {
    anchor = hrefStr.substr(hashPos + 1);
  }
  const bool sameFile = !hrefStr.empty() && hrefStr[0] == '#';
  int targetSpine = sameFile ? spineIndex_ : epub_->resolveHrefToSpineIndex(hrefStr);
  if (targetSpine < 0) {
    LOG_DBG("RVR", "Could not resolve href: %s", hrefStr.c_str());
    if (savePosition && footnoteDepth_ > 0) footnoteDepth_--;
    GUI.drawPopup(renderer, "Link not found", BaseTheme::kPopupCenterY, true);
    delay(400);
    return;
  }

  // Rivulet v1: open target spine at start (in-chapter anchor walk not implemented).
  (void)anchor;
  if (loadSpine(targetSpine)) {
    firstPaint_ = true;
    (void)saveProgress();
    requestUpdate();
  } else {
    if (savePosition && footnoteDepth_ > 0) footnoteDepth_--;
    GUI.drawPopup(renderer, "Chapter not readable", BaseTheme::kPopupCenterY, true);
    delay(400);
  }
}

void RivuletReaderActivity::restoreFootnotePosition() {
  if (footnoteDepth_ <= 0) return;
  footnoteDepth_--;
  const SavedPos pos = footnoteStack_[footnoteDepth_];
  if (loadSpine(pos.spine)) {
    (void)engine_.goToPage(renderer, pos.page, 512);
    firstPaint_ = true;
    (void)saveProgress();
  }
  requestUpdate();
}

void RivuletReaderActivity::setBookCompleted(const bool completed) {
  if (!epub_ || readingStats_.isCompleted == completed) return;

  readingStats_.isCompleted = completed;
  if (completed && SETTINGS.readingStatsTrackingEnabled()) {
    readingStats_.setProgressPercent(100.0f);
    if (globalReadingStats_.completedBooks < UINT32_MAX) {
      globalReadingStats_.completedBooks++;
      globalReadingStats_.save();
    }
  } else if (!completed && SETTINGS.readingStatsTrackingEnabled() && globalReadingStats_.completedBooks > 0) {
    globalReadingStats_.completedBooks--;
    globalReadingStats_.save();
  }

  if (completed) {
    if (SETTINGS.removeReadBooksFromRecents) {
      RECENT_BOOKS.removeByPath(epub_->getPath());
    }
    if (SETTINGS.moveFinishedToReadFolder && !FinishedBooks::isInFinishedFolder(epub_->getPath())) {
      const std::string src = epub_->getPath();
      const int keepSpine = spineIndex_;
      const int keepPage = engine_.currentPage();
      engine_.clear();
      ready_ = false;
      const std::string moved = FinishedBooks::moveToFinished(src);
      if (!moved.empty() && moved != src) {
        epub_ = std::make_shared<Epub>(moved, CasperPaths::kPackageCacheRoot);
        epub_->load(/*buildIfMissing=*/false, /*skipLoadingCss=*/true);
        stableId_ = CasperBook::openBook(epub_->getPath(), epub_->getTitle(), epub_->getAuthor());
        casperBookDir_ = CasperBook::bookDir(stableId_);
        irDir_ = CasperBook::rivuletDir(stableId_);
        APP_STATE.openEpubPath = moved;
        APP_STATE.saveToFile();
        ImageBlock::setExtractor(this, &RivuletReaderActivity::extractEpubItem);
        if (loadSpine(keepSpine, keepPage)) {
          firstPaint_ = true;
        } else if (loadSpine(0)) {
          firstPaint_ = true;
        }
      } else if (loadSpine(keepSpine, keepPage)) {
        firstPaint_ = true;
      }
    }
  } else {
    if (SETTINGS.moveFinishedToReadFolder && FinishedBooks::isInFinishedFolder(epub_->getPath())) {
      const std::string cur = epub_->getPath();
      const int keepSpine = spineIndex_;
      const int keepPage = engine_.currentPage();
      engine_.clear();
      ready_ = false;
      const std::string restored = FinishedBooks::restoreFromFinished(cur);
      if (!restored.empty() && restored != cur) {
        epub_ = std::make_shared<Epub>(restored, CasperPaths::kPackageCacheRoot);
        epub_->load(/*buildIfMissing=*/false, /*skipLoadingCss=*/true);
        stableId_ = CasperBook::openBook(epub_->getPath(), epub_->getTitle(), epub_->getAuthor());
        casperBookDir_ = CasperBook::bookDir(stableId_);
        irDir_ = CasperBook::rivuletDir(stableId_);
        APP_STATE.openEpubPath = restored;
        APP_STATE.saveToFile();
        ImageBlock::setExtractor(this, &RivuletReaderActivity::extractEpubItem);
        if (loadSpine(keepSpine, keepPage)) {
          firstPaint_ = true;
        } else if (loadSpine(0)) {
          firstPaint_ = true;
        }
      } else if (loadSpine(keepSpine, keepPage)) {
        firstPaint_ = true;
      }
    }
    if (SETTINGS.removeReadBooksFromRecents) {
      RECENT_BOOKS.addBook(epub_->getPath(), epub_->getTitle(), epub_->getAuthor(), epub_->getThumbBmpPath());
    }
  }

  if (!casperBookDir_.empty()) readingStats_.save(casperBookDir_);
  PenumbraThemeUi::updateRecentsProgressForPath(epub_->getPath().c_str(), readingStats_.getProgressPercent());
  GUI.drawPopup(renderer, completed ? "Marked finished" : "Marked unfinished", BaseTheme::kPopupCenterY, true);
  delay(400);
  requestUpdate();
}

bool RivuletReaderActivity::launchLeaveKoSync(const bool uploadOnly) {
  if (!epub_) return false;

  const float bookPercent = bookProgress01() * 100.0f;
  const int currentPage = engine_.currentPage();
  const int totalPages = std::max(1, engine_.chapterPageCount(&renderer));
  const std::optional<uint16_t> paragraphIndex = std::nullopt;

  BookPosition localPos{};
  localPos.spineIndex = spineIndex_;
  localPos.pageNumber = currentPage;
  localPos.totalPages = totalPages;
  SavedProgressPosition localKoPos = ProgressMapper::toSavedProgress(epub_, localPos);
  const int tocIdx = epub_->getTocIndexForSpineIndex(spineIndex_);
  std::string localChapterName = (tocIdx >= 0) ? epub_->getTocItem(tocIdx).title : "";
  const std::string savedEpubPath = epub_->getPath();
  const int keepSpine = spineIndex_;

  if (!saveProgress()) {
    LOG_ERR("KOSync", "Aborting leave-sync because current progress could not be saved");
    GUI.drawPopup(renderer, "Could not save progress", BaseTheme::kPopupCenterY, true);
    delay(400);
    requestUpdate();
    return false;
  }

  LOG_DBG("KOSync", "Starting leave sync (uploadOnly=%d heap=%u percent=%.1f)", uploadOnly ? 1 : 0,
          static_cast<unsigned>(ESP.getFreeHeap()), static_cast<double>(bookPercent));

  {
    RenderLock lock(*this);
    ImageBlock::setExtractor(nullptr, nullptr);
    engine_.clear();
    ready_ = false;
    epub_.reset();
  }

  activityManager.replaceActivity(std::make_unique<KOReaderSyncActivity>(
      renderer, mappedInput, savedEpubPath, keepSpine, currentPage, totalPages, std::move(localKoPos),
      std::move(localChapterName), paragraphIndex, /*autoUploadOnly=*/uploadOnly, bookPercent,
      /*leaveToHome=*/true));
  return true;
}

bool RivuletReaderActivity::tryStartAutoKoUpload() {
  if (!epub_) return false;
  if (!KOREADER_STORE.hasCredentials()) return false;

  const KOReaderSyncBehavior behavior = KOREADER_STORE.getSyncBehavior();
  if (behavior == KOReaderSyncBehavior::OFF) return false;

  const float bookPercent = bookProgress01() * 100.0f;
  const std::string& bookPath = epub_->getPath();

  if (behavior == KOReaderSyncBehavior::ASK_EVERY_TIME) {
    startActivityForResult(
        std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_SYNC_PROGRESS), tr(STR_KOREADER_SYNC)),
        [this](const ActivityResult& res) {
          if (res.isCancelled) {
            Activity::onGoHome();
            return;
          }
          if (!launchLeaveKoSync(/*uploadOnly=*/false)) {
            Activity::onGoHome();
          }
        });
    return true;
  }

  if (behavior == KOReaderSyncBehavior::SMART) {
    return launchLeaveKoSync(/*uploadOnly=*/false);
  }

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

void RivuletReaderActivity::flushExitProgressAndStats() {
  if (!ready_ || !epub_) return;
  (void)saveProgress();
  // Home ring % always; session totals when tracking is on.
  const bool tracking = SETTINGS.readingStatsTrackingEnabled() && readingSessionStartMs_ != 0;
  persistHomeProgress(/*writeToDisk=*/!tracking);
  if (tracking) {
    const unsigned long nowMs = millis();
    uint32_t elapsedSecs =
        nowMs >= readingSessionStartMs_ ? static_cast<uint32_t>((nowMs - readingSessionStartMs_) / 1000UL) : 0u;
    const uint32_t idleCap = SETTINGS.getReadingSessionIdleSeconds();
    if (lastPageTurnTime_ != 0UL && nowMs >= lastPageTurnTime_) {
      const uint32_t tailSecs = static_cast<uint32_t>((nowMs - lastPageTurnTime_) / 1000UL);
      if (tailSecs > idleCap && elapsedSecs > tailSecs - idleCap) {
        elapsedSecs -= (tailSecs - idleCap);
      }
    }
    if (elapsedSecs >= 60 && readingStats_.sessionCount < UINT16_MAX) {
      readingStats_.sessionCount++;
      if (globalReadingStats_.totalSessions < UINT32_MAX) globalReadingStats_.totalSessions++;
    }
    if (elapsedSecs >= 10) {
      if (readingStats_.totalReadingSeconds <= UINT32_MAX - elapsedSecs) {
        readingStats_.totalReadingSeconds += elapsedSecs;
      } else {
        readingStats_.totalReadingSeconds = UINT32_MAX;
      }
      if (globalReadingStats_.totalReadingSeconds <= UINT32_MAX - elapsedSecs) {
        globalReadingStats_.totalReadingSeconds += elapsedSecs;
      } else {
        globalReadingStats_.totalReadingSeconds = UINT32_MAX;
      }
    }
    if (readingStats_.isCompleted) {
      readingStats_.setProgressPercent(100.0f);
      readingStats_.estimatedTimeLeftSeconds = 0;
    } else if (smoothedBookTimeLeftSeconds_ > 0) {
      readingStats_.estimatedTimeLeftSeconds = smoothedBookTimeLeftSeconds_;
    }
    PenumbraThemeUi::updateRecentsProgressForPath(epub_->getPath().c_str(), readingStats_.getProgressPercent());
    CasperStats::saveBook(epub_->getPath(), readingStats_);
    globalReadingStats_.save();
    LOG_INF("RVR", "exit stats+progress %.1f%% spine=%d page=%d id=%s",
            static_cast<double>(readingStats_.getProgressPercent()), spineIndex_, engine_.currentPage(),
            stableId_.c_str());
  } else {
    LOG_INF("RVR", "exit progress spine=%d page=%d id=%s", spineIndex_, engine_.currentPage(), stableId_.c_str());
  }
  readingSessionStartMs_ = 0;
}

void RivuletReaderActivity::leaveReaderToHome() {
  SystemLog::logTiming("LEAVE", "reader spine=%d page=%d ready=%d busy=%d fre=%u", spineIndex_,
                       engine_.currentPage(), ready_ ? 1 : 0, chapterNavBusy_ ? 1 : 0,
                       static_cast<unsigned>(ESP.getFreeHeap()));
  if (tryStartAutoKoUpload()) {
    return;
  }
  // Save progress while the book page stays on glass. Upper-left "Saving" with
  // reader dark polarity (reader-only inverts only for this push).
  if (!activityManager.isSleepTransition() && ready_ && epub_) {
    GUI.drawTopLeftStatus(renderer, tr(STR_STATUS_SAVING_STATS), /*refresh=*/false);
    ReaderUtils::displayWithDarkMode(renderer, HalDisplay::FAST_REFRESH);
    flushExitProgressAndStats();
    leaveExitFlushed_ = true;
  }
  // Loop already waits before PopToHome; only block if a paint is still mid-flight.
  activityManager.waitForRenderIdle();
  (void)mappedInput.wasPressed(MappedInputManager::Button::Back);
  (void)mappedInput.wasReleased(MappedInputManager::Button::Back);
  Activity::onGoHome();
}

bool RivuletReaderActivity::launchKOReaderSync(const bool leaveToHome, const bool uploadOnly) {
  if (!epub_) return false;
  if (!KOREADER_STORE.hasCredentials()) return false;
  if (KOREADER_STORE.getSyncBehavior() == KOReaderSyncBehavior::OFF) return false;

  // Leave-path variants use the dedicated leave helpers (uploadOnly / leaveToHome).
  if (leaveToHome) {
    return launchLeaveKoSync(uploadOnly);
  }

  const int currentPage = engine_.currentPage();
  const int totalPages = std::max(1, engine_.chapterPageCount(&renderer));
  const std::optional<uint16_t> paragraphIndex = std::nullopt;

  BookPosition localPos{};
  localPos.spineIndex = spineIndex_;
  localPos.pageNumber = currentPage;
  localPos.totalPages = totalPages;
  SavedProgressPosition localKoPos = ProgressMapper::toSavedProgress(epub_, localPos);
  const int tocIdx = epub_->getTocIndexForSpineIndex(spineIndex_);
  std::string localChapterName = (tocIdx >= 0) ? epub_->getTocItem(tocIdx).title : "";
  const std::string savedEpubPath = epub_->getPath();
  const int keepSpine = spineIndex_;

  if (!saveProgress()) {
    LOG_ERR("KOSync", "Aborting sync because current progress could not be saved");
    GUI.drawPopup(renderer, "Could not save progress", BaseTheme::kPopupCenterY, true);
    delay(400);
    requestUpdate();
    return true;
  }

  LOG_DBG("KOSync", "Releasing epub for sync (heap before: %u)", static_cast<unsigned>(ESP.getFreeHeap()));
  {
    RenderLock lock(*this);
    ImageBlock::setExtractor(nullptr, nullptr);
    engine_.clear();
    ready_ = false;
    epub_.reset();
  }
  LOG_DBG("KOSync", "Epub released (heap after: %u)", static_cast<unsigned>(ESP.getFreeHeap()));

  activityManager.replaceActivity(std::make_unique<KOReaderSyncActivity>(
      renderer, mappedInput, savedEpubPath, keepSpine, currentPage, totalPages, std::move(localKoPos),
      std::move(localChapterName), paragraphIndex));
  return true;
}

void RivuletReaderActivity::takeReaderScreenshot() {
  // Capture after the page is painted (pending flag from menu/shortcut).
  pendingScreenshot_ = true;
  firstPaint_ = false;
  requestUpdate();
}

float RivuletReaderActivity::bookProgress01() const {
  if (!epub_) return 0.0f;
  const int spine = heavyReleasedForUi_ ? heldSpineForUi_ : spineIndex_;
  const int page = heavyReleasedForUi_ ? heldPageForUi_ : engine_.currentPage();
  const int chapterPages =
      heavyReleasedForUi_ ? std::max(1, page + 1) : std::max(1, engine_.chapterPageCount(&renderer));
  const float chapterFrac =
      std::clamp(static_cast<float>(page) / static_cast<float>(chapterPages), 0.0f, 1.0f);
  return std::clamp(epub_->calculateProgress(spine, chapterFrac), 0.0f, 1.0f);
}

void RivuletReaderActivity::showError(const char* msg) {
  error_ = true;
  ready_ = false;
  errorMsg_ = msg ? msg : "Error";
  LOG_ERR("RVR", "%s", errorMsg_.c_str());
  requestUpdate();
}

bool RivuletReaderActivity::canRetainGlyphCache() {
  // Match classic: retain page glyph buffers only when the next page / UI still
  // has room. Below this, free them so menus and chapter convert are not starved.
  constexpr size_t kRetainFreeHeap = 40 * 1024;
  constexpr size_t kRetainMaxAlloc = 20 * 1024;
  return ESP.getFreeHeap() > kRetainFreeHeap && ESP.getMaxAllocHeap() > kRetainMaxAlloc;
}

void RivuletReaderActivity::prepareHeapForChapterLoad(const bool aggressive) {
  // Match classic open: drop retained chapter + image decode cache before a new convert.
  // Goal (issue #8): free contiguous heap *before* HTML inflate / IR / first layout so
  // vector growth never hits -fno-exceptions abort(). Soft-fail only if still tight.
  engine_.clear();
  ImageBlock::releaseRenderCache();
  ImageBlock::clearSessionRenderFailures();
  chapterFootnotes_.clear();
  currentPageFootnotes_.clear();
  footnoteCacheSpine_ = -1;
  footnoteScanDeferred_ = true;
  glyphCacheSpine_ = -1;
  glyphCachePage_ = -1;
  // PNGdec ~50 KB held across convert is the main maxAlloc killer on X4 (no PSRAM).
  PngToFramebufferConverter::releaseWarmIfHeapTight(/*minMaxAllocBytes=*/48 * 1024);
  // Font glyph cache fragments maxAlloc. Always clear when aggressive (prev-chapter
  // last page needs a full HTML→IR convert) or when contiguous heap is already tight.
  if (FontCacheManager* fcm = renderer.getFontCacheManager()) {
    if (!fcm->isScanning() && (aggressive || ESP.getMaxAllocHeap() < 48 * 1024)) {
      fcm->clearCache();
      LOG_INF("RVR", "prepareHeap: cleared font cache maxA=%u aggressive=%d",
              static_cast<unsigned>(ESP.getMaxAllocHeap()), aggressive ? 1 : 0);
    }
  }
  // Second pass: if still fragmented after PNG/font free, force warmer release.
  if (ESP.getMaxAllocHeap() < 32 * 1024) {
    PngToFramebufferConverter::releaseWarmIfHeapTight(/*minMaxAllocBytes=*/96 * 1024);
  }
  yield();
  delay(aggressive ? 20 : 0);
  yield();
  LOG_INF("RVR", "prepareHeap free=%u maxA=%u", static_cast<unsigned>(ESP.getFreeHeap()),
          static_cast<unsigned>(ESP.getMaxAllocHeap()));
}

void RivuletReaderActivity::releaseHeavyForUi() {
  // Policy: before book menu / heavy settings, pin progress to SD and drop the
  // chapter IR + caches so Settings/Text/UI have contiguous heap. Keep epub_
  // package + spine/page numbers so restore is a short Loading + loadSpine, not
  // a cold open. Users never lose place; they may see a brief Loading on return.
  if (heavyReleasedForUi_) return;
  if (!epub_ || !ready_) return;

  heldSpineForUi_ = spineIndex_;
  // Prefer live page; if the engine was already cleared, reuse last saved place.
  if (engine_.hasChapter()) {
    heldPageForUi_ = engine_.currentPage();
  } else if (lastSavedSpine_ >= 0 && lastSavedPage_ >= 0) {
    heldSpineForUi_ = lastSavedSpine_;
    heldPageForUi_ = lastSavedPage_;
  } else {
    heldPageForUi_ = 0;
  }
  (void)saveProgress();
  persistPageMapIfComplete();
  persistHomeProgress(/*writeToDisk=*/true);

  const uint32_t freeBefore = ESP.getFreeHeap();
  const uint32_t maxBefore = ESP.getMaxAllocHeap();
  {
    RenderLock lock(*this);
    prepareHeapForChapterLoad(/*aggressive=*/true);
  }
  heavyReleasedForUi_ = true;
  LOG_INF("RVR", "releaseHeavyForUi spine=%d page=%d free %u→%u maxA %u→%u", heldSpineForUi_, heldPageForUi_,
          static_cast<unsigned>(freeBefore), static_cast<unsigned>(ESP.getFreeHeap()),
          static_cast<unsigned>(maxBefore), static_cast<unsigned>(ESP.getMaxAllocHeap()));
}

bool RivuletReaderActivity::restoreAfterUi(const bool showLoading) {
  if (!heavyReleasedForUi_) return true;
  if (!epub_) {
    heavyReleasedForUi_ = false;
    return false;
  }

  // A child activity had the foreground and may have touched SD (cache clear,
  // sync, font install). Drop the saveProgress() skip-cache so the next save
  // definitely writes rather than trusting state from before the excursion.
  // Costs at most one extra write per child-UI return.
  lastSavedSpine_ = -1;
  lastSavedPage_ = -1;
  lastSavedPageCount_ = -1;

  const int spine = heldSpineForUi_;
  const int page = heldPageForUi_;
  LOG_INF("RVR", "restoreAfterUi spine=%d page=%d free=%u maxA=%u", spine, page,
          static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));

  // Status must use the orientation still on glass (what the user is looking at).
  // Callers that applyOrientation first must show Loading themselves, then pass false.
  if (showLoading) {
    GUI.drawTopLeftStatus(renderer, tr(STR_LOADING_POPUP), /*refresh=*/true);
  }

  ImageBlock::setExtractor(this, &RivuletReaderActivity::extractEpubItem);
  configureRenderKey();

  // Fonts / Reader UI often leave the heap fragmented — scrub before reload so
  // goToPage(held) can rebuild the map instead of silently landing on page 0.
  prepareHeapForChapterLoad(/*aggressive=*/true);

  bool ok = false;
  {
    RenderLock lock(*this);
    ok = loadSpine(spine, page);
    if (!ok) {
      prepareHeapForChapterLoad(/*aggressive=*/true);
      ok = loadSpine(spine, page);
    }
    if (!ok) ok = loadSpine(spine, 0);
    // Never scan other spines here. Falling through to spine 0 is exactly what
    // sent readers to book page 1 after Manage Fonts / Reader UI (logs: ERS open
    // spine=0 after a child activity). CrossInk keeps the held place.
  }

  // If we opened the right chapter but landed far below the held page, retry once.
  if (ok && page > 2 && engine_.currentPage() < page / 2) {
    LOG_ERR("RVR", "restoreAfterUi shallow land spine=%d want=%d got=%d — retry", spine, page,
            engine_.currentPage());
    prepareHeapForChapterLoad(/*aggressive=*/true);
    RenderLock lock(*this);
    if (loadSpine(spine, page)) {
      ok = true;
    }
  }

  if (!ok) {
    // Keep the held place on SD so the next open / QR does not resume at 0.
    heavyReleasedForUi_ = true;  // saveProgress must read heldSpine/heldPage
    lastSavedSpine_ = -1;
    lastSavedPage_ = -1;
    lastSavedPageCount_ = -1;
    (void)saveProgress();
    heavyReleasedForUi_ = false;
    showError("Could not restore page");
    LOG_ERR("RVR", "restoreAfterUi failed spine=%d page=%d — progress pinned", spine, page);
    return false;
  }

  heavyReleasedForUi_ = false;
  firstPaint_ = true;
  // Re-pin progress to the actual landed page (may be closest-reachable).
  lastSavedSpine_ = -1;
  lastSavedPage_ = -1;
  lastSavedPageCount_ = -1;
  (void)saveProgress();
  LOG_INF("RVR", "restoreAfterUi ok spine=%d page=%d free=%u", spineIndex_, engine_.currentPage(),
          static_cast<unsigned>(ESP.getFreeHeap()));
  return true;
}

bool RivuletReaderActivity::loadTocChapter(const int tocSpineIndex, const int startPage) {
  if (!epub_) return false;
  const int n = epub_->getSpineItemsCount();
  if (tocSpineIndex < 0 || tocSpineIndex >= n) return false;

  // Same model as every normal open: free prior chapter, load target, show
  // page 0 (or startPage). Idle tickIdlePageMap builds the rest. No full-chapter
  // walk, no jumping to later TOC entries.
  const int keepSpine = spineIndex_;
  const int keepPage = engine_.currentPage();
  const bool canRestore = ready_ && engine_.hasChapter();

  prepareHeapForChapterLoad();
  // Always open at the start of the chapter when picking from TOC (legacy).
  (void)startPage;
  if (loadSpine(tocSpineIndex, /*startPage=*/0)) {
    LOG_INF("RVR", "loadTocChapter ok spine=%d page0 free=%u", tocSpineIndex,
            static_cast<unsigned>(ESP.getFreeHeap()));
    return true;
  }

  // One retry after a harder heap scrub (font cache if still tight).
  prepareHeapForChapterLoad();
  if (FontCacheManager* fcm = renderer.getFontCacheManager()) {
    if (!fcm->isScanning() && ESP.getMaxAllocHeap() < 48 * 1024) fcm->clearCache();
  }
  yield();
  if (loadSpine(tocSpineIndex, 0)) return true;

  LOG_ERR("RVR", "loadTocChapter failed spine=%d — restore %d", tocSpineIndex, keepSpine);
  if (canRestore && keepSpine >= 0 && keepSpine < n) {
    prepareHeapForChapterLoad();
    if (loadSpine(keepSpine, keepPage) || loadSpine(keepSpine, 0)) return false;
  }
  ready_ = false;
  return false;
}

bool RivuletReaderActivity::loadSpine(const int spineIndex, const int startPage, const bool requireCompleteIr) {
  if (!epub_) return false;

  // Chapter IR acquisition lives in ChapterLoader so the Home screen can index
  // chapters with the same pipeline (see chapterload::loadChapterIr).
  chapterload::Request req;
  req.epub = epub_.get();
  req.engine = &engine_;
  req.renderer = &renderer;
  req.irDir = irDir_;
  req.spineIndex = spineIndex;
  req.imageRendering = SETTINGS.imageRendering;
  req.requireCompleteIr = requireCompleteIr;
  req.bindPageCache = true;

  chapterload::Hooks hooks;
  hooks.ctx = this;
  hooks.prepareHeap = [](void* ctx, const bool aggressive) {
    static_cast<RivuletReaderActivity*>(ctx)->prepareHeapForChapterLoad(aggressive);
  };
  hooks.prepareImages = [](void* ctx, const char* href) {
    static_cast<RivuletReaderActivity*>(ctx)->prepareChapterImages(href ? href : "");
  };

  const chapterload::Result loaded = chapterload::loadChapterIr(req, hooks);
  if (!loaded.ok) {
    ready_ = false;
    return false;
  }
  const bool fromIrCache = loaded.fromCache;
  // Defer spineIndex_ = spineIndex until layout succeeds (see below). A failed
  // chapter-skip used to leave spineIndex_ on the new chapter with ready_=false
  // → blank Loading, then recovery at book start.
  error_ = false;
  firstPaint_ = true;

  // Free convert scratch before first layout (HTML buffer already reset; PNG/font
  // may have grown during prepareChapterImages). Soft-fail layout if still tight.
  PngToFramebufferConverter::releaseWarmIfHeapTight(24 * 1024);
  if (ESP.getMaxAllocHeap() < 16 * 1024) {
    if (FontCacheManager* fcm = renderer.getFontCacheManager()) {
      if (!fcm->isScanning()) fcm->clearCache();
    }
    PngToFramebufferConverter::releaseWarmIfHeapTight(32 * 1024);
    LOG_INF("RVR", "pre-layout scrub free=%u maxA=%u", static_cast<unsigned>(ESP.getFreeHeap()),
            static_cast<unsigned>(ESP.getMaxAllocHeap()));
  }

  const uint32_t tLayout = millis();
  // startPage < 0: load IR (+ optional .rvpm) only — used by PageBack so we can
  // goToLastPageNearEnd without first painting page 0 of the previous spine.
  if (startPage < 0) {
    spineIndex_ = spineIndex;
    ready_ = true;
    LOG_INF("RVR", "spine=%d ir-only text=%u blocks=%u map=%d free=%u ir=%d", spineIndex,
            static_cast<unsigned>(engine_.chapter().textSize()),
            static_cast<unsigned>(engine_.chapter().blockCount()), engine_.mapKnownPages(),
            static_cast<unsigned>(ESP.getFreeHeap()), fromIrCache ? 1 : 0);
    if (footnoteCacheSpine_ != spineIndex) {
      footnoteCacheSpine_ = -1;
      footnoteScanDeferred_ = true;
      chapterFootnotes_.clear();
      currentPageFootnotes_.clear();
    }
    return true;
  }

  // First page only (or resume page). Map extension is idle — same as every open.
  // IMPORTANT: never silently fall back to page 0 when the user had a deep saved
  // page (QR resume landed on ch6 p1 after saving p31 — goToPage failed on a
  // stale/short .rvpm and the old code called goToStart).
  const int wantPage = std::max(0, startPage);
  bool laid = false;
  if (wantPage > 0) {
    // Budget must cover the resume depth — a 64-page cap was only barely enough
    // for mid-chapter resumes and failed when the map had to be rebuilt.
    const int budget = std::max(96, wantPage + 64);
    laid = engine_.goToPage(renderer, wantPage, budget);
    if (!laid) {
      LOG_ERR("RVR", "spine %d goToPage(%d) failed (known=%d) — invalidate map and retry", spineIndex, wantPage,
              engine_.mapKnownPages());
      engine_.invalidatePageMap();
      laid = engine_.goToPage(renderer, wantPage, budget);
    }
    if (!laid) {
      // Land as close as we got — never wipe the reader back to page 0 of the
      // chapter when they were dozens of pages in.
      const int closest = std::max(0, engine_.mapKnownPages() - 1);
      LOG_ERR("RVR", "spine %d resume %d unreachable — landing closest page=%d", spineIndex, wantPage, closest);
      if (closest > 0) {
        laid = engine_.goToPage(renderer, closest, budget);
      }
    }
    if (!laid) {
      LOG_ERR("RVR", "spine %d goToPage(%d) failed after retry — goToStart last resort", spineIndex, wantPage);
    }
  }
  if (!laid) {
    // Page 0 + one ahead warm (goToStart). Rest of map: tickIdlePageMap.
    laid = engine_.goToStart(renderer);
  }
  if (!laid) {
    // Empty chapter (cover image only etc.) — try next spine.
    // Do NOT leave spineIndex_/ready_ half-updated: caller may be chapter-skip.
    LOG_DBG("RVR", "spine %d empty layout — skip (%lums)", spineIndex, static_cast<unsigned long>(millis() - tLayout));
    ready_ = false;
    return false;
  }
  // Only commit the spine index after layout succeeded — a failed loadSpine
  // used to leave spineIndex_ on the new chapter with ready_=false (blank
  // Loading forever), then a later recovery opened spine 0.
  if (spineIndex != spineIndex_) lastNextSpineWarmFrom_ = -1;
  spineIndex_ = spineIndex;
  ready_ = true;

  LOG_INF("RVR", "spine=%d ready page=%d pages~%d spans=%u font=%d layoutMs=%lu free=%u ir=%d", spineIndex,
          engine_.currentPage(), engine_.chapterPageCount(nullptr),
          static_cast<unsigned>(engine_.page().spans.size()), engine_.renderKey().fontId,
          static_cast<unsigned long>(millis() - tLayout), static_cast<unsigned>(ESP.getFreeHeap()),
          fromIrCache ? 1 : 0);
  updateBookmarkFlag();
  // New spine → drop footnote cache. Scan is idle-only (first paint of a
  // just-converted chapter is the abort() site: std::string append at ~31KB maxA).
  if (footnoteCacheSpine_ != spineIndex) {
    footnoteCacheSpine_ = -1;
    footnoteScanDeferred_ = true;
    chapterFootnotes_.clear();
    currentPageFootnotes_.clear();
  }

  // Index a small window around the land page so the first few turns are cheap.
  // NOTHING heavy may run here: this is on the path to first ink. Building the
  // previous spine's map inline pushed book open past 30 s (logs: repeated
  // `activity_slow 9017ms/13184ms` after book.bin=MISS with no FIRST_INK at all,
  // i.e. a white screen with Loading). All adjacent-chapter indexing is deferred
  // to tickIdlePageMap, which only runs after the page is on glass.
  if (!warmingAdjacent_) {
    warmOpenNavigationWindow();
  }
  return true;
}

void RivuletReaderActivity::warmOpenNavigationWindow() {
  // Deliberately does nothing on the open path.
  //
  // This used to run ensureMapAhead(10) — ten measure-layout passes — plus an
  // ahead/behind paint, all before first ink. That is dead weight between the
  // button press and the page appearing, and it runs again on every chapter
  // change. tickIdlePageMap builds exactly the same window starting ~80ms after
  // the page is on glass, where the user never feels it.
}

bool RivuletReaderActivity::spineHasPageMap(const int spine) const {
  if (irDir_.empty() || spine < 0) return false;
  char mapPath[200];
  std::snprintf(mapPath, sizeof(mapPath), "%s/s%d_m%u.rvpm", irDir_.c_str(), spine,
                static_cast<unsigned>(SETTINGS.imageRendering));
  return Storage.exists(mapPath);
}

// Nearest readable spine that still has no page map, searched outward from the
// current one. Backward first: that is the direction PageBack needs.
int RivuletReaderActivity::nearestSpineWithoutMap(const bool preferBackward, const bool adjacentOnly) const {
  if (!epub_ || irDir_.empty()) return -1;
  const int n = epub_->getSpineItemsCount();
  const int maxDelta = adjacentOnly ? 1 : n;
  for (int delta = 1; delta <= maxDelta; ++delta) {
    const int first = preferBackward ? spineIndex_ - delta : spineIndex_ + delta;
    const int second = preferBackward ? spineIndex_ + delta : spineIndex_ - delta;
    for (const int cand : {first, second}) {
      if (cand < 0 || cand >= n || cand == spineIndex_) continue;
      if (epub_->getSpineItem(cand).href.empty()) continue;
      if (spineHasPageMap(cand)) continue;
      return cand;
    }
  }
  return -1;
}

// Build + persist one spine's full page map, then return to where the reader was.
// This is the Rivulet equivalent of CrossInk writing section.bin: pay once, and
// every later PageBack / chapter jump into that spine is an index lookup.
bool RivuletReaderActivity::warmNextSpinePrefix(const int pages) {
  if (warmingAdjacent_ || !ready_ || !epub_ || pages < 1 || irDir_.empty()) return false;
  const int n = epub_->getSpineItemsCount();
  int next = spineIndex_ + 1;
  while (next < n && epub_->getSpineItem(next).href.empty()) ++next;
  if (next >= n) {
    SystemLog::logTiming("MAP", "next_warm none after spine=%d", spineIndex_);
    return false;
  }
  if (spineHasPageMap(next)) {
    SystemLog::logTiming("MAP", "next_warm spine=%d already_has_map", next);
    return false;
  }

  const int curSpine = spineIndex_;
  const int curPage = engine_.currentPage();
  persistPageMapIfComplete();

  warmingAdjacent_ = true;
  const uint32_t t0 = millis();
  prepareHeapForChapterLoad(/*aggressive=*/true);
  bool ok = loadSpine(next, /*startPage=*/-1, /*requireCompleteIr=*/false);
  if (!ok || engine_.chapter().failed()) {
    prepareHeapForChapterLoad(/*aggressive=*/true);
    ok = loadSpine(next, -1, /*requireCompleteIr=*/true);
  }

  int known = 0;
  if (ok && !engine_.chapter().failed()) {
    (void)engine_.extendPageMap(renderer, pages);
    known = engine_.mapKnownPages();
    if (known > 0) {
      char mapPath[200];
      std::snprintf(mapPath, sizeof(mapPath), "%s/s%d_m%u.rvpm", irDir_.c_str(), next,
                    static_cast<unsigned>(SETTINGS.imageRendering));
      (void)engine_.savePageMap(mapPath);
    }
    SystemLog::logTiming("MAP", "next_warm spine=%d known=%d complete=%d ms=%lu fre=%u", next, known,
                         engine_.mapComplete() ? 1 : 0, static_cast<unsigned long>(millis() - t0),
                         static_cast<unsigned>(ESP.getFreeHeap()));
  } else {
    SystemLog::logTiming("MAP", "next_warm spine=%d load_fail", next);
  }

  prepareHeapForChapterLoad(/*aggressive=*/true);
  if (!loadSpine(curSpine, curPage)) {
    (void)loadSpine(curSpine, 0);
  }
  warmingAdjacent_ = false;
  return known > 0;
}

bool RivuletReaderActivity::indexSpinePageMap(const int spine) {
  if (warmingAdjacent_ || !ready_ || !epub_ || spine < 0) return false;
  if (spine == spineIndex_ || spineHasPageMap(spine)) return false;

  warmingAdjacent_ = true;
  chapterNavBusy_ = true;
  const int curSpine = spineIndex_;
  const int curPage = engine_.currentPage();
  GUI.drawTopLeftStatus(renderer, tr(STR_LOADING_POPUP), /*refresh=*/true);
  const uint32_t t0 = millis();

  // Bank the reader's own map before the eviction below throws it away.
  // prepareHeapForChapterLoad() calls engine_.clear(), so without this a map the
  // idle tick had just finished for the chapter the user is actually reading is
  // discarded and has to be rebuilt after the swap.
  persistPageMapIfComplete();

  prepareHeapForChapterLoad(/*aggressive=*/true);
  bool ok = loadSpine(spine, /*startPage=*/-1, /*requireCompleteIr=*/false);
  if (!ok || engine_.chapter().failed()) {
    prepareHeapForChapterLoad(/*aggressive=*/true);
    ok = loadSpine(spine, -1, /*requireCompleteIr=*/true);
  }
  bool indexed = false;
  if (ok && !engine_.chapter().failed()) {
    // buildPageMap is the pure measure-only walk (no spans emitted per page), so
    // it is the cheapest way to get an exact page count + every page start.
    const bool built = engine_.buildPageMap(renderer) && engine_.mapComplete();
    if (!built) {
      // Fall back to the end-seeking walk, which also seals the map.
      (void)engine_.goToLastPage(renderer, /*maxWalkPages=*/2048);
    }
    if (engine_.mapComplete()) {
      pageMapDirty_ = true;
      persistPageMapIfComplete();
      indexed = spineHasPageMap(spine);
    }
    SystemLog::logTiming("INDEX", "spine=%d ok=%d pages=%d ms=%lu fre=%u", spine, indexed ? 1 : 0,
                         engine_.mapKnownPages(), static_cast<unsigned long>(millis() - t0),
                         static_cast<unsigned>(ESP.getFreeHeap()));
    LOG_INF("RVR", "indexed spine=%d pages=%d ok=%d ms=%lu", spine, engine_.mapKnownPages(), indexed ? 1 : 0,
            static_cast<unsigned long>(millis() - t0));
  } else {
    SystemLog::logTiming("INDEX", "spine=%d load failed partial=%d", spine, engine_.chapter().failed() ? 1 : 0);
  }

  prepareHeapForChapterLoad(/*aggressive=*/true);
  if (!loadSpine(curSpine, curPage)) {
    (void)loadSpine(curSpine, 0);
  }
  chapterNavBusy_ = false;
  warmingAdjacent_ = false;
  // The restore ran with the guard set, so it skipped its own window warm.
  warmOpenNavigationWindow();
  return indexed;
}

// Whole-book background indexing is DISABLED.
//
// It swapped the resident chapter out from under the reader: each pass loads
// another spine, walks it, then reloads the current one. A pass costs seconds
// (a cold convert measured ~4s), the loop is blocking, and it re-armed every
// 1.5s — so once the reader sat still for 6s the device spent most of its time
// indexing and page turns stopped painting. It also could not terminate: a spine
// whose IR converts partial never gets a saved map, so nearestSpineWithoutMap
// kept handing back the same spine forever.
//
// Only the adjacent (previous) chapter is warmed now, from tickIdlePageMap,
// because that is the one PageBack actually needs. Whole-book indexing needs to
// happen without evicting the reader's chapter before it can come back.
void RivuletReaderActivity::tickBackgroundIndexer() {}

// Images are deliberately absent: they are already 1-bit dithered plates baked
// into the BW frame that storeBwBuffer snapshots, and re-decoding them under
// that ~48 KB hold is what aborted image-heavy pages (Fourth Wing's dragon /
// ornament pages at free~40KB maxA~24KB → operator new abort).
void RivuletReaderActivity::paintTextLayerForAa() {
  engine_.paint(renderer, marginX_, marginY_);
  paintClippingHighlights();
  paintFootnoteMarkers();
}

void RivuletReaderActivity::scheduleAaCatchUp() {
  // The try counter is deliberately NOT reset here: it is what bounds a page
  // whose heap never recovers, and the render that schedules the next attempt is
  // the same render that failed the last one. It resets when AA actually runs,
  // or when we move to a different page (which gets its own budget).
  const int spine = spineIndex_;
  const int page = engine_.currentPage();
  if (spine != aaCatchUpSpine_ || page != aaCatchUpPage_) {
    aaCatchUpSpine_ = spine;
    aaCatchUpPage_ = page;
    aaCatchUpTries_ = 0;
  }
  if (aaCatchUpTries_ >= kAaCatchUpMaxTries) return;
  aaCatchUpPending_ = true;
  aaCatchUpAtMs_ = millis() + kAaCatchUpDelayMs;
}

// Ask for a repaint that is allowed to run AA. Deliberately does NOT paint here.
//
// The first version did the greyscale passes inline, and it never once fired on
// device — no AACU line of any kind in a full capture. Two reasons it could not
// work: loop() runs on the main task while render() runs on the render task, so
// painting from here is the wrong task for the job; and it guarded itself with
// eight early returns, seven of them silent, so there was no way to see which
// one was eating it. Requesting a render instead removes both problems — the
// paint happens where every other paint happens, under the lock the render task
// already holds, and the outcome shows up on the normal PAGE line as why=c.
void RivuletReaderActivity::tickAaCatchUp() {
  if (!aaCatchUpPending_) return;

  // Settings can change between scheduling and firing.
  if (SETTINGS.textAntiAliasing == 0 || ReaderUtils::readerDarkModeEnabled()) {
    aaCatchUpPending_ = false;
    return;
  }
  if (!ready_ || chapterNavBusy_ || warmingAdjacent_) return;
  // A held control means a turn is coming; that repaint supersedes this one.
  if (ReaderUtils::anyPageTurnControlHeld(mappedInput) ||
      mappedInput.isPressed(MappedInputManager::Button::Confirm) ||
      mappedInput.isPressed(MappedInputManager::Button::Back)) {
    return;
  }
  if (static_cast<long>(millis() - aaCatchUpAtMs_) < 0) return;

  aaCatchUpPending_ = false;
  ++aaCatchUpTries_;
  forceAaThisRender_ = true;
  SystemLog::logTiming("AACU", "request spine=%d page=%d try=%u fre=%u maxA=%u", spineIndex_,
                       engine_.currentPage() + 1, static_cast<unsigned>(aaCatchUpTries_),
                       static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
  requestUpdate();
}

void RivuletReaderActivity::persistPageMapIfComplete() {
  // Never persist a map built on partial OOM IR — that froze "last page" mid-chapter
  // (e.g. DCC Ch1 ending on the T'Ghee totem line instead of the saferoom).
  if (!pageMapDirty_) return;
  if (!engine_.mapComplete() || irDir_.empty() || engine_.chapter().failed()) return;
  char mapPath[200];
  std::snprintf(mapPath, sizeof(mapPath), "%s/s%d_m%u.rvpm", irDir_.c_str(), spineIndex_,
                static_cast<unsigned>(SETTINGS.imageRendering));
  if (engine_.savePageMap(mapPath)) {
    pageMapDirty_ = false;
    LOG_DBG("RVR", "saved page map %s pages=%d", mapPath, engine_.mapKnownPages());
  }
}

void RivuletReaderActivity::tickIdlePageMap() {
  if (!ready_ || !epub_ || !firstInkDone_) return;
  if (ReaderUtils::anyPageTurnControlHeld(mappedInput) ||
      mappedInput.isPressed(MappedInputManager::Button::Confirm) ||
      mappedInput.isPressed(MappedInputManager::Button::Back)) {
    return;
  }
  const unsigned long now = millis();
  if (ESP.getMaxAllocHeap() < 20 * 1024 || ESP.getFreeHeap() < 28 * 1024) return;

  static unsigned long s_lastMapWorkMs = 0;
  static int s_lastLoggedKnown = -1;
  static int s_lastLoggedSpine = -1;
  static unsigned long s_lastMapLogMs = 0;
  static int s_stallTicks = 0;
  static int s_idleGaveUpSpine = -1;

  // Turn speed first: warm next page in RAM ASAP (no SD). Only 50ms after a turn.
  // 0029 disabled this and every forward turn paid a full layout — felt unusable.
  // If ahead is still cold, do NOT fall through to extendPageMap: a 1.5s measure
  // walk of the map tail (device log known=3→4) steals the next tap and the
  // user still pays a full paint layout on the turn.
  if (!engine_.aheadWarm()) {
    const bool atChapterEnd = engine_.hasChapter() && engine_.page().atChapterEnd;
    if (!atChapterEnd) {
      if (lastPageTurnTime_ == 0UL || (now - lastPageTurnTime_) >= 50UL) {
        if (engine_.warmAheadPage(renderer)) {
          if (now - s_lastMapLogMs >= 5000UL) {
            s_lastMapLogMs = now;
            SystemLog::logTiming("MAP", "warm_ahead spine=%d page=%d", spineIndex_, engine_.currentPage() + 1);
          }
        }
      }
      return;
    }
  }

  // Behind warm after a chapter land (PageBack onto last page). Not on the
  // turn path — that added SD/layout onto the 3.5s Back the user already felt.
  if (!engine_.behindWarm() && engine_.currentPage() > 0 &&
      (lastPageTurnTime_ == 0UL || (now - lastPageTurnTime_) >= 50UL)) {
    if (engine_.warmBehindPage(renderer)) return;
  }

  // After first ink + ahead/behind warm: one HTML footnote scan. Wait 2s so
  // this never overlaps the chapter-land refresh (device: scan+SD during the
  // 3.2s PageBack ink, then no exit).
  if (footnoteScanDeferred_ && lastPageTurnTime_ != 0UL && (now - lastPageTurnTime_) >= 2000UL &&
      ESP.getMaxAllocHeap() >= 12 * 1024 && ESP.getFreeHeap() >= 20 * 1024) {
    footnoteScanDeferred_ = false;
    ensureChapterFootnotes();
    return;
  }

  // Map seal ("~"): slower background. Quiet 2s after turn; 1.5s after each bite.
  if (lastPageTurnTime_ != 0UL && (now - lastPageTurnTime_) < 2000UL) return;
  if (s_lastMapWorkMs != 0 && (now - s_lastMapWorkMs) < 1500UL) return;
  if (engine_.mapComplete() || s_idleGaveUpSpine == spineIndex_) return;

  const int knownBefore = engine_.mapKnownPages();
  const int est = engine_.chapterPageCount(&renderer);
  const bool progressed = engine_.extendPageMap(renderer, /*maxPages=*/1);
  s_lastMapWorkMs = millis();
  const int knownAfter = engine_.mapKnownPages();
  if (progressed) {
    pageMapDirty_ = true;
    s_stallTicks = 0;
  } else {
    ++s_stallTicks;
    if (s_stallTicks >= 3 && knownAfter > 0) {
      if (engine_.tryCompleteMapAtEnd(renderer)) {
        pageMapDirty_ = true;
        s_stallTicks = 0;
        SystemLog::logTiming("MAP", "try_complete spine=%d known=%d complete=%d", spineIndex_,
                             engine_.mapKnownPages(), engine_.mapComplete() ? 1 : 0);
      } else if (s_stallTicks >= 8) {
        s_idleGaveUpSpine = spineIndex_;
        SystemLog::logTiming("MAP", "idle_give_up spine=%d known=%d est=%d stall=%d", spineIndex_, knownAfter, est,
                             s_stallTicks);
      }
    }
  }

  const bool nowComplete = engine_.mapComplete();
  if (nowComplete || !progressed || knownAfter != s_lastLoggedKnown || spineIndex_ != s_lastLoggedSpine ||
      (now - s_lastMapLogMs) >= 5000UL) {
    s_lastLoggedKnown = knownAfter;
    s_lastLoggedSpine = spineIndex_;
    s_lastMapLogMs = now;
    SystemLog::logTiming("MAP", "spine=%d known=%d->%d est=%d complete=%d prog=%d stall=%d page=%d fre=%u maxA=%u",
                         spineIndex_, knownBefore, knownAfter, est, nowComplete ? 1 : 0, progressed ? 1 : 0,
                         s_stallTicks, engine_.currentPage() + 1, static_cast<unsigned>(ESP.getFreeHeap()),
                         static_cast<unsigned>(ESP.getMaxAllocHeap()));
  }
  if (nowComplete) {
    persistPageMapIfComplete();
    s_stallTicks = 0;
  }
}

// After map is complete and the reader has been quiet a while, silently prime
// the next spine's first two page-map starts (SD .rvpm). Gated hard so quick
// page turns never collide with a chapter swap.
void RivuletReaderActivity::tickNextSpineWarmIfIdle() {
  if (!ready_ || !epub_ || !firstInkDone_ || chapterNavBusy_ || warmingAdjacent_) return;
  if (!engine_.mapComplete()) return;
  const unsigned long now = millis();
  if (lastPageTurnTime_ == 0UL || (now - lastPageTurnTime_) < 8000UL) return;
  if (ESP.getFreeHeap() < 95 * 1024 || ESP.getMaxAllocHeap() < 56 * 1024) return;
  if (lastNextSpineWarmFrom_ == spineIndex_) return;
  lastNextSpineWarmFrom_ = spineIndex_;
  (void)warmNextSpinePrefix(/*pages=*/2);
}

void RivuletReaderActivity::loadCachedBookmarks() {
  cachedBookmarks_.clear();
  if (cachedBookmarks_.capacity() < kInitialBookmarkCacheCapacity) {
    cachedBookmarks_.reserve(kInitialBookmarkCacheCapacity);
  }
  if (!epub_) {
    currentPageBookmarked_ = false;
    return;
  }
  if (BookmarkFile::load(epub_->getPath(), cachedBookmarks_)) {
    LOG_INF("RVR", "bookmarks loaded n=%zu path=%s", cachedBookmarks_.size(), epub_->getPath().c_str());
  } else {
    LOG_DBG("RVR", "no bookmarks for %s", epub_->getPath().c_str());
  }
  updateBookmarkFlag();
}

void RivuletReaderActivity::updateBookmarkFlag() {
  if (!epub_ || !ready_ || cachedBookmarks_.empty()) {
    currentPageBookmarked_ = false;
    return;
  }
  const int page = engine_.currentPage();
  const int pageCount = std::max(page + 1, engine_.chapterPageCount(&renderer));
  const float prog = bookProgress01();
  currentPageBookmarked_ =
      std::any_of(cachedBookmarks_.begin(), cachedBookmarks_.end(), [&](const BookmarkEntry& b) {
        return bookmarkMatchesPage(b, spineIndex_, page, pageCount, prog);
      });
}

std::string RivuletReaderActivity::pageSummaryForBookmark() const {
  std::string text;
  text.reserve(96);
  for (const auto& sp : engine_.page().spans) {
    if (sp.text.empty()) continue;
    if (!text.empty() && text.back() != ' ') text.push_back(' ');
    text += sp.text;
    if (text.size() >= 80) break;
  }
  return BookmarkUtil::sanitizeBookmarkSummary(std::move(text));
}

void RivuletReaderActivity::toggleBookmark() {
  if (!epub_ || !ready_) return;

  const int page = engine_.currentPage();
  const int pageCount = std::max(page + 1, engine_.chapterPageCount(&renderer));
  const float prog = bookProgress01();

  const size_t before = cachedBookmarks_.size();
  cachedBookmarks_.erase(std::remove_if(cachedBookmarks_.begin(), cachedBookmarks_.end(),
                                        [&](const BookmarkEntry& b) {
                                          return bookmarkMatchesPage(b, spineIndex_, page, pageCount, prog);
                                        }),
                         cachedBookmarks_.end());

  bool removed = cachedBookmarks_.size() != before;
  if (removed) {
    currentPageBookmarked_ = false;
  } else {
    BookmarkEntry entry;
    entry.percentage = prog;
    // Rivulet does not keep classic XPath progress; store a stable chapter hint.
    entry.xpath = "/body/DocFragment[" + std::to_string(spineIndex_ + 1) + "]/body";
    entry.summary = pageSummaryForBookmark();
    entry.computedSpineIndex = static_cast<uint16_t>(std::clamp(spineIndex_, 0, 65535));
    entry.computedChapterPageCount = static_cast<uint16_t>(std::clamp(pageCount, 0, 65535));
    entry.computedChapterProgress = static_cast<uint16_t>(std::clamp(page, 0, 65535));
    cachedBookmarks_.insert(cachedBookmarks_.begin(), std::move(entry));
    currentPageBookmarked_ = true;
  }

  if (!BookmarkFile::save(epub_->getPath(), cachedBookmarks_)) {
    LOG_ERR("RVR", "Failed to save bookmarks");
    GUI.drawPopup(renderer, "Bookmark save failed", BaseTheme::kPopupCenterY, true);
  } else {
    GUI.drawPopup(renderer, removed ? tr(STR_BOOKMARK_REMOVED) : tr(STR_BOOKMARK_ADDED), BaseTheme::kPopupCenterY,
                  true);
  }
  delay(400);
  requestUpdate();
}

void RivuletReaderActivity::jumpToBookmarkProgress(const ProgressChangeResult& sync) {
  if (!epub_) return;

  int targetSpine = sync.spineIndex;
  int targetPage = sync.page;
  const int n = epub_->getSpineItemsCount();

  // Prefer stored spine/page when in range; else approximate via book %.
  const bool spineOk = targetSpine >= 0 && targetSpine < n;
  const bool pageHintOk = spineOk && sync.totalPages > 0 && targetPage >= 0;

  bool jumped = false;
  if (pageHintOk) {
    if (loadSpine(targetSpine)) {
      // Scale page if chapter page count changed since bookmark was taken.
      const int nowCount = std::max(1, engine_.chapterPageCount(&renderer));
      int page = targetPage;
      if (sync.totalPages > 0 && sync.totalPages != nowCount) {
        page = static_cast<int>((static_cast<float>(targetPage) / static_cast<float>(sync.totalPages)) *
                                    static_cast<float>(nowCount) +
                                0.5f);
        page = std::clamp(page, 0, std::max(0, nowCount - 1));
      }
      (void)engine_.goToPage(renderer, page, /*maxWalkPages=*/512);
      jumped = true;
      firstPaint_ = true;
    }
  }

  if (!jumped && sync.hasSavedProgress && sync.percentage > 0.0f) {
    jumpToPercent(std::clamp(static_cast<int>(sync.percentage * 100.0f + 0.5f), 0, 100));
    jumped = ready_;
  } else if (!jumped && spineOk) {
    if (loadSpine(targetSpine)) {
      firstPaint_ = true;
      jumped = true;
    }
  }

  if (jumped) {
    updateBookmarkFlag();
    (void)saveProgress();
    persistHomeProgress(true);
  } else {
    GUI.drawPopup(renderer, "Could not open bookmark", BaseTheme::kPopupCenterY, true);
    delay(400);
  }
}

bool RivuletReaderActivity::extractEpubItem(void* ctx, const char* srcPath, const char* destPath) {
  auto* self = static_cast<RivuletReaderActivity*>(ctx);
  if (!self || !self->epub_ || !srcPath || !destPath) return false;
  return self->epub_->extractItemToFile(srcPath, destPath);
}

void RivuletReaderActivity::prepareChapterImages(const std::string& spineHref) {
  if (!epub_) return;
  auto& chapter = engine_.chapterMutable();
  auto& blocks = chapter.blocksMutable();
  if (blocks.empty()) return;
  // Suppress / Placeholder bake at convert; still zero plates if stale IR slipped through.
  if (SETTINGS.imageRendering == CasperSettings::IMAGES_SUPPRESS ||
      SETTINGS.imageRendering == CasperSettings::IMAGES_PLACEHOLDER) {
    int n = 0;
    for (auto& b : blocks) {
      if (b.kind != rivulet::BlockKind::Image) continue;
      b.imageW = 0;
      b.imageH = 0;
      ++n;
    }
    if (n > 0) {
      LOG_INF("RVR", "prepareChapterImages skip %d plates (imageRendering=%u)", n,
              static_cast<unsigned>(SETTINGS.imageRendering));
    }
    return;
  }

  // Directory of the HTML spine item (OEBPS/Text/ch.xhtml → OEBPS/Text/).
  std::string baseDir;
  {
    const auto slash = spineHref.find_last_of('/');
    if (slash != std::string::npos) baseDir = spineHref.substr(0, slash + 1);
  }
  const int viewW = std::max(32, static_cast<int>(engine_.renderKey().viewportW));
  const int viewH = std::max(32, static_cast<int>(engine_.renderKey().viewportH));
  const auto& runs = chapter.runs();
  int prepared = 0;
  int skipped = 0;
  // Resolve HTML-relative or package-absolute href → ZIP path inside the EPUB.
  // Tries candidates against the ZIP (getItemSize) so we never prefix baseDir onto
  // an already-absolute path (OEBPS/Text/ + OEBPS/Images/… → broken double path).
  auto resolveItemPath = [&](const std::string& rel) -> std::string {
    if (rel.empty()) return {};
    const std::string decoded = FsHelpers::decodeUriEscapes(rel);
    std::string cands[4];
    int nc = 0;
    auto add = [&](std::string p) {
      p = FsHelpers::normalisePath(std::move(p));
      if (p.empty()) return;
      for (int i = 0; i < nc; ++i) {
        if (cands[i] == p) return;
      }
      if (nc < 4) cands[nc++] = std::move(p);
    };
    // Relative to chapter HTML (../Images/orn.png).
    if (!decoded.empty() && decoded[0] == '.') {
      add(baseDir + decoded);
    }
    // Package-absolute (rewritten IR) or bare path from HTML.
    add(decoded);
    if (!baseDir.empty()) add(baseDir + decoded);
    // Prefer a path that actually exists in the EPUB zip.
    size_t itemSz = 0;
    for (int i = 0; i < nc; ++i) {
      if (epub_->getItemSize(cands[i], &itemSz) && itemSz > 0) return cands[i];
    }
    return nc > 0 ? cands[0] : std::string{};
  };

  for (size_t bi = 0; bi < blocks.size(); ++bi) {
    auto& b = blocks[bi];
    if (b.kind != rivulet::BlockKind::Image || b.runCount == 0) continue;
    if (b.runBegin >= runs.size()) continue;
    std::string rel = chapter.runString(runs[b.runBegin]);
    if (rel.empty()) continue;
    std::string resolved = resolveItemPath(rel);
    if (resolved.empty()) {
      LOG_ERR("RVR", "image resolve fail rel=%s base=%s", rel.c_str(), baseDir.c_str());
      b.imageW = 0;
      b.imageH = 0;
      ++skipped;
      continue;
    }
    if (!ImageDecoderFactory::isFormatSupported(resolved)) {
      // SVG ornamental breaks etc. — leave 0×0 so layouter skips the plate and
      // the EPUB text fallback ("* * *") is the only ink (no hollow white box).
      LOG_DBG("RVR", "image skip unsupported %s", resolved.c_str());
      b.imageW = 0;
      b.imageH = 0;
      ++skipped;
      continue;
    }

    ImageDimensions dims{0, 0};
    ImageDimsProbe probe;
    const bool streamOk = epub_->readItemContentsToStream(resolved, probe, 1024, /*allowEarlyStop=*/true);
    if (!probe.getDimensions(dims) || dims.width <= 0 || dims.height <= 0) {
      // Fallback box so the page still reserves space — paint may still extract/decode.
      dims.width = static_cast<uint16_t>(viewW);
      dims.height = static_cast<uint16_t>(std::max(64, viewH / 3));
      LOG_ERR("RVR", "image dims probe fail streamOk=%d path=%s — using %dx%d", streamOk ? 1 : 0,
              resolved.c_str(), dims.width, dims.height);
    }

    int iw = dims.width;
    int ih = dims.height;
    // CSS width from figleft/figright (stored on block before probe) wins as display width.
    int cssW = b.imageW > 0 ? static_cast<int>(b.imageW) : 0;
    bool leftFloat = (b.flags & rivulet::kBlockFloatLeft) != 0;
    bool rightFloat = (b.flags & rivulet::kBlockFloatRight) != 0;
    const bool isOrnament = (b.flags & rivulet::kBlockOrnament) != 0;
    // Illuminae briefings are ~723px document plates with CSS float:right; width:40%.
    // On e-ink 40% is unreadable. Large document floats become nearly full-width
    // centered plates; only small stamps stay as true side floats.
    const bool docFloatPlate =
        (leftFloat || rightFloat) && !isOrnament && dims.width >= 280 && dims.height >= 120;
    if (docFloatPlate) {
      b.flags = static_cast<uint16_t>(b.flags & ~(rivulet::kBlockFloatLeft | rivulet::kBlockFloatRight));
      leftFloat = false;
      rightFloat = false;
      cssW = std::max(240, (viewW * 94) / 100);
    } else if ((leftFloat || rightFloat) && dims.width > 0) {
      // Small float icons / email stamps — keep a modest side column.
      const int floatCap = std::max(80, (viewW * 45) / 100);
      if (cssW <= 0 || cssW > floatCap || cssW < 40) {
        cssW = floatCap;
      }
    }
    if (cssW > 0 && cssW <= viewW && dims.width > 0) {
      // Scale natural aspect to CSS width (Alice figleft 80 / figright 183).
      ih = std::max(1, (dims.height * cssW) / std::max(1, static_cast<int>(dims.width)));
      iw = cssW;
    }
    // Fit width; cap height so a single plate never exceeds ~90% of the page.
    if (iw > viewW) {
      ih = std::max(1, (ih * viewW) / iw);
      iw = viewW;
    }
    const int maxH = (viewH * 9) / 10;
    if (ih > maxH) {
      iw = std::max(1, (iw * maxH) / ih);
      ih = maxH;
    }
    // Chapter ornaments (.orn img { width: 12% }) — never full-page plates.
    if (isOrnament && dims.width > 0) {
      const int targetW = std::max(28, (viewW * 12) / 100);
      ih = std::max(1, (dims.height * targetW) / std::max(1, static_cast<int>(dims.width)));
      iw = targetW;
    }

    // Letter-shrink ONLY narrow LEFT floats (Alice ornate C). Never shrink figright
    // plates (183×450) — that produced the tiny white box on page 3.
    // Ornaments stay centered (kBlockOrnament) — never mis-tagged as letter floats.
    const int bodyLineEst = std::max(18, renderer.getLineHeight(engine_.renderKey().fontId, 1.0f));
    const int maxLetterW = std::max(120, (viewW * 28) / 100);
    const bool letterGlyph = !isOrnament && leftFloat && iw > 0 && iw <= maxLetterW;
    if (letterGlyph) {
      const int targetH = bodyLineEst * 2;
      if (ih > targetH && ih > 0) {
        iw = std::max(1, (iw * targetH) / ih);
        ih = targetH;
      }
    } else if (!isOrnament && !leftFloat && !rightFloat && iw <= maxLetterW && iw <= ih * 2 &&
               ih <= bodyLineEst * 4) {
      // Heuristic letter without float class.
      const int targetH = bodyLineEst * 2;
      if (ih > targetH && ih > 0) {
        iw = std::max(1, (iw * targetH) / ih);
        ih = targetH;
      }
      b.flags = static_cast<uint16_t>(b.flags | rivulet::kBlockFloatLeft);
    }
    b.imageW = static_cast<uint16_t>(std::min(65535, iw));
    b.imageH = static_cast<uint16_t>(std::min(65535, ih));
    ++prepared;
    LOG_INF("RVR", "image[%u] %s %dx%d (src %dx%d) orn=%d L=%d R=%d", static_cast<unsigned>(bi),
            resolved.c_str(), iw, ih, static_cast<int>(dims.width), static_cast<int>(dims.height),
            isOrnament ? 1 : 0, leftFloat ? 1 : 0, rightFloat ? 1 : 0);

    // Store package-absolute path on the IR run so paint/extract never depend on
    // baseDir + "../Images/..." re-resolution (fragile after IR cache reload).
    if (resolved != rel) {
      (void)chapter.setRunText(b.runBegin, resolved);
    }
  }
  if (prepared > 0 || skipped > 0) {
    LOG_INF("RVR", "prepareChapterImages spine=%s prepared=%d skipped=%d free=%u maxA=%u", spineHref.c_str(),
            prepared, skipped, static_cast<unsigned>(ESP.getFreeHeap()),
            static_cast<unsigned>(ESP.getMaxAllocHeap()));
  }
}

void RivuletReaderActivity::paintPageImages() {
  if (!epub_ || !ready_) return;
  if (SETTINGS.imageRendering == CasperSettings::IMAGES_SUPPRESS ||
      SETTINGS.imageRendering == CasperSettings::IMAGES_PLACEHOLDER) {
    return;  // convert already omitted/replaced plates; never decode
  }
  const auto& plates = engine_.page().images;
  if (plates.empty()) return;

  // Ensure extractor is live — menu / KO sync / heap prep can clear it.
  ImageBlock::setExtractor(this, &RivuletReaderActivity::extractEpubItem);

  const auto item = epub_->getSpineItem(spineIndex_);
  std::string baseDir;
  {
    const auto slash = item.href.find_last_of('/');
    if (slash != std::string::npos) baseDir = item.href.substr(0, slash + 1);
  }
  // Prefer rivulet/img; fall back to package-adjacent if irDir was empty.
  std::string imgCacheDir = irDir_.empty() ? (casperBookDir_ + "/rivulet/img/") : (irDir_ + "/img/");
  if (!imgCacheDirReady_) {
    Storage.ensureDirectoryExists(imgCacheDir.c_str());
    imgCacheDirReady_ = true;
  }

  // Free PNG warm so chapter ornaments / full plates can allocate the decoder.
  PngToFramebufferConverter::releaseWarmIfHeapTight(/*minMaxAllocBytes=*/48 * 1024);
  // Resolve HTML-relative or package-absolute href → ZIP path inside the EPUB.
  // Tries candidates against the ZIP (getItemSize) so we never prefix baseDir onto
  // an already-absolute path (OEBPS/Text/ + OEBPS/Images/… → broken double path).
  auto resolveItemPath = [&](const std::string& rel) -> std::string {
    if (rel.empty()) return {};
    const std::string decoded = FsHelpers::decodeUriEscapes(rel);
    std::string cands[4];
    int nc = 0;
    auto add = [&](std::string p) {
      p = FsHelpers::normalisePath(std::move(p));
      if (p.empty()) return;
      for (int i = 0; i < nc; ++i) {
        if (cands[i] == p) return;
      }
      if (nc < 4) cands[nc++] = std::move(p);
    };
    // Relative to chapter HTML (../Images/orn.png).
    if (!decoded.empty() && decoded[0] == '.') {
      add(baseDir + decoded);
    }
    // Package-absolute (rewritten IR) or bare path from HTML.
    add(decoded);
    if (!baseDir.empty()) add(baseDir + decoded);
    // Prefer a path that actually exists in the EPUB zip.
    size_t itemSz = 0;
    for (int i = 0; i < nc; ++i) {
      if (epub_->getItemSize(cands[i], &itemSz) && itemSz > 0) return cands[i];
    }
    return nc > 0 ? cands[0] : std::string{};
  };

  auto destForHref = [&](const std::string& resolved) -> std::string {
    std::string ext;
    const auto dot = resolved.rfind('.');
    if (dot != std::string::npos) ext = resolved.substr(dot);
    const size_t h = std::hash<std::string>{}(resolved);
    char name[48];
    std::snprintf(name, sizeof(name), "%08x%s", static_cast<unsigned>(h & 0xffffffffu),
                  ext.empty() ? ".img" : ext.c_str());
    return imgCacheDir + name;
  };

  int painted = 0;
  for (const auto& plate : plates) {
    if (plate.href.empty() || plate.w <= 0 || plate.h <= 0) continue;

    // IR rewrite stores a package-absolute href. If that hash is already on SD,
    // skip the ZIP getItemSize probe (central-directory walk on every paint).
    const std::string hrefDecoded = FsHelpers::decodeUriEscapes(plate.href);
    std::string destPath = destForHref(hrefDecoded);
    std::string resolved = hrefDecoded;
    if (!Storage.exists(destPath.c_str())) {
      resolved = resolveItemPath(plate.href);
      if (resolved.empty() || !ImageDecoderFactory::isFormatSupported(resolved)) {
        // Do not draw a hollow white box for unsupported images (SVG ornaments).
        LOG_ERR("RVR", "paint skip image href=%s resolved=%s", plate.href.c_str(),
                resolved.empty() ? "(empty)" : resolved.c_str());
        continue;
      }
      destPath = destForHref(resolved);
    } else if (!ImageDecoderFactory::isFormatSupported(resolved)) {
      continue;
    }

    LOG_DBG("RVR", "paint image %s -> %s at %d,%d %dx%d", resolved.c_str(), destPath.c_str(),
            marginX_ + plate.x, marginY_ + plate.y, plate.w, plate.h);
    ImageBlock ib(destPath, resolved, plate.w, plate.h);
    ib.render(renderer, marginX_ + plate.x, marginY_ + plate.y);
    ++painted;
  }
  if (painted > 0) {
    LOG_INF("RVR", "paintPageImages n=%d free=%u maxA=%u", painted, static_cast<unsigned>(ESP.getFreeHeap()),
            static_cast<unsigned>(ESP.getMaxAllocHeap()));
  }
  ImageBlock::releaseRenderCache();
}

void RivuletReaderActivity::onEnter() {
  Activity::onEnter();
  // Honor Settings button map + Reading Orientation (PageBack/PageForward remap).
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
  if (!epub_) {
    showError("No EPUB");
    return;
  }

  ImageBlock::setExtractor(this, &RivuletReaderActivity::extractEpubItem);
  ImageBlock::clearSessionRenderFailures();

  // --- Ownership pillars: stable id + /.crosspoint/book_<id>/ + ledger ---
  stableId_ = CasperBook::openBook(epub_->getPath(), epub_->getTitle(), epub_->getAuthor());
  casperBookDir_ = CasperBook::bookDir(stableId_);
  irDir_ = CasperBook::rivuletDir(stableId_);

  configureRenderKey();
  epub_->setupCacheDir();  // package book.bin under /.crosspoint (or legacy hit)
  // Clippings load deferred until save/list — saves SD + parse on every open.

  // Stats SD: defer on QR/warm open hints so first ink is not blocked on
  // book stats + global stats (0.1.5 opened the page first).
  const bool snappyOpen = ReaderActivity::hasOpenHints();
  if (!snappyOpen) {
    readingStats_ = CasperStats::loadBook(epub_->getPath());
    if (SETTINGS.readingStatsTrackingEnabled()) {
      globalReadingStats_ = GlobalReadingStats::load();
    }
  } else {
    pendingStatsLoad_ = true;
  }
  readingSessionStartMs_ = millis();
  lastPageTurnTime_ = readingSessionStartMs_;

  // Sticky path for QR in RAM immediately; SD write deferred until after first ink
  // (state.json save was ~100–200ms on the critical open path).
  APP_STATE.openEpubPath = epub_->getPath();
  APP_STATE.readerActivityLoadCount = 0;
  pendingOpenStateSave_ = true;
  // Recents: skip SD rewrite if this book is already front of the list.
  // Defer addBook SD write to after first paint as well.
  {
    const auto& recents = RECENT_BOOKS.getBooks();
    pendingRecentsTouch_ = recents.empty() || recents[0].path != epub_->getPath();
  }
  pagesUntilFullRefresh_ = SETTINGS.getRefreshFrequency();

  // Reinforce Opening after the activity swap (Home already painted it). Skip on
  // QR wake — glass already has the page; don't flash status over it.
  if (!QrTimingLog::active()) {
    GUI.drawTopLeftStatus(renderer, tr(STR_STATUS_OPENING), /*refresh=*/true);
  }

  // Hyphenation language from OPF (Liang patterns).
  Hyphenator::setPreferredLanguage(epub_->getLanguage());

  LOG_INF("RVR", "onEnter path=%s id=%s casper=%s spines=%d free=%u maxAlloc=%u", epub_->getPath().c_str(),
          stableId_.c_str(), casperBookDir_.c_str(), epub_->getSpineItemsCount(),
          static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));

  // Resume: casper progress.bin → legacy package/Casper progress.bin →
  // stats % (home ring) → first-open land past cover spines.
  int resumeSpine = -1;
  int resumePage = 0;
  loadProgress(resumeSpine, resumePage);

  // Precise progress.bin missing: land near stats progress % (classic did this
  // when only the home ring survived a failed progress write / old path layout).
  if (resumeSpine < 0) {
    const float statsPct = readingStats_.getProgressPercent();
    if (statsPct >= 1.0f) {
      LOG_INF("RVR", "progress.bin missing; resuming from stats %.1f%%", static_cast<double>(statsPct));
      // jumpToPercent loads spine + page and saves casper progress.bin.
      jumpToPercent(static_cast<int>(statsPct + 0.5f));
      if (ready_) {
        requestUpdate();
        return;
      }
    }
  }

  int start = 0;
  if (resumeSpine >= 0 && resumeSpine < epub_->getSpineItemsCount()) {
    start = resumeSpine;
  } else {
    const int land = epub_->getFirstOpenSpineIndex();
    if (land > 0) start = land;
    resumePage = 0;
  }

  bool loaded = false;
  // Cap attempts + yield so a bad book cannot soft-lock the UI for minutes.
  constexpr int kMaxSpineAttempts = 24;
  int attempts = 0;
  for (int pass = 0; pass < 2 && !loaded; ++pass) {
    const int from = (pass == 0) ? start : 0;
    const int to = epub_->getSpineItemsCount();
    for (int i = from; i < to; ++i) {
      if (pass == 1 && i >= start) break;  // already tried [start, end)
      if (++attempts > kMaxSpineAttempts) {
        LOG_ERR("RVR", "open: spine attempt cap %d — stop", kMaxSpineAttempts);
        break;
      }
      yield();
      // Resume page inside loadSpine when this is the saved spine (one layout pass).
      // Include page 0 so a saved chapter start is not re-derived as first-open land.
      const int pageArg = (resumeSpine >= 0 && i == resumeSpine) ? resumePage : 0;
      if (loadSpine(i, pageArg)) {
        loaded = true;
        break;
      }
    }
  }
  if (!loaded) {
    showError("No readable chapters");
    return;
  }
  // Path-keyed bookmarks under /.crosspoint/bookmarks (migrate + classic share this).
  loadCachedBookmarks();
  // QR slept on the book menu: reopen it after first paint so wake lands in-menu.
  if (APP_STATE.sleepResumeTarget == CasperState::RESUME_READER_MENU) {
    APP_STATE.sleepResumeTarget = CasperState::RESUME_READER;
    openReaderMenu();
    return;
  }
  requestUpdate();
}

void RivuletReaderActivity::onExit() {
  // leaveReaderToHome already flushed under status chrome so PopToHome does
  // not stall on SD with the book page frozen and no feedback.
  if (!leaveExitFlushed_) {
    flushExitProgressAndStats();
  } else {
    LOG_INF("RVR", "exit (pre-flushed) spine=%d page=%d id=%s", spineIndex_, engine_.currentPage(),
            stableId_.c_str());
  }
  readingSessionStartMs_ = 0;
  leaveExitFlushed_ = false;
  ImageBlock::setExtractor(nullptr, nullptr);
  ImageBlock::releaseRenderCache();
  Activity::onExit();
  engine_.clear();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
}

void RivuletReaderActivity::openReaderMenu() {
  if (!epub_ || !ready_) return;
  const uint32_t t0 = millis();
  // Snapshot menu chrome numbers while the chapter is still warm. Do NOT
  // releaseHeavyForUi here — that SD save + IR/font scrub made every menu open
  // feel multi-second. Release only when launching heap-heavy children (Fonts /
  // Reader UI). Cancel / light actions keep the chapter in RAM for instant return.
  const int page = (engine_.hasChapter() ? engine_.currentPage() : heldPageForUi_) + 1;
  const int total = std::max(page, engine_.hasChapter() ? engine_.chapterPageCount(&renderer) : page);
  const int bookProgressPercent = std::clamp(static_cast<int>(bookProgress01() * 100.0f + 0.5f), 0, 100);
  const std::string title = epub_->getTitle().empty() ? epub_->getPath() : epub_->getTitle();
  if (!cachedBookmarks_.empty() && !currentPageBookmarked_) {
    updateBookmarkFlag();
  }
  const bool hasFootnotes = !currentPageFootnotes_.empty();
  const bool hasClips = clippingsLoaded_ && CLIPPINGS.hasClippings();
  const bool hasBookmarks = !cachedBookmarks_.empty();
  const bool pageBookmarked = currentPageBookmarked_;
  const bool bookCompleted = readingStats_.isCompleted;
  // Cheap pin so sleep / crash during the menu still has a place — no IR drop.
  (void)saveProgress();
  LOG_INF("RVR", "openReaderMenu prep=%lums page=%d/%d spine=%d bm=%zu clips=%d fn=%d",
          static_cast<unsigned long>(millis() - t0), page, total, spineIndex_, cachedBookmarks_.size(),
          hasClips ? 1 : 0, hasFootnotes ? 1 : 0);

  const uint8_t darkModeOnOpen = SETTINGS.readerDarkMode;
  startActivityForResult(
      std::make_unique<EpubReaderMenuActivity>(renderer, mappedInput, title, page, total, bookProgressPercent,
                                               SETTINGS.orientation, hasFootnotes, hasBookmarks, hasClips,
                                               pageBookmarked, bookCompleted),
      [this, darkModeOnOpen](const ActivityResult& result) {
        using MA = EpubReaderMenuActivity::MenuAction;
        int action = -1;
        if (const auto* menu = std::get_if<MenuResult>(&result.data)) {
          // Orientation / front-button follow apply even when cancelled.
          if (SETTINGS.orientation != menu->orientation) {
            GUI.drawTopLeftStatus(renderer, tr(STR_LOADING_POPUP), /*refresh=*/true);
            SETTINGS.orientation = menu->orientation;
            SETTINGS.frontButtonFollowOrientation =
                CasperSettings::defaultFrontButtonFollowForOrientation(menu->orientation);
            SETTINGS.saveToFile();
            ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
            configureRenderKey();
            if (heavyReleasedForUi_) {
              (void)restoreAfterUi(/*showLoading=*/false);
            } else {
              // Chapter still warm — re-layout at the same place under the new key.
              const int spine = spineIndex_;
              const int pageNow = engine_.hasChapter() ? engine_.currentPage() : 0;
              (void)loadSpine(spine, pageNow);
            }
            firstPaint_ = true;
          } else if (SETTINGS.frontButtonFollowOrientation != menu->frontButtonFollowOrientation) {
            SETTINGS.frontButtonFollowOrientation = menu->frontButtonFollowOrientation;
            SETTINGS.saveToFile();
          }
          if (!result.isCancelled && menu->action >= 0) {
            action = menu->action;
          }
        }

        // Live Dark Mode toggle never returns an action — scrub polarity on leave.
        if (SETTINGS.readerDarkMode != darkModeOnOpen) {
          firstPaint_ = true;
          pagesUntilFullRefresh_ = CasperSettings::REFRESH_COUNTDOWN_FORCE_SCRUB;
        }

        // Leave reader: no restore needed (onExit / leave path handles cleanup).
        if (action == static_cast<int>(MA::GO_HOME)) {
          onReaderMenuAction(action);
          return;
        }

        // Fonts / Reader UI need contiguous heap — release now, restore on child return.
        if (action == static_cast<int>(MA::MANAGE_FONTS) || action == static_cast<int>(MA::MANAGE_READER_UI)) {
          releaseHeavyForUi();
          onReaderMenuAction(action);
          return;
        }

        // Delete cache paints its own Deleting/Opening chrome and full reload —
        // do not requestUpdate() afterward (that raced the status off the glass).
        if (action == static_cast<int>(MA::DELETE_CACHE)) {
          onReaderMenuAction(action);
          return;
        }

        // Back to reading (cancel) or any action that needs the page.
        if (heavyReleasedForUi_) {
          (void)restoreAfterUi();
        }
        if (action >= 0) {
          onReaderMenuAction(action);
        }
        requestUpdate();
      });
}

void RivuletReaderActivity::onReaderMenuAction(const int action) {
  using MA = EpubReaderMenuActivity::MenuAction;
  switch (static_cast<MA>(action)) {
    case MA::GO_HOME:
      leaveReaderToHome();
      return;
    case MA::SELECT_CHAPTER: {
      if (!epub_) return;
      const int spineIdx = spineIndex_;
      const std::string path = epub_->getPath();
      startActivityForResult(
          std::make_unique<EpubReaderChapterSelectionActivity>(renderer, mappedInput, epub_, path, spineIdx),
          [this](const ActivityResult& result) {
            if (!result.isCancelled) {
              if (const auto* chapter = std::get_if<ChapterResult>(&result.data)) {
                LOG_INF("RVR", "chapter select spine=%d anchor=%s", chapter->spineIndex,
                        chapter->anchor.empty() ? "-" : chapter->anchor.c_str());
                // Stay inside this TOC entry's spine range — never scan forward into
                // later chapters when the target fails (that landed users on Ch 5).
                if (loadTocChapter(chapter->spineIndex)) {
                  firstPaint_ = true;
                  (void)saveProgress();
                } else {
                  // loadTocChapter restores the previous spine; do not leave Empty page.
                  GUI.drawPopup(renderer, "Chapter not readable", BaseTheme::kPopupCenterY, true);
                  delay(600);
                  firstPaint_ = true;
                }
              }
            }
            requestUpdate();
          });
      return;
    }
    case MA::MANAGE_READER_UI:
      // Same screen as Settings → Reader → Manage Reader UI (corners / font size).
      // Chapter already released by openReaderMenu when launched from menu.
      startActivityForResult(std::make_unique<StatusBarSettingsActivity>(renderer, mappedInput),
                             [this](const ActivityResult&) {
                               (void)SETTINGS.saveToFile();
                               configureRenderKey();
                               // Always restore the held place — never goToStart.
                               if (!restoreAfterUi()) {
                                 LOG_ERR("RVR", "Manage Reader UI restore failed — progress pinned");
                               }
                               requestUpdate();
                             });
      return;
    case MA::MANAGE_FONTS:
      // Chapter released while fonts UI runs; restore to held page (reflow via load).
      startActivityForResult(
          std::make_unique<TextSettingsActivity>(renderer, mappedInput, &sdFontSystem.registry(),
                                                 TextSettingsActivity::Tab::Family),
          [this](const ActivityResult&) {
            configureRenderKey();
            // NEVER goToStart here — that reset readers to chapter page 1 (and
            // cascading progress.bin=0) after checking AA / font settings.
            if (!heavyReleasedForUi_) {
              // Should be released from openReaderMenu; re-hold from progress if not.
              int spine = spineIndex_;
              int page = engine_.currentPage();
              loadProgress(spine, page);
              heldSpineForUi_ = spine;
              heldPageForUi_ = std::max(0, page);
              heavyReleasedForUi_ = true;
            }
            if (!restoreAfterUi()) {
              LOG_ERR("RVR", "Manage Fonts restore failed — progress pinned");
            }
            requestUpdate();
          });
      return;
    case MA::GO_TO_PERCENT: {
      const int initial = std::clamp(static_cast<int>(bookProgress01() * 100.0f + 0.5f), 0, 100);
      startActivityForResult(std::make_unique<EpubReaderPercentSelectionActivity>(renderer, mappedInput, initial),
                             [this](const ActivityResult& result) {
                               if (!result.isCancelled) {
                                 if (const auto* p = std::get_if<PercentResult>(&result.data)) {
                                   jumpToPercent(p->percent);
                                 }
                               }
                               requestUpdate();
                             });
      return;
    }
    case MA::READING_STATS:
      openBookStats();
      return;
    case MA::DICTIONARY:
      openDictionary();
      return;
    case MA::VIEW_CLIPPINGS:
      openClippingList();
      return;
    case MA::SAVE_CLIPPING:
      openClippingTool();
      return;
    case MA::BOOKMARKS: {
      if (!epub_) return;
      startActivityForResult(
          std::make_unique<EpubReaderBookmarksActivity>(renderer, mappedInput, epub_, epub_->getPath()),
          [this](const ActivityResult& result) {
            loadCachedBookmarks();
            if (!result.isCancelled) {
              if (const auto* sync = std::get_if<ProgressChangeResult>(&result.data)) {
                jumpToBookmarkProgress(*sync);
              }
            }
            requestUpdate();
          });
      return;
    }
    case MA::TOGGLE_BOOKMARK:
      toggleBookmark();
      return;
    case MA::DELETE_BOOKMARKS: {
      if (!epub_) return;
      startActivityForResult(
          std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_DELETE_BOOKMARKS), epub_->getTitle()),
          [this](const ActivityResult& result) {
            if (!result.isCancelled && epub_) {
              cachedBookmarks_.clear();
              currentPageBookmarked_ = false;
              if (!BookmarkFile::save(epub_->getPath(), cachedBookmarks_)) {
                LOG_ERR("RVR", "Failed to clear bookmarks");
              }
            }
            requestUpdate();
          });
      return;
    }
    case MA::TOGGLE_COMPLETED:
      setBookCompleted(!readingStats_.isCompleted);
      return;
    case MA::FOOTNOTES:
      openFootnotesMenu();
      return;
    case MA::SYNC:
      (void)launchKOReaderSync();
      return;
    case MA::SCREENSHOT:
      takeReaderScreenshot();
      return;
    case MA::DISPLAY_QR: {
      const std::string text = currentPagePlainText(900);
      if (text.empty()) {
        BookActions::drawToast(renderer, "No text on page");
        delay(500);
        requestUpdate();
        return;
      }
      startActivityForResult(std::make_unique<QrDisplayActivity>(renderer, mappedInput, text),
                             [this](const ActivityResult&) { requestUpdate(); });
      return;
    }
    case MA::DELETE_STATS: {
      // Wipe stats under book_<id> plus any legacy epub_*/legacy folders
      // loadForBook might still find (same as File Browser → Delete Book Stats).
      if (epub_) {
        (void)BookReadingStats::removeForBook(epub_->getPath());
        LOG_INF("RVR", "deleted stats for %s", epub_->getPath().c_str());
      } else if (!casperBookDir_.empty()) {
        (void)BookReadingStats::remove(casperBookDir_);
        LOG_INF("RVR", "deleted casper stats %s", casperBookDir_.c_str());
      }
      readingStats_ = BookReadingStats{};
      PenumbraThemeUi::invalidateRecentsProgressCache();
      GUI.drawPopup(renderer, "Stats deleted", BaseTheme::kPopupCenterY, true);
      delay(400);
      requestUpdate();
      return;
    }
    case MA::RESET_READING_PACE: {
      readingStats_.avgSecondsPerForwardPage = 0;
      readingStats_.paceSampleCount = 0;
      readingStats_.estimatedTimeLeftSeconds = 0;
      if (!casperBookDir_.empty()) readingStats_.save(casperBookDir_);
      GUI.drawPopup(renderer, "Pace reset", BaseTheme::kPopupCenterY, true);
      delay(400);
      requestUpdate();
      return;
    }
    case MA::DELETE_CACHE: {
      // Full derived-cache wipe for this book (package book.bin + rivulet IR/HTML).
      // Keeps progress.bin + stats under book_<id>/. Forces ZIP re-extract on open.
      // Capture place BEFORE wipe — loadSpine() defaults to page 0 and the old
      // "walk first 24 spines" fallback jumped readers (e.g. book start → ch6).
      int keepSpine = heavyReleasedForUi_ ? heldSpineForUi_ : spineIndex_;
      int keepPage = heavyReleasedForUi_ ? heldPageForUi_ : engine_.currentPage();
      {
        int progSpine = -1, progPage = 0;
        loadProgress(progSpine, progPage);
        if (progSpine >= 0) {
          keepSpine = progSpine;
          keepPage = std::max(0, progPage);
        }
      }
      // Upper-left "Deleting" like Home "Opening" / exit "Saving".
      // Menu dismiss leaves the menu still in the FB — wipe paper first so an
      // 8pt corner label is not lost on busy menu ink, then FAST-refresh.
      activityManager.waitForRenderIdle();
      renderer.clearScreen();  // white paper — menu ink must not hide the label
      GUI.drawTopLeftStatus(renderer, tr(STR_STATUS_DELETING), /*refresh=*/true);
      if (epub_) {
        clearBookCache(epub_->getPath());
        if (!irDir_.empty()) {
          const bool rivOk = wipeCacheDirectory(irDir_);
          SystemLog::logTiming("CACHE", "menu wipe rivulet=%d spine=%d page=%d", rivOk ? 1 : 0, keepSpine, keepPage);
        }
        LOG_INF("RVR", "cleared book cache path=%s — restore spine=%d page=%d", epub_->getPath().c_str(), keepSpine,
                keepPage);
        GUI.drawTopLeftStatus(renderer, tr(STR_STATUS_OPENING), /*refresh=*/true);
        // Package dir may have been removed — recreate before reload.
        epub_->setupCacheDir();
        // book.bin may be gone; reload spine/TOC from the epub.
        if (!epub_->load(true, /*skipLoadingCss=*/true)) {
          showError("Could not rebuild book index");
          return;
        }
      } else if (!irDir_.empty() && Storage.exists(irDir_.c_str())) {
        (void)wipeCacheDirectory(irDir_);
        LOG_INF("RVR", "cleared rivulet cache %s", irDir_.c_str());
        GUI.drawTopLeftStatus(renderer, tr(STR_STATUS_OPENING), /*refresh=*/true);
      }
      if (!irDir_.empty()) Storage.ensureDirectoryExists(irDir_.c_str());
      // Reset engine render caches; then reopen at the saved place.
      prepareHeapForChapterLoad(/*aggressive=*/true);
      if (!irDir_.empty()) {
        const std::string pagesDir = irDir_ + "/pages";
        Storage.ensureDirectoryExists(pagesDir.c_str());
        engine_.setPageCacheDir(pagesDir.c_str());
      } else {
        engine_.clearPageCacheDir();
        engine_.setPageCacheSpine(-1);
      }
      bool loaded = loadSpine(keepSpine, keepPage);
      if (!loaded) loaded = loadSpine(keepSpine, 0);
      if (!loaded && epub_) {
        // Last resort: nearest readable spine around the saved place (not spine 0 walk).
        const int n = epub_->getSpineItemsCount();
        for (int delta = 1; delta < n && !loaded; ++delta) {
          if (keepSpine + delta < n && loadSpine(keepSpine + delta, 0)) loaded = true;
          if (!loaded && keepSpine - delta >= 0 && loadSpine(keepSpine - delta, 0)) loaded = true;
        }
      }
      if (loaded) {
        firstPaint_ = true;
        lastSavedSpine_ = -1;
        lastSavedPage_ = -1;
        lastSavedPageCount_ = -1;
        (void)saveProgress();
        requestUpdate();
      } else {
        showError("No readable chapters");
      }
      return;
    }
    case MA::ROTATE_SCREEN:
    case MA::ORIENT_FRONT_BUTTONS:
    case MA::TOGGLE_DARK_MODE:
      // Already applied live in the menu (On/Off column + save).
      return;
    case MA::AUTO_PAGE_TURN:
      // Removed from product menu; ignore if ever dispatched.
      return;
    default:
      LOG_DBG("RVR", "menu action %d ignored", action);
      requestUpdate();
      return;
  }
}

bool RivuletReaderActivity::turnNext(const int skipPages) {
  int remaining = std::max(1, skipPages);
  bool crossedChapter = false;
  while (remaining-- > 0) {
    const bool hadAhead = engine_.aheadWarm();
    const uint32_t tTurn = millis();
    if (engine_.nextPage(renderer)) {
      // .rvpm already on SD when the map is complete — rewriting it every turn
      // is a FAT write on the path the user feels. Only dirty an incomplete map.
      if (!engine_.mapComplete()) pageMapDirty_ = true;
      SystemLog::logTiming("TURN", "next spine=%d page=%d ahead=%d ms=%lu fre=%u maxA=%u", spineIndex_,
                           engine_.currentPage() + 1, hadAhead ? 1 : 0,
                           static_cast<unsigned long>(millis() - tTurn),
                           static_cast<unsigned>(ESP.getFreeHeap()),
                           static_cast<unsigned>(ESP.getMaxAllocHeap()));
      continue;
    }
    // Only leave the chapter when live layout says the chapter is finished.
    // nextPage also fails on mid-chapter layout stuck — advancing the spine
    // there made chapter 1 look like it was only 2–3 pages long.
    if (engine_.lastTurnFail() == rivulet::RivuletEngine::TurnFail::LayoutFailed ||
        !engine_.page().atChapterEnd) {
      LOG_ERR("RVR", "nextPage stuck mid-chapter spine=%d page=%d known=%d — not advancing spine",
              spineIndex_, engine_.currentPage(), engine_.mapKnownPages());
      // Try one more progressive walk step before giving up on this turn.
      if (engine_.goToPage(renderer, engine_.currentPage() + 1, /*maxWalkPages=*/32)) {
        pageMapDirty_ = true;
        continue;
      }
      requestUpdate();
      return false;
    }
    // Seal + persist this spine's page map BEFORE opening the next file so one
    // PageBack from the next spine's page 0 can map-hit "last page" (CrossInk).
    if (engine_.sealMapAtChapterEnd()) {
      pageMapDirty_ = true;
      persistPageMapIfComplete();
    }
    // Advance spine (next non-empty href).
    const int n = epub_->getSpineItemsCount();
    bool advanced = false;
    for (int i = spineIndex_ + 1; i < n; ++i) {
      if (loadSpine(i)) {
        advanced = true;
        break;
      }
    }
    if (!advanced) {
      GUI.drawPopup(renderer, "End of book", BaseTheme::kPopupCenterY, true);
      delay(400);
      (void)saveProgress();
      requestUpdate();
      return false;
    }
    crossedChapter = true;
    forceFastAfterChapterNav_ = true;
  }
  noteForwardPageTurn();
  // Latch already waits for release (one press → one page). ignoreNextSideRelease_
  // returns from loop() before idle warm-ahead — keep it for chapter hops only,
  // where the side key is still held after a multi-second load.
  if (crossedChapter) ignoreNextSideRelease_ = true;
  pageTurnLatch_.waitingRelease = true;
  (void)saveProgress();
  // In-chapter turns: update the in-memory home percent only. CasperStats /
  // recents hits SD on chapter change, sleep, and leave (Resource Protocol 8).
  persistHomeProgress(/*writeToDisk=*/crossedChapter);
  updateBookmarkFlag();
  requestUpdate();
  return true;
}

bool RivuletReaderActivity::turnPrev(const int skipPages) {
  int remaining = std::max(1, skipPages);
  bool crossedChapter = false;
  while (remaining-- > 0) {
    // In-chapter: page N → N-1 (must work after landing on ch6 page 38).
    if (engine_.prevPage(renderer)) continue;

    // prevPage() failing does NOT mean "chapter start". A transient layout/heap
    // failure mid-chapter used to fall through here and open the previous spine,
    // which is why one Back could jump a whole chapter. Only a real boundary may
    // change spine; anything else stays on the current page.
    if (engine_.lastTurnFail() == rivulet::RivuletEngine::TurnFail::LayoutFailed ||
        !engine_.atChapterStart()) {
      LOG_ERR("RVR", "pageBack layout failed mid-chapter spine=%d page=%d — staying", spineIndex_,
              engine_.currentPage());
      SystemLog::logTiming("BACK", "layout_fail spine=%d page=%d fre=%u", spineIndex_, engine_.currentPage(),
                           static_cast<unsigned>(ESP.getFreeHeap()));
      requestUpdate();
      return true;
    }

    // Only at page 0 of this spine: open previous spine at its REAL last page
    // with a full page map (CrossInk section.bin). Goal:
    //   ch7 p1 → Back → ch6 p38 → Back → ch6 p37 → … → ch6 p1 → Back → prior spine
    // Never land as "page 0 / 1-page map" of ch6 — that made the next Back hop to ch5.
    const int originSpine = spineIndex_;
    const int originPage = engine_.currentPage();
    const int n = epub_->getSpineItemsCount();

    int targetSpine = -1;
    for (int i = originSpine - 1; i >= 0; --i) {
      if (!epub_->getSpineItem(i).href.empty()) {
        targetSpine = i;
        break;
      }
    }

    bool advanced = false;
    if (targetSpine >= 0) {
      chapterNavBusy_ = true;
      forceFastAfterChapterNav_ = true;
      SystemLog::armHangWatch("chapter_back");
      // Do not displayWindow from the main task — that raced the render task
      // and the next full ink was a 3.2s HALF (device log refresh=3257ms).
      activityManager.waitForRenderIdle();
      GUI.drawTopLeftStatus(renderer, tr(STR_LOADING_POPUP), /*refresh=*/false);
      requestUpdate();
      prepareHeapForChapterLoad(/*aggressive=*/true);
      const uint32_t tLoad = millis();
      // Prefer a complete convert, but NEVER end up with nothing: requireCompleteIr
      // makes loadSpine return false for a partial IR, and the old code then
      // restored chapter 7 without a word — the "Back just refreshes" report.
      // Order: normal load → complete-IR retry → accept whatever converted.
      bool loaded = loadSpine(targetSpine, /*startPage=*/-1, /*requireCompleteIr=*/false);
      if (loaded && engine_.chapter().failed()) {
        prepareHeapForChapterLoad(/*aggressive=*/true);
        const bool full = loadSpine(targetSpine, -1, /*requireCompleteIr=*/true);
        if (!full) {
          // Complete convert did not fit in RAM. Re-load the partial and use it.
          prepareHeapForChapterLoad(/*aggressive=*/true);
          loaded = loadSpine(targetSpine, -1, /*requireCompleteIr=*/false);
        }
      } else if (!loaded) {
        prepareHeapForChapterLoad(/*aggressive=*/true);
        loaded = loadSpine(targetSpine, -1, /*requireCompleteIr=*/false);
      }
      const bool partialIr = loaded && engine_.chapter().failed();
      if (partialIr) {
        // A partial convert is why a chapter can look like it has a single page —
        // and why Back then lands on its first page instead of its last.
        SystemLog::logTiming("IR", "partial spine=%d blocks=%u text=%u free=%u maxA=%u", targetSpine,
                             static_cast<unsigned>(engine_.chapter().blockCount()),
                             static_cast<unsigned>(engine_.chapter().textSize()),
                             static_cast<unsigned>(ESP.getFreeHeap()),
                             static_cast<unsigned>(ESP.getMaxAllocHeap()));
      }
      // These land in /.casper-logs so a user capture shows exactly which step
      // failed — LOG_INF only reaches serial, which is why earlier captures had
      // no evidence for "Back does nothing".
      SystemLog::logTiming("BACK", "load spine=%d ok=%d partial=%d text=%u blocks=%u ms=%lu fre=%u", targetSpine,
                           loaded ? 1 : 0, engine_.chapter().failed() ? 1 : 0,
                           static_cast<unsigned>(engine_.chapter().textSize()),
                           static_cast<unsigned>(engine_.chapter().blockCount()),
                           static_cast<unsigned long>(millis() - tLoad),
                           static_cast<unsigned>(ESP.getFreeHeap()));
      if (loaded) {
        const uint32_t t0 = millis();
        // Full page-map walk (or .rvpm hit). Leaves currentPage_ = last index.
        // On a partial IR we still land on the last page we could build.
        bool landed = engine_.goToLastPageNearEnd(renderer, /*maxForwardPages=*/1024, partialIr);
        if (!landed) {
          landed = engine_.goToBestEffortLastPage(renderer, /*maxWalkPages=*/1024, partialIr);
        }
        if (!landed) {
          // Last resort: show page 1 of the previous chapter. Not the paper-book
          // behaviour, but the reader MUST move — silently repainting the page the
          // user pressed Back on is the single worst outcome.
          landed = engine_.goToStart(renderer);
          if (landed) {
            LOG_ERR("RVR", "pageBack fell back to page 1 of spine=%d", targetSpine);
            SystemLog::logTiming("BACK", "fallback_page1 spine=%d partial=%d", targetSpine, partialIr ? 1 : 0);
          }
        }
        // Accept a verified end, a deep page, or the page-1 fallback above.
        const bool okLand = landed;
        LOG_INF("RVR", "pageBack prev-spine=%d page=%d/%d end=%d ok=%d walkMs=%lu", targetSpine,
                engine_.currentPage() + 1, std::max(1, engine_.mapKnownPages()),
                engine_.page().atChapterEnd ? 1 : 0, okLand ? 1 : 0,
                static_cast<unsigned long>(millis() - t0));
        SystemLog::logTiming("BACK",
                             "land spine=%d page=%d/%d end=%d ok=%d walkPages=%d block=%d/%u stop=%u skips=%d "
                             "stallKind=%d walk=%lums fre=%u",
                             targetSpine, engine_.currentPage() + 1, std::max(1, engine_.mapKnownPages()),
                             engine_.page().atChapterEnd ? 1 : 0, okLand ? 1 : 0, engine_.lastWalkPages(),
                             engine_.lastWalkBlock(), static_cast<unsigned>(engine_.chapter().blockCount()),
                             static_cast<unsigned>(engine_.lastWalkStop()), engine_.lastWalkSkips(),
                             engine_.lastWalkStallKind(), static_cast<unsigned long>(millis() - t0),
                             static_cast<unsigned>(ESP.getFreeHeap()));
        if (okLand) {
          persistPageMapIfComplete();
          // behind_ is warmed on idle — doing it here added layout/SD to the
          // already-slow chapter Back and delayed first ink.
          advanced = true;
          crossedChapter = true;
        }
      }
      chapterNavBusy_ = false;
    }

    if (!advanced) {
      SystemLog::logTiming("BACK", "restore spine=%d page=%d (prev target=%d unusable)", originSpine, originPage,
                           targetSpine);
      if (spineIndex_ != originSpine || engine_.currentPage() != originPage || !ready_) {
        prepareHeapForChapterLoad();
        if (!loadSpine(originSpine, originPage)) {
          (void)loadSpine(originSpine, 0);
        }
      }
      // Tell the user. A bare repaint of the page they pressed Back on reads as
      // "the firmware is broken"; naming the reason is honest and debuggable.
      if (targetSpine >= 0) {
        GUI.drawPopup(renderer, "Previous chapter unavailable", BaseTheme::kPopupCenterY, true);
        delay(700);
        firstPaint_ = true;
      }
      requestUpdate();
      return true;
    }
  }
  lastPageTurnTime_ = millis();
  // Chapter hops only: the side key is still held after a multi-second load.
  // In-chapter Back must reach tickIdlePageMap so behind/ahead can warm.
  if (crossedChapter) ignoreNextSideRelease_ = true;
  pageTurnLatch_.waitingRelease = true;
  (void)saveProgress();
  persistHomeProgress(true);
  updateBookmarkFlag();
  requestUpdate();
  return true;
}

bool RivuletReaderActivity::fireMenuShortcut(const uint8_t function) {
  switch (function) {
    case CasperSettings::LP_MENU_DICTIONARY:
      openDictionary();
      return true;
    case CasperSettings::LP_MENU_CLIPPINGS:
      openClippingTool();
      return true;
    case CasperSettings::LP_MENU_BOOKMARK:
      toggleBookmark();
      return true;
    case CasperSettings::LP_MENU_KOSYNC:
      return launchKOReaderSync();
    case CasperSettings::LP_MENU_READING_STATS:
      if (!SETTINGS.readingStatsTrackingEnabled()) return false;
      openBookStats();
      return true;
    case CasperSettings::LP_MENU_SLEEP:
      activityManager.goToSleep();
      return true;
    case CasperSettings::LP_MENU_FORCE_REFRESH:
      pagesUntilFullRefresh_ = 0;
      firstPaint_ = true;
      requestUpdate();
      return true;
    case CasperSettings::LP_MENU_FILE_BROWSER:
      activityManager.goToFileBrowser();
      return true;
    case CasperSettings::LP_MENU_FILE_TRANSFER:
      activityManager.goToFileTransfer();
      return true;
    case CasperSettings::LP_MENU_SCREENSHOT:
      takeReaderScreenshot();
      return true;
    case CasperSettings::LP_MENU_FOOTNOTES:
      // Same as power FOOTNOTES / menu: restore stack, jump one, or open list.
      if (footnoteDepth_ > 0) {
        restoreFootnotePosition();
      } else {
        openFootnotesMenu();
      }
      return true;
    case CasperSettings::LP_MENU_CHAPTER_SKIP:
      chapterSkipNext();
      return true;
    case CasperSettings::LP_MENU_ORIENTATION_CHANGE:
      cycleReadingOrientation(/*nextTriggered=*/false);
      return true;
    case CasperSettings::LP_MENU_ORIENTATION_FLIP:
      flipReadingOrientation();
      return true;
    case CasperSettings::LP_MENU_DARK_MODE: {
      // In-reader shortcut: toggle reader-scoped dark (book dark, Home stays light).
      const bool on = !(SETTINGS.readerDarkMode != 0 && SETTINGS.darkModeReaderOnly != 0);
      SETTINGS.readerDarkMode = on ? 1 : 0;
      if (on) SETTINGS.darkModeReaderOnly = 1;
      SETTINGS.saveToFile();
      renderer.setInvertOnDisplay(false);  // never whole-UI from a reader hold
      firstPaint_ = true;
      requestUpdate();
      return true;
    }
    case CasperSettings::LP_MENU_DISABLED:
    default:
      return false;
  }
}

void RivuletReaderActivity::applyReadingOrientation(const uint8_t neu) {
  if (neu >= CasperSettings::ORIENTATION_COUNT || neu == SETTINGS.orientation) return;

  const int keepSpine = spineIndex_;
  const int keepPage = engine_.currentPage();
  const int keepCount = std::max(1, engine_.chapterPageCount(&renderer));

  // Corner status while still on the current orientation (see menu path).
  GUI.drawTopLeftStatus(renderer, tr(STR_LOADING_POPUP), /*refresh=*/true);

  SETTINGS.orientation = neu;
  SETTINGS.frontButtonFollowOrientation = CasperSettings::defaultFrontButtonFollowForOrientation(neu);
  SETTINGS.saveToFile();
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
  configureRenderKey();

  if (loadSpine(keepSpine)) {
    const int nowCount = std::max(1, engine_.chapterPageCount(&renderer));
    int page = keepPage;
    if (keepCount > 0 && nowCount != keepCount) {
      page = static_cast<int>((static_cast<float>(keepPage) / static_cast<float>(keepCount)) *
                                  static_cast<float>(nowCount) +
                              0.5f);
      page = std::clamp(page, 0, nowCount - 1);
    }
    (void)engine_.goToPage(renderer, page, /*maxWalkPages=*/512);
    firstPaint_ = true;
    (void)saveProgress();
  }
  requestUpdate();
}

void RivuletReaderActivity::cycleReadingOrientation(const bool nextTriggered) {
  const uint8_t count = static_cast<uint8_t>(CasperSettings::ORIENTATION_COUNT);
  if (count == 0) return;
  const uint8_t cur = SETTINGS.orientation;
  const uint8_t neu =
      nextTriggered ? static_cast<uint8_t>((cur + count - 1) % count) : static_cast<uint8_t>((cur + 1) % count);
  applyReadingOrientation(neu);
}

void RivuletReaderActivity::flipReadingOrientation() {
  // Portrait ↔ Flip With. Either side long-press toggles; if currently elsewhere,
  // first flip lands on Portrait so the pair is always reachable in one hold.
  uint8_t other = SETTINGS.orientationFlipWith;
  if (other == CasperSettings::PORTRAIT || other >= CasperSettings::ORIENTATION_COUNT) {
    other = CasperSettings::LANDSCAPE_CCW;
  }
  const uint8_t neu =
      (SETTINGS.orientation == CasperSettings::PORTRAIT) ? other : static_cast<uint8_t>(CasperSettings::PORTRAIT);
  applyReadingOrientation(neu);
}

void RivuletReaderActivity::chapterSkipNext() {
  if (!epub_) return;
  const int n = epub_->getSpineItemsCount();
  const int originSpine = spineIndex_;
  const int originPage = engine_.currentPage();

  auto tryLoadNext = [&](const int spine) -> bool {
    if (spine < 0 || spine >= n || spine == originSpine) return false;
    GUI.drawTopLeftStatus(renderer, tr(STR_LOADING_POPUP), /*refresh=*/true);
    // Cold cache after Delete Cache needs the same aggressive scrub as prev-chapter.
    prepareHeapForChapterLoad(/*aggressive=*/true);
    if (!loadSpine(spine, /*startPage=*/0)) {
      prepareHeapForChapterLoad(/*aggressive=*/true);
      return loadSpine(spine, 0);
    }
    return true;
  };

  // Prefer TOC chapter boundaries (CrossInk) — raw spine+1 often stays inside the
  // same named chapter (split XHTML) so "next chapter" looked like a reload of ch6.
  const int tocCount = epub_->getTocItemsCount();
  const int tocIdx = epub_->getTocIndexForSpineIndex(originSpine);
  bool advanced = false;
  if (tocCount > 0 && tocIdx >= 0) {
    for (int t = tocIdx + 1; t < tocCount; ++t) {
      const int spine = epub_->getTocItem(t).spineIndex;
      if (spine < 0 || spine == originSpine) continue;
      if (tryLoadNext(spine)) {
        advanced = true;
        break;
      }
    }
  } else {
    for (int i = originSpine + 1; i < n; ++i) {
      if (tryLoadNext(i)) {
        advanced = true;
        break;
      }
    }
  }

  if (advanced) {
    firstPaint_ = true;
    (void)saveProgress();
    persistHomeProgress(true);
    updateBookmarkFlag();
    requestUpdate();
    return;
  }

  // Restore origin — do not claim "End of book" when a later TOC entry exists but
  // failed to load (cold convert / OOM). That matched the "keeps reloading ch6 p1" loop.
  if (spineIndex_ != originSpine || !ready_) {
    prepareHeapForChapterLoad();
    if (!loadSpine(originSpine, originPage)) {
      (void)loadSpine(originSpine, 0);
    }
  }
  const bool hasLaterToc = (tocCount > 0 && tocIdx >= 0 && tocIdx + 1 < tocCount);
  GUI.drawPopup(renderer, hasLaterToc ? "Chapter not readable" : "End of book", BaseTheme::kPopupCenterY, true);
  delay(400);
  requestUpdate();
}

void RivuletReaderActivity::chapterSkipPrev() {
  if (!epub_) return;
  // Classic: long-prev mid-chapter → jump to this chapter's start first.
  if (engine_.currentPage() > 0) {
    if (engine_.goToPage(renderer, 0, /*maxWalkPages=*/64) || engine_.goToStart(renderer)) {
      lastPageTurnTime_ = millis();
      firstPaint_ = true;
      (void)saveProgress();
      persistHomeProgress(true);
      updateBookmarkFlag();
      requestUpdate();
      return;
    }
  }
  const int n = epub_->getSpineItemsCount();
  const int originSpine = spineIndex_;
  const int originPage = engine_.currentPage();

  // Immediate previous TOC chapter only — never scan ch6→ch5→…→ch3 (that is
  // what landed users on chapter 3 after a long blink rebuild).
  int targetSpine = -1;
  const int tocCount = epub_->getTocItemsCount();
  const int tocIdx = epub_->getTocIndexForSpineIndex(originSpine);
  if (tocCount > 0 && tocIdx > 0) {
    for (int t = tocIdx - 1; t >= 0; --t) {
      const int spine = epub_->getTocItem(t).spineIndex;
      if (spine >= 0 && spine < n && spine != originSpine) {
        targetSpine = spine;
        break;
      }
    }
  } else {
    for (int i = originSpine - 1; i >= 0; --i) {
      if (!epub_->getSpineItem(i).href.empty()) {
        targetSpine = i;
        break;
      }
    }
  }

  bool advanced = false;
  if (targetSpine >= 0) {
    chapterNavBusy_ = true;
    GUI.drawTopLeftStatus(renderer, tr(STR_LOADING_POPUP), /*refresh=*/true);
    prepareHeapForChapterLoad(/*aggressive=*/true);
    bool loaded = loadSpine(targetSpine, /*startPage=*/0, /*requireCompleteIr=*/true);
    if (!loaded || engine_.chapter().failed()) {
      prepareHeapForChapterLoad(/*aggressive=*/true);
      loaded = loadSpine(targetSpine, 0, /*requireCompleteIr=*/false);
    }
    if (loaded && !engine_.chapter().failed()) {
      const bool landed = engine_.goToBestEffortLastPage(renderer, /*maxWalkPages=*/1024);
      if (landed && (engine_.page().atChapterEnd || engine_.currentPage() > 0)) {
        if (engine_.sealMapAtChapterEnd()) {
          pageMapDirty_ = true;
          persistPageMapIfComplete();
        }
        advanced = true;
      }
    }
    chapterNavBusy_ = false;
  }

  if (advanced) {
    lastPageTurnTime_ = millis();
    firstPaint_ = true;
    (void)saveProgress();
    persistHomeProgress(true);
    updateBookmarkFlag();
    requestUpdate();
    return;
  }

  if (spineIndex_ != originSpine) {
    prepareHeapForChapterLoad();
    if (!loadSpine(originSpine, originPage)) {
      (void)loadSpine(originSpine, 0);
    }
  }
  GUI.drawPopup(renderer, targetSpine >= 0 ? "Chapter not readable" : "Start of book", BaseTheme::kPopupCenterY,
                true);
  delay(400);
  requestUpdate();
}

bool RivuletReaderActivity::tryLongPressShortcut(const uint8_t function, bool& suppressRelease) {
  if (function == CasperSettings::LP_MENU_DISABLED) return false;
  // Already fired this hold (or release still pending after child activity).
  if (suppressRelease) return false;
  const unsigned long needHold =
      (function == CasperSettings::LP_MENU_KOSYNC) ? ReaderUtils::GO_HOME_MS : ReaderUtils::BOOKMARK_HOLD_MS;
  if (mappedInput.getHeldTime() < needHold) return false;
  if (!fireMenuShortcut(function)) return false;
  suppressRelease = true;
  pendingConfirmMenuOpen_ = false;
  return true;
}

// Fire side long-press while the key is still held (Confirm-style). Must not wait
// for release: on release isPressed is false, so the old path always fell through
// to turnPrev/turnNext ("assigned Dark Mode, release went to previous page").
bool RivuletReaderActivity::trySideLongPressShortcut() {
  if (ignoreNextSideRelease_) return false;
  const bool sideA = gpio.isPressed(HalGPIO::BTN_UP);    // X3 Left / X4 Up
  const bool sideB = gpio.isPressed(HalGPIO::BTN_DOWN);  // X3 Right / X4 Down
  if (!sideA && !sideB) return false;

  const uint8_t action = sideA ? SETTINGS.longPressSideA : SETTINGS.longPressSideB;
  if (action == CasperSettings::LP_MENU_DISABLED) return false;

  const unsigned long needHold =
      (action == CasperSettings::LP_MENU_KOSYNC) ? ReaderUtils::GO_HOME_MS : ReaderUtils::BOOKMARK_HOLD_MS;
  if (mappedInput.getHeldTime() < needHold) return false;

  bool ok = false;
  if (action == CasperSettings::LP_MENU_CHAPTER_SKIP) {
    // Physical side A (left/up) = back; side B (right/down) = forward.
    if (sideA) {
      chapterSkipPrev();
    } else {
      chapterSkipNext();
    }
    ok = true;
  } else if (action == CasperSettings::LP_MENU_ORIENTATION_CHANGE) {
    cycleReadingOrientation(/*nextTriggered=*/sideB);
    ok = true;
  } else {
    ok = fireMenuShortcut(action);
  }
  if (!ok) return false;

  ignoreNextSideRelease_ = true;
  // Latch must wait for release too, or a residual edge can still turn the page.
  pageTurnLatch_.waitingRelease = true;
  pendingConfirmMenuOpen_ = false;
  LOG_INF("RVR", "side long-press hw=%s action=%u", sideA ? "A" : "B", static_cast<unsigned>(action));
  return true;
}

void RivuletReaderActivity::loop() {
  // Back must work even during chapter load / !ready_ (device: PageBack into
  // spine 21 painted, then no exit — loop returned before handleBackNavigation).
  if (footnoteDepth_ > 0 && ready_ && mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    restoreFootnotePosition();
    return;
  }
  if (ReaderUtils::handleBackNavigation(mappedInput, activityManager, epub_ ? epub_->getPath().c_str() : "",
                                        {this, [](void* ctx) {
                                           auto* self = static_cast<RivuletReaderActivity*>(ctx);
                                           self->leaveReaderToHome();
                                         }})) {
    return;
  }
  if (error_) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      onGoHome();
    }
    return;
  }
  if (!ready_) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      leaveReaderToHome();
    }
    return;
  }

  // Deferred single Confirm after double-press window → menu (classic wiring).
  if (pendingConfirmMenuOpen_ && (millis() - lastConfirmReleaseMs_) >= ReaderUtils::DOUBLE_PRESS_MENU_MS) {
    pendingConfirmMenuOpen_ = false;
    openReaderMenu();
    return;
  }

  // Confirm: long-press = SETTINGS.longPressMenuFunction (Dictionary default);
  // double-tap = SETTINGS.doublePressMenuFunction (Clipping Tool default on Casper);
  // single short = menu (deferred when double-press is enabled).
  if (ignoreNextConfirmRelease_) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      ignoreNextConfirmRelease_ = false;
    } else if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
      ignoreNextConfirmRelease_ = false;
    }
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const uint8_t dbl = SETTINGS.doublePressMenuFunction;
    if (dbl != CasperSettings::LP_MENU_DISABLED) {
      if (pendingConfirmMenuOpen_ && (millis() - lastConfirmReleaseMs_) < ReaderUtils::DOUBLE_PRESS_MENU_MS) {
        pendingConfirmMenuOpen_ = false;
        LOG_INF("RVR", "double-press Confirm → shortcut %u", static_cast<unsigned>(dbl));
        if (fireMenuShortcut(dbl)) return;
        openReaderMenu();
        return;
      }
      pendingConfirmMenuOpen_ = true;
      lastConfirmReleaseMs_ = millis();
    } else {
      openReaderMenu();
      return;
    }
  } else if (ReaderUtils::isTouchMenuGesture(mappedInput)) {
    pendingConfirmMenuOpen_ = false;
    openReaderMenu();
    return;
  }

  // Side long-press: fire while held (before page-turn latch eats the release).
  if (trySideLongPressShortcut()) {
    return;
  }
  if (ignoreNextSideRelease_) {
    if (!gpio.isPressed(HalGPIO::BTN_UP) && !gpio.isPressed(HalGPIO::BTN_DOWN)) {
      ignoreNextSideRelease_ = false;
      // Drain page-turn edges from this release so Home/reader don't double-act.
      (void)mappedInput.wasPressed(MappedInputManager::Button::PageBack);
      (void)mappedInput.wasReleased(MappedInputManager::Button::PageBack);
      (void)mappedInput.wasPressed(MappedInputManager::Button::PageForward);
      (void)mappedInput.wasReleased(MappedInputManager::Button::PageForward);
    }
    return;
  }

  // Long-press Confirm must run before page-turn latch so hold is not eaten.
  if (!ignoreNextConfirmRelease_ && mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
    if (tryLongPressShortcut(SETTINGS.longPressMenuFunction, ignoreNextConfirmRelease_)) {
      LOG_INF("RVR", "long-press Confirm → shortcut %u",
              static_cast<unsigned>(SETTINGS.longPressMenuFunction));
      return;
    }
  }

  // Power button FOOTNOTES is reader-local (sleep / QR / refresh / page-turn live in main
  // or detectPageTurn). Short and long power both map through shortPwrBtn / longPwrBtn.
  if (mappedInput.wasReleased(MappedInputManager::Button::Power) &&
      !mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    const unsigned long held = mappedInput.getHeldTime();
    const auto pwrAction = held < SETTINGS.getPowerButtonLongPressDuration()
                               ? static_cast<CasperSettings::SHORT_PWRBTN>(SETTINGS.shortPwrBtn)
                               : static_cast<CasperSettings::SHORT_PWRBTN>(SETTINGS.longPwrBtn);
    if (pwrAction == CasperSettings::SHORT_PWRBTN::FOOTNOTES) {
      if (footnoteDepth_ > 0) {
        // Quick-return from footnote body (Settings → Pwr Btn Footnote Back is product default on).
        if (SETTINGS.pwrBtnFootnoteBack != 0 || footnoteDepth_ > 0) {
          restoreFootnotePosition();
        }
      } else {
        openFootnotesMenu();
      }
      return;
    }
  }

  // Page turns: PageBack/PageForward (includes Up/Down/Left/Right + Side Layout +
  // Orient Front Buttons from the button map), tilt, optional power-button turn,
  // and touch zones — same path as Xtc/Epub/Txt readers.
  const auto touch = ReaderUtils::detectTouchPageTurn(renderer, mappedInput);
  auto [prevTriggered, nextTriggered, fromTilt] = ReaderUtils::detectPageTurn(mappedInput);
  prevTriggered = prevTriggered || touch.prev;
  nextTriggered = nextTriggered || touch.next;
  const bool fromTouch = touch.prev || touch.next;
  if (!pageTurnLatch_.accept(prevTriggered, nextTriggered, fromTilt, fromTouch, mappedInput)) {
    // No page-turn edge (normal idle) — accept() returns false. This used to
    // return before tickIdlePageMap(), so sitting on a page never advanced the
    // chapter map (device: 2 min hold, map stuck at 2/est, zero MAP lines).
    tickAaCatchUp();
    tickIdlePageMap();
    tickNextSpineWarmIfIdle();
    tickBackgroundIndexer();
    return;
  }

  // Don't chapter-skip after a power+side chord (screenshot path on some boards).
  if (gpio.wasReleased(HalGPIO::BTN_POWER) && gpio.wasReleased(HalGPIO::BTN_DOWN)) {
    return;
  }

  const unsigned long heldMs = (touch.prev || touch.next) ? touch.heldMs : mappedInput.getHeldTime();
  (void)heldMs;

  // Side long-press is handled while held (trySideLongPressShortcut above). Do not
  // re-interpret the release edge here — isPressed is false on release, so that
  // path always fell through to turnPrev/turnNext.

  if (prevTriggered) {
    (void)turnPrev(1);
    return;
  }
  if (nextTriggered) {
    (void)turnNext(1);
    return;
  }

  // A: pay off any greyscale pass owed to the page already on glass. Before the
  //    map work so a deferred first ink sharpens promptly rather than queueing
  //    behind SD I/O.
  tickAaCatchUp();
  // B: idle progressive map for the current chapter + adjacent-chapter warm.
  tickIdlePageMap();
  tickNextSpineWarmIfIdle();
  // C: once settled, index the rest of the book chapter by chapter onto SD so a
  //    jump + PageBack anywhere behaves like a paper book. Heavily gated: after
  //    first ink, idle, healthy heap, one chapter per pass.
  tickBackgroundIndexer();
}

bool RivuletReaderActivity::formatTimeLeftLabel(char* buf, const size_t len, const bool bookEstimate) const {
  if (!buf || len == 0 || !epub_ || !ready_) return false;

  uint32_t liveTotalSeconds = readingStats_.totalReadingSeconds;
  if (readingSessionStartMs_ != 0UL) {
    const unsigned long nowMs = millis();
    if (nowMs >= readingSessionStartMs_) {
      const uint32_t sessionSecs = static_cast<uint32_t>((nowMs - readingSessionStartMs_) / 1000UL);
      if (liveTotalSeconds <= UINT32_MAX - sessionSecs) liveTotalSeconds += sessionSecs;
      else liveTotalSeconds = UINT32_MAX;
    }
  }

  const uint32_t secPerPage =
      estimateSecondsPerPage(readingStats_.avgSecondsPerForwardPage, readingStats_.paceSampleCount, liveTotalSeconds,
                             readingStats_.totalPagesTurned);
  if (secPerPage == 0) {
    snprintf(buf, len, "%s", tr(STR_TIME_LEFT_CALCULATING));
    return true;
  }

  const int currentPage1 = engine_.currentPage() + 1;
  const int chapterPages = std::max(1, engine_.chapterPageCount(&renderer));
  const float sectionChapterProg = static_cast<float>(currentPage1) / static_cast<float>(chapterPages);
  const float bookProg = epub_->calculateProgress(spineIndex_, sectionChapterProg);
  const float chapterStartProg = epub_->calculateProgress(spineIndex_, 0.0f);
  const float chapterEndProg = epub_->calculateProgress(spineIndex_, 1.0f);

  float remainingPages = 0.0f;
  if (bookEstimate) {
    remainingPages =
        estimateRemainingBookPages(chapterPages, currentPage1, bookProg, chapterStartProg, chapterEndProg);
  } else {
    remainingPages = static_cast<float>(std::max(0, chapterPages - currentPage1));
  }

  uint32_t seconds = 0;
  if (bookEstimate) {
    if (!estimateBookTimeLeftSeconds(remainingPages, secPerPage, liveTotalSeconds, bookProg * 100.0f, seconds)) {
      snprintf(buf, len, "%s", tr(STR_TIME_LEFT_CALCULATING));
      return true;
    }
    if (smoothedBookTimeLeftSeconds_ == 0 && readingStats_.estimatedTimeLeftSeconds > 0) {
      smoothedBookTimeLeftSeconds_ = readingStats_.estimatedTimeLeftSeconds;
    }
    seconds = smoothTimeLeftSeconds(smoothedBookTimeLeftSeconds_, seconds);
    smoothedBookTimeLeftSeconds_ = seconds;
  } else if (!estimateTimeLeftFromPages(remainingPages, secPerPage, seconds)) {
    snprintf(buf, len, "%s", tr(STR_TIME_LEFT_CALCULATING));
    return true;
  }

  const char* suffix = bookEstimate ? tr(STR_TIME_LEFT_IN_BOOK) : tr(STR_TIME_LEFT_IN_CHAPTER);
  if (seconds < 60) {
    snprintf(buf, len, "<1m %s", suffix);
  } else if (seconds < 3600) {
    snprintf(buf, len, "%lum %s", static_cast<unsigned long>((seconds + 30) / 60), suffix);
  } else {
    const unsigned long h = seconds / 3600;
    const unsigned long m = (seconds % 3600) / 60;
    if (m == 0)
      snprintf(buf, len, "%luh %s", h, suffix);
    else
      snprintf(buf, len, "%luh %lum %s", h, m, suffix);
  }
  return true;
}

void RivuletReaderActivity::renderStatusBar() const {
  if (!epub_ || !ready_) return;

  const int chapterPage = engine_.currentPage() + 1;
  const int chapterPageCount = std::max(chapterPage, engine_.chapterPageCount(&renderer));
  const float bookProgress = bookProgress01() * 100.0f;
  // Chapter "~" is meaningful: it clears once this chapter's page map completes.
  // Book "~" was hardcoded true, so it could never clear — a permanent marker
  // conveys nothing and is what reads as "the ~ never goes away". Book pages are
  // derived from byte progress and are always approximate; that is understood
  // without decorating every single frame with it.
  const bool pageCountEstimated = !engine_.mapComplete();
  constexpr bool bookPageEstimated = false;

  // Single call — same path as classic EpubReader. Do NOT also draw
  // drawSystemStatusBar / drawTopStatusBarClock (those are Home chrome and
  // overwrite or fight Manage Reader UI corner slots).
  const auto sb = SETTINGS.statusBarSpec();
  std::string bookTitle;
  std::string chapterTitle;
  if (sb.wantsBookTitle) {
    bookTitle = epub_->getTitle();
  }
  if (sb.wantsChapterTitle) {
    chapterTitle = tr(STR_UNNAMED);
    const int tocIndex = epub_->getTocIndexForSpineIndex(spineIndex_);
    if (tocIndex >= 0) {
      chapterTitle = epub_->getTocItem(tocIndex).title;
    }
  }

  int chapterIndex = 0;
  int chapterTotal = epub_->getTocItemsCount();
  if (chapterTotal > 0) {
    const int tocIndex = epub_->getTocIndexForSpineIndex(spineIndex_);
    if (tocIndex >= 0) {
      chapterIndex = tocIndex + 1;
    } else {
      chapterIndex = 1;
      for (int s = spineIndex_; s >= 0; --s) {
        const int t = epub_->getTocIndexForSpineIndex(s);
        if (t >= 0) {
          chapterIndex = t + 1;
          break;
        }
      }
    }
  } else {
    chapterTotal = std::max(1, epub_->getSpineItemsCount());
    chapterIndex = std::min(chapterTotal, std::max(1, spineIndex_ + 1));
  }

  // Whole-book page estimate from spine weights (same formula as EpubReader).
  int bookPage = 0;
  int bookPageCount = 0;
  if (epub_->getBookSize() > 0 && chapterPageCount > 0) {
    const float chapterStart = epub_->calculateProgress(spineIndex_, 0.0f);
    const float chapterEnd = epub_->calculateProgress(spineIndex_, 1.0f);
    const float chapterSpan = chapterEnd - chapterStart;
    if (chapterSpan > 0.001f) {
      const float pagesPerBookFrac = static_cast<float>(chapterPageCount) / chapterSpan;
      bookPageCount = std::max(1, static_cast<int>(pagesPerBookFrac + 0.5f));
      bookPage = std::max(1, std::min(bookPageCount,
                                     static_cast<int>(bookProgress01() * pagesPerBookFrac + 0.5f)));
    }
  }

  char timeLeftBook[48] = {};
  char timeLeftChapter[48] = {};
  const char* bookTl =
      (sb.wantsTimeLeftBook && formatTimeLeftLabel(timeLeftBook, sizeof(timeLeftBook), true)) ? timeLeftBook : nullptr;
  const char* chapTl =
      (sb.wantsTimeLeftChapter && formatTimeLeftLabel(timeLeftChapter, sizeof(timeLeftChapter), false))
          ? timeLeftChapter
          : nullptr;

  // Always pass titles when slots want them (already gated). Battery / clock /
  // progress bar / all six corners come from SETTINGS.statusBarSpec() inside
  // BaseTheme::drawStatusBar — same as classic.
  GUI.drawStatusBar(renderer, bookProgress, chapterPage, chapterPageCount, std::move(bookTitle),
                    /*paddingBottom=*/0, /*textYOffset=*/0, /*fillMargin=*/true,
                    /*isPageBookmarked=*/currentPageBookmarked_, pageCountEstimated, bookTl, chapTl,
                    /*drawTopBattery=*/true, bookPage, bookPageCount, bookPageEstimated, chapterIndex, chapterTotal,
                    std::move(chapterTitle));
}

bool RivuletReaderActivity::handleForcedRefresh() {
  {
    RenderLock lock(*this);
    pagesUntilFullRefresh_ = CasperSettings::REFRESH_COUNTDOWN_FORCE_SCRUB;
  }
  requestUpdate();
  return true;
}

ScreenshotInfo RivuletReaderActivity::getScreenshotInfo() const {
  ScreenshotInfo info;
  info.readerType = ScreenshotInfo::ReaderType::Epub;
  if (epub_) {
    const std::string t = epub_->getTitle();
    const size_t n = std::min(t.size(), sizeof(info.title) - 1);
    if (n > 0) {
      std::memcpy(info.title, t.data(), n);
    }
    info.title[n] = '\0';
  }
  info.spineIndex = spineIndex_;
  info.currentPage = engine_.currentPage() + 1;
  info.totalPages = std::max(info.currentPage, engine_.chapterPageCount(&renderer));
  info.progressPercent = static_cast<int>(bookProgress01() * 100.0f + 0.5f);
  return info;
}

void RivuletReaderActivity::render(RenderLock&& lock) {
  (void)lock;
  // 0xFF = white paper; clearScreen(false) is 0 = solid black (bug that ate all text).
  renderer.clearScreen(0xFF);

  // Prev-chapter walk yields; never paint mid-chapter body as if it were the end.
  if (chapterNavBusy_) {
    GUI.drawTopLeftStatus(renderer, tr(STR_LOADING_POPUP), /*refresh=*/false);
    ReaderUtils::displayWithDarkMode(renderer, HalDisplay::FAST_REFRESH);
    return;
  }

  if (error_ || !ready_) {
    const char* msg = error_ ? errorMsg_.c_str() : tr(STR_LOADING_POPUP);
    GUI.drawTopLeftStatus(renderer, msg, /*refresh=*/false);
    ReaderUtils::displayWithDarkMode(renderer, HalDisplay::FAST_REFRESH);
    return;
  }

  if (!engine_.ensureLaidOut(renderer)) {
    renderer.drawText(UI_10_FONT_ID, marginX_, marginY_ + 40, "Empty page", true, EpdFontFamily::REGULAR);
    ReaderUtils::displayWithDarkMode(renderer, HalDisplay::HALF_REFRESH);
    return;
  }

  const size_t nSpans = engine_.page().spans.size();
  const size_t nImgs = engine_.page().images.size();
  LOG_DBG("RVR", "paint spans=%u images=%u page=%d", static_cast<unsigned>(nSpans), static_cast<unsigned>(nImgs),
          engine_.currentPage());

  const uint32_t tPaint0 = millis();

  // Classic-style page glyph prewarm: scan → decompress needed glyphs into RAM
  // page buffers → real paint. Retain when heap allows so reverse/next turn and
  // drop-cap letters do not thrash SD fonts. Clear when tight so chapter convert
  // and menus keep maxAlloc.
  auto* fcm = renderer.getFontCacheManager();
  const int curPage = engine_.currentPage();
  const bool cacheWarm = fcm && glyphCacheSpine_ == spineIndex_ && glyphCachePage_ == curPage;
  const bool retainAfter = canRetainGlyphCache();
  if (fcm) {
    if (cacheWarm) {
      if (!retainAfter) {
        fcm->clearCache();
        glyphCacheSpine_ = -1;
        glyphCachePage_ = -1;
      }
    } else {
      // clearOnEnter: free prior page slots. clearOnExit false when retaining.
      auto scope = fcm->createPrewarmScope(/*clearOnEnter=*/true, /*clearOnExit=*/!retainAfter);
      engine_.paint(renderer, marginX_, marginY_);  // scan only (drawText records; no ink)
      scope.endScanAndPrewarm();
      if (retainAfter) {
        glyphCacheSpine_ = spineIndex_;
        glyphCachePage_ = curPage;
      } else {
        glyphCacheSpine_ = -1;
        glyphCachePage_ = -1;
      }
      // Scan may have touched FB for non-text; clear for real ink.
      renderer.clearScreen(0xFF);
    }
  }

  auto paintPageContent = [this]() {
    engine_.paint(renderer, marginX_, marginY_);
    paintPageImages();
    paintClippingHighlights();
    // Footnote markers: underline only (paint-time). Does not reflow Book's Style.
    paintFootnoteMarkers();
  };
  // Text + overlays only — used by the AA greyscale passes. See
  // paintTextLayerForAa for why images are excluded.
  auto paintTextForAa = [this]() { paintTextLayerForAa(); };
  // Reader-only dark: displayWithRefreshCycle inverts for the panel push only
  // (FB stays light paint-space so home / sleep never inherit inverted bits).

  bool preferFastFirst = false;
  bool deferAa = false;
  uint32_t openWallMs = 0;
  const bool hadOpenHints = ReaderActivity::takeOpenHints(preferFastFirst, deferAa, openWallMs);

  if (firstPaint_) {
    firstPaint_ = false;
    // Open scrub only when Home/QR passed open hints. firstPaint_ is also set
    // after chapter jumps / UI restore — those must stay on the normal FAST
    // countdown (never re-arm FORCE_SCRUB or Back feels like a full flash).
    if (hadOpenHints) {
      if (preferFastFirst) {
        // Cached open / clean BW home: FAST first ink (0.1.5 snappy open).
        if (pagesUntilFullRefresh_ == CasperSettings::REFRESH_COUNTDOWN_FORCE_SCRUB) {
          pagesUntilFullRefresh_ = SETTINGS.getRefreshFrequency();
        }
      } else {
        // Cold book.bin or greys residual: one HALF scrub on first ink only.
        pagesUntilFullRefresh_ = CasperSettings::REFRESH_COUNTDOWN_FORCE_SCRUB;
      }
    }
  }
  if (forceFastAfterChapterNav_) {
    forceFastAfterChapterNav_ = false;
    // Never HALF-scrub the first ink after a chapter hop (PageBack land was
    // refresh=3257ms on X3). Keep the interval countdown at 2+.
    if (pagesUntilFullRefresh_ <= 1) {
      const int freq = SETTINGS.getRefreshFrequency();
      pagesUntilFullRefresh_ = (freq > 1) ? freq : 2;
    }
  }

  // 2-bit fonts need greys multipass for smooth edges. Without it, AA fringes
  // either vanish (BW threshold) or look speckled (all fringes → black).
  //
  // Force scrub stays BW HALF: AA's gray nudge never consumed FORCE_SCRUB, so a
  // long-press refresh with AA on used to feel dead. Everything else that wants
  // AA goes through renderAntiAliased, which now pushes the BW page onto the
  // panel (displayGrayscaleBase) before the greys — that was the missing step
  // that made every AA-on turn look like "page did not load" until a scrub.
  constexpr size_t kAaPaintHeadroom = 12 * 1024;
  const bool forceScrub = (pagesUntilFullRefresh_ == CasperSettings::REFRESH_COUNTDOWN_FORCE_SCRUB);
  const bool heapOkForAa = renderer.canStoreBwBuffer(kAaPaintHeadroom);
  const bool aaWanted = SETTINGS.textAntiAliasing != 0 && !ReaderUtils::readerDarkModeEnabled();
  // Catch-up flag kept for a heap-recovery retry; it no longer has to paper over
  // a preferFast/defer skip, because those no longer decline AA.
  const bool aaCatchUp = forceAaThisRender_;
  forceAaThisRender_ = false;
  const bool aaThisFrame = aaWanted && heapOkForAa && !forceScrub;
  const char aaWhy = !aaWanted      ? 'o'   // off in settings (or dark mode)
                     : forceScrub   ? 's'   // long-press / interval scrub owns this frame
                     : !heapOkForAa ? 'h'   // storeBwBuffer would not fit
                     : aaCatchUp    ? 'c'   // heap-recovery catch-up
                                    : '-';  // ran (or will run)

  // BW glyph weight: Mild when AA is off (same default menus use), Normal when
  // AA is on so the greyscale multipass still has light fringe to shade.
  renderer.setBwGlyphWeight(aaWanted ? GfxRenderer::BwGlyphWeight::Normal
                                      : GfxRenderer::BwGlyphWeight::Mild);
  paintPageContent();
  renderStatusBar();

  const uint32_t tRefresh = millis();
  // The BW page is already painted into the framebuffer above. AA puts it on
  // glass (base) then enhances; if AA cannot run we must still push the BW frame
  // or the turn silently shows the previous page.
  bool aaRan = false;
  if (aaThisFrame) {
    // FAST base keeps open/turn snappy; the gray nudge is the "smooths a moment
    // later" the user remembers. Half scrub stays on the non-AA path above.
    aaRan = ReaderUtils::renderAntiAliased(
        renderer, [&]() { paintTextForAa(); }, HalDisplay::FAST_REFRESH);
    if (aaRan) {
      const int freq = SETTINGS.getRefreshFrequency();
      if (freq != CasperSettings::REFRESH_COUNTDOWN_DISABLED && pagesUntilFullRefresh_ > 1) {
        pagesUntilFullRefresh_--;
      }
    }
  }
  if (!aaRan) {
    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh_);
  }

  // Heap refused AA this frame: ask for a recovery pass once the page is on
  // glass in BW. PreferFast/defer no longer need this — they go through AA.
  if (aaRan) {
    aaCatchUpPending_ = false;
    aaCatchUpTries_ = 0;
  } else if (aaWanted && !forceScrub && !heapOkForAa) {
    scheduleAaCatchUp();
  } else {
    aaCatchUpPending_ = false;
  }
  if (hadOpenHints || preferFastFirst) {
    LOG_INF("RVR", "first_ink refresh=%lums preferFast=%d aa=%d wall=%lums",
            static_cast<unsigned long>(millis() - tRefresh), preferFastFirst ? 1 : 0, aaRan ? 1 : 0,
            openWallMs != 0 ? static_cast<unsigned long>(millis() - openWallMs) : 0UL);
  }

  // The Rivulet reader had no render-path entry in /.casper-logs at all — the old
  // PAGE/ERS lines belonged to the classic reader and went away with it. That is
  // why a report of "pages do not load" could not be checked against a capture.
  // One line per painted page: which page, whether AA ran, and the heap it ran on.
  SystemLog::logTiming("PAGE",
                       "spine=%d page=%d/%d map=%d/%s aa=%d ran=%d why=%c paint=%lums refresh=%lums fre=%u maxA=%u",
                       spineIndex_, engine_.currentPage() + 1, std::max(1, engine_.chapterPageCount(nullptr)),
                       engine_.mapKnownPages(), engine_.mapComplete() ? "done" : "est", aaThisFrame ? 1 : 0,
                       aaRan ? 1 : 0, aaWhy, static_cast<unsigned long>(tRefresh - tPaint0),
                       static_cast<unsigned long>(millis() - tRefresh), static_cast<unsigned>(ESP.getFreeHeap()),
                       static_cast<unsigned>(ESP.getMaxAllocHeap()));

  // Layout geometry: how much of the viewport the page actually used, and whether
  // one more body line would have fitted. slack >= bodyLine means we are leaving a
  // usable line on the table (reserve too generous or a block margin blocked it).
  {
    const auto& rk = engine_.renderKey();
    const int bodyLine = std::max(1, renderer.getLineHeight(rk.fontId, SETTINGS.getReaderLineCompression()));
    const int contentH = engine_.page().contentH;
    const int slack = static_cast<int>(rk.viewportH) - contentH;
    SystemLog::logTiming("FIT", "vpH=%u used=%d slack=%d bodyLine=%d spare_lines=%d mT=%d mB=%d statusH=%d menuF=%u",
                         static_cast<unsigned>(rk.viewportH), contentH, slack, bodyLine,
                         bodyLine > 0 ? slack / bodyLine : 0, marginY_, marginB_,
                         UITheme::getInstance().getStatusBarHeight(),
                         static_cast<unsigned>(SETTINGS.menuFontSize));
  }

  // Page is on glass — adjacent-chapter indexing may now run on the idle tick.
  firstInkDone_ = true;

  // After glass has the page: stats + path + recents (never on critical open).
  if (pendingStatsLoad_) {
    pendingStatsLoad_ = false;
    readingStats_ = CasperStats::loadBook(epub_->getPath());
    if (SETTINGS.readingStatsTrackingEnabled()) {
      globalReadingStats_ = GlobalReadingStats::load();
    }
  }
  if (pendingOpenStateSave_) {
    pendingOpenStateSave_ = false;
    APP_STATE.saveToFile();
  }
  if (pendingRecentsTouch_ && epub_) {
    pendingRecentsTouch_ = false;
    RECENT_BOOKS.addBook(epub_->getPath(), epub_->getTitle(), epub_->getAuthor(), epub_->getThumbBmpPath());
  }

  if (pendingScreenshot_) {
    pendingScreenshot_ = false;
    ScreenshotUtil::takeScreenshot(renderer);
  }
}
