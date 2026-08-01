#include "BookActions.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Xtc.h>

#include "ClippingStore.h"
#include "CrossPointSettings.h"
#include "RecentBooksStore.h"
#include "activities/reader/BookReadingStats.h"
#include "activities/reader/GlobalReadingStats.h"
#include "fontIds.h"
#include "util/BookCacheUtils.h"
#include "util/UiGhostPolicy.h"

namespace BookActions {
namespace {

bool hasReadingStats(const std::string& path) {
  return FsHelpers::hasEpubExtension(path) || FsHelpers::hasXtcExtension(path);
}

std::string bookStatsCachePath(const std::string& path) { return BookReadingStats::cachePathForBook(path); }

}  // namespace

std::vector<FileBrowserActionActivity::MenuItem> buildBookActionItems(const std::string& fullPath,
                                                                      const bool includeRemoveFromRecents) {
  // Long-press: Read, Synopsis (EPUB), Reset Pace, …
  // (Per-book Reading Stats entry was Stats-theme only; use Menu → Reading Stats.)
  std::vector<FileBrowserActionActivity::MenuItem> items;
  items.reserve(9);
  items.push_back({FileBrowserAction::Open, StrId::STR_READ});
  if (FsHelpers::hasEpubExtension(fullPath)) {
    items.push_back({FileBrowserAction::Description, StrId::STR_SYNOPSIS});
  }
  const bool statsOk = SETTINGS.readingStatsTrackingEnabled();
  if (statsOk && hasReadingStats(fullPath)) {
    items.push_back({FileBrowserAction::ResetPace, StrId::STR_RESET_READING_PACE});
  }
  if (includeRemoveFromRecents) {
    items.push_back({FileBrowserAction::RemoveFromRecents, StrId::STR_REMOVE_FROM_RECENTS});
  }
  // Mark finished is not session tracking — used for finished folder / recents rules.
  if (hasReadingStats(fullPath)) {
    items.push_back({FileBrowserAction::ToggleCompleted,
                     isBookCompleted(fullPath) ? StrId::STR_MARK_UNFINISHED : StrId::STR_MARK_FINISHED});
  }
  items.push_back({FileBrowserAction::Delete, StrId::STR_DELETE});
  if (statsOk && hasReadingStats(fullPath)) {
    items.push_back({FileBrowserAction::DeleteStats, StrId::STR_DELETE_BOOK_STATS});
  }
  if (hasClearableBookCache(fullPath)) {
    items.push_back({FileBrowserAction::DeleteCache, StrId::STR_DELETE_CACHE});
  }
  return items;
}

bool hasClearableBookCache(const std::string& path) {
  return FsHelpers::hasEpubExtension(path) || FsHelpers::hasXtcExtension(path) || FsHelpers::hasTxtExtension(path);
}

void clearFileMetadata(const std::string& fullPath) {
  // Drop reading cache + stats when deleting a book from SD.
  ::clearBookCache(fullPath);
  // Wipe stats under Casper and CrossInk cache hashes (if both exist).
  BookReadingStats::removeForBook(fullPath);
  if (FsHelpers::hasEpubExtension(fullPath)) {
    ClippingStore::deleteForFilePath(fullPath, "epub");
  }
  LOG_DBG("BookActions", "Cleared metadata for: %s", fullPath.c_str());
}

bool clearBookCache(const std::string& fullPath) {
  if (!hasClearableBookCache(fullPath)) {
    return false;
  }
  ::clearBookCache(fullPath);
  return true;
}

bool deleteBookStats(const std::string& fullPath) {
  const std::string cachePath = bookStatsCachePath(fullPath);
  if (cachePath.empty()) {
    return false;
  }
  return BookReadingStats::removeForBook(fullPath);
}

bool resetReadingPace(const std::string& fullPath) {
  const std::string cachePath = bookStatsCachePath(fullPath);
  if (cachePath.empty()) {
    return false;
  }
  // loadForBook picks up CrossInk FNV stats if present, then we rewrite Casper path.
  BookReadingStats stats = BookReadingStats::loadForBook(fullPath);
  stats.avgSecondsPerForwardPage = 0;
  stats.paceSampleCount = 0;
  stats.estimatedTimeLeftSeconds = 0;
  stats.save(cachePath);
  LOG_DBG("BookActions", "Reset reading pace for: %s", fullPath.c_str());
  return true;
}

std::string confirmationHeading(const StrId actionLabelId) {
  return std::string(tr(STR_CONFIRM)) + ": " + std::string(I18N.get(actionLabelId));
}

bool isBookCompleted(const std::string& fullPath) {
  const std::string cachePath = bookStatsCachePath(fullPath);
  return !cachePath.empty() && BookReadingStats::load(cachePath).isCompleted;
}

bool toggleBookCompleted(const std::string& fullPath, const std::string& displayName, bool& completed) {
  const bool isEpub = FsHelpers::hasEpubExtension(fullPath);
  const bool isXtc = FsHelpers::hasXtcExtension(fullPath);
  if (!isEpub && !isXtc) {
    return false;
  }

  Epub epub(fullPath, "/.crosspoint");
  Xtc xtc(fullPath, "/.crosspoint");
  std::string cachePath;
  std::string title = displayName;
  std::string author;
  std::string thumbPath;
  if (isEpub) {
    epub.setupCacheDir();
    cachePath = epub.getCachePath();
    title = epub.getTitle();
    author = epub.getAuthor();
    thumbPath = epub.getThumbBmpPath();
  } else {
    if (!xtc.load()) {
      return false;
    }
    xtc.setupCacheDir();
    cachePath = xtc.getCachePath();
    title = xtc.getTitle();
    author = xtc.getAuthor();
    thumbPath = xtc.getThumbBmpPath();
  }

  BookReadingStats stats = BookReadingStats::load(cachePath);
  const bool wasCompleted = stats.isCompleted;
  stats.isCompleted = !wasCompleted;
  completed = stats.isCompleted;
  // Finished date is tracking metadata only.
  if (completed && SETTINGS.readingStatsTrackingEnabled() && !stats.finishedDateManual) {
    ReadingStatsDateTime now;
    if (getCurrentLocalReadingStatsDateTime(now)) {
      stats.finishedDate = now.date;
    }
  }
  stats.save(cachePath);

  if (completed) {
    if (SETTINGS.removeReadBooksFromRecents) {
      RECENT_BOOKS.removeByPath(fullPath);
    }
  } else {
    if (SETTINGS.removeReadBooksFromRecents) {
      RECENT_BOOKS.addBook(fullPath, title, author, thumbPath);
    }
  }

  // Lifetime completed-book count only when tracking is on.
  if (SETTINGS.readingStatsTrackingEnabled()) {
    GlobalReadingStats globalStats = GlobalReadingStats::load();
    if (completed) {
      if (globalStats.completedBooks < UINT32_MAX) {
        globalStats.completedBooks++;
      }
    } else if (globalStats.completedBooks > 0) {
      globalStats.completedBooks--;
    }
    globalStats.save();
  }
  return true;
}

void drawToast(const GfxRenderer& renderer, const char* msg) {
  const auto pageHeight = renderer.getScreenHeight();
  renderer.clearScreen();
  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 10, msg, true);
  UiGhostPolicy::displayMenuFrame(renderer);
}

std::string loadBookDescription(const std::string& fullPath) {
  if (!FsHelpers::hasEpubExtension(fullPath)) {
    return {};
  }
  Epub epub(fullPath, "/.crosspoint");
  return epub.getDescription();
}

}  // namespace BookActions
