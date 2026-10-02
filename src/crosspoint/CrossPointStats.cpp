#include "crosspoint/CrossPointStats.h"

#include <Logging.h>

#include "RecentBooksStore.h"
#include "util/CrossPointBookStore.h"

namespace CrossPointStats {
namespace {

uint16_t milliFromPercent(float percent) {
  if (percent < 0.0f) return 0xFFFF;
  if (percent > 100.0f) percent = 100.0f;
  uint16_t m = static_cast<uint16_t>(percent * 100.0f + 0.5f);
  if (m > 10000) m = 10000;
  return m;
}

float percentFromMilli(uint16_t milli) {
  if (milli == 0xFFFF) return -1.0f;
  return static_cast<float>(milli) / 100.0f;
}

}  // namespace

std::string bookDir(const std::string& bookFilePath) {
  if (bookFilePath.empty()) return {};
  return CrossPointBook::bookDirForPath(bookFilePath);
}

BookReadingStats loadBook(const std::string& bookFilePath) {
  const std::string dir = bookDir(bookFilePath);
  if (dir.empty()) return {};
  // Single directory: /.crosspoint/epub_<std::hash>/ (or xtc_/txt_).
  return BookReadingStats::load(dir);
}

void saveBook(const std::string& bookFilePath, const BookReadingStats& stats) {
  const std::string dir = bookDir(bookFilePath);
  if (dir.empty()) {
    LOG_ERR("CSTATS", "saveBook: no book dir for path");
    return;
  }
  stats.save(dir);
  setHomeProgress(bookFilePath, stats.getProgressPercent());
}

float homeProgressPercent(const std::string& bookFilePath) {
  if (bookFilePath.empty()) return -1.0f;

  RECENT_BOOKS.ensureLoadedPublic();
  for (const auto& b : RECENT_BOOKS.getBooks()) {
    if (b.path == bookFilePath && b.progressPercentMilli != 0xFFFF) {
      return percentFromMilli(b.progressPercentMilli);
    }
  }

  const BookReadingStats s = loadBook(bookFilePath);
  const float pct = s.getProgressPercent();
  if (pct >= 0.0f) {
    // Warm recent.json so next Home paint is free.
    setHomeProgress(bookFilePath, pct);
  }
  return pct;
}

void setHomeProgress(const std::string& bookFilePath, float percent0to100) {
  if (bookFilePath.empty()) return;
  RECENT_BOOKS.ensureLoadedPublic();
  const uint16_t milli = milliFromPercent(percent0to100);
  if (RECENT_BOOKS.setProgressMilli(bookFilePath, milli)) {
    (void)RECENT_BOOKS.saveToFile();
  }
}

bool removeBook(const std::string& bookFilePath) {
  const std::string dir = bookDir(bookFilePath);
  if (dir.empty()) return true;
  return BookReadingStats::remove(dir);
}

}  // namespace CrossPointStats
