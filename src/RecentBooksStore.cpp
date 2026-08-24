#include "RecentBooksStore.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Xtc.h>

#include <algorithm>
#include <iterator>

#include "util/CrossPointPaths.h"

bool RecentBooksStore::parseBooksArray(JsonVariantConst doc, std::vector<RecentBook>& out) {
  out.clear();
  JsonArrayConst arr = doc["books"].as<JsonArrayConst>();
  out.reserve(std::min(arr.size(), static_cast<size_t>(MAX_RECENT_BOOKS)));
  for (JsonObjectConst obj : arr) {
    if (out.size() >= static_cast<size_t>(MAX_RECENT_BOOKS)) break;
    RecentBook book;
    book.path = obj["path"] | "";
    book.title = obj["title"] | "";
    book.author = obj["author"] | "";
    book.coverBmpPath = obj["coverBmpPath"] | "";
    // CrossPoint progress field (missing → unknown).
    if (obj["progressMilli"].is<int>() || obj["progressMilli"].is<unsigned int>()) {
      const int m = obj["progressMilli"] | -1;
      book.progressPercentMilli = (m >= 0 && m <= 10000) ? static_cast<uint16_t>(m) : 0xFFFF;
    } else {
      book.progressPercentMilli = 0xFFFF;
    }
    if (book.path.empty()) continue;
    out.push_back(std::move(book));
  }
  return true;
}

void RecentBooksStore::toJson(JsonDocument& doc) const {
  JsonArray arr = doc["books"].to<JsonArray>();
  for (const auto& book : recentBooks) {
    JsonObject obj = arr.add<JsonObject>();
    obj["path"] = book.path;
    obj["title"] = book.title;
    obj["author"] = book.author;
    obj["coverBmpPath"] = book.coverBmpPath;
    if (book.progressPercentMilli != 0xFFFF) {
      obj["progressMilli"] = book.progressPercentMilli;
    }
  }
}

bool RecentBooksStore::fromJson(JsonVariantConst doc) {
  loadedOnce_ = true;

  std::vector<RecentBook> loaded;
  parseBooksArray(doc, loaded);

  // Never replace a non-empty list with an empty parse (truncated / half-write).
  if (loaded.empty() && !recentBooks.empty()) {
    LOG_ERR("RBS", "Refusing to load empty recent.json over %d in-memory entries", getCount());
    return true;
  }

  // Never shrink an in-memory list with a shorter disk snapshot unless memory
  // was empty (cold load). QR used to addBook before load and save 1 book;
  // a later loadFromFile of that short file must not be the only history —
  // but if memory already has more (e.g. user session), keep the richer set
  // and merge any disk paths we do not have.
  if (!loaded.empty() && !recentBooks.empty() && loaded.size() < recentBooks.size()) {
    LOG_ERR("RBS", "disk has %u books, memory has %u — merging disk into memory (no shrink)",
            static_cast<unsigned>(loaded.size()), static_cast<unsigned>(recentBooks.size()));
    for (const auto& d : loaded) {
      const bool have =
          std::any_of(recentBooks.begin(), recentBooks.end(), [&](const RecentBook& b) { return b.path == d.path; });
      if (!have && recentBooks.size() < static_cast<size_t>(MAX_RECENT_BOOKS)) {
        recentBooks.push_back(d);
      }
    }
    return true;
  }

  recentBooks.swap(loaded);
  LOG_INF("RBS", "Recent books loaded (%d entries)", getCount());
  return true;
}

void RecentBooksStore::ensureLoaded() {
  if (loadedOnce_) return;
  // loadFromFile calls fromJson (sets loadedOnce_) on success. Missing file
  // returns false without fromJson — still mark loaded so we do not spin.
  if (!loadFromFile()) {
    loadedOnce_ = true;
    LOG_DBG("RBS", "ensureLoaded: no recent.json yet (empty list)");
  }
}

void RecentBooksStore::mergeMissingFromDisk() {
  // Merge CrossPoint + legacy foreign-root recent only before v2 migrate completes.
  auto mergeFile = [this](const char* path) -> size_t {
    JsonDocument doc;
    if (!PersistableStoreBase::readDocFromFile(path, doc)) return 0;
    std::vector<RecentBook> disk;
    parseBooksArray(doc.as<JsonVariantConst>(), disk);
    size_t added = 0;
    for (const auto& d : disk) {
      const bool have =
          std::any_of(recentBooks.begin(), recentBooks.end(), [&](const RecentBook& b) { return b.path == d.path; });
      if (!have && recentBooks.size() < static_cast<size_t>(MAX_RECENT_BOOKS)) {
        recentBooks.push_back(d);
        ++added;
      }
    }
    return added;
  };

  const size_t a = mergeFile(getFilePath());
  if (a > 0) {
    LOG_INF("RBS", "merged %u book(s) from /.crosspoint (total %d)", static_cast<unsigned>(a), getCount());
  }
}

void RecentBooksStore::addBook(const std::string& path, const std::string& title, const std::string& author,
                               const std::string& coverBmpPath) {
  // Critical: load full history before mutating. Quick Resume boot defers
  // loadFromFile until after first ink; without this, addBook saved a 1-entry
  // list over a full recent.json and wiped the library.
  ensureLoaded();
  mergeMissingFromDisk();

  // Remove existing entry if present (path match). Keep its Home progress —
  // a 4-field insert used to default progressPercentMilli to unknown and the
  // Recents bars went blank until the next stats save.
  uint16_t keptProgress = 0xFFFF;
  auto it =
      std::find_if(recentBooks.begin(), recentBooks.end(), [&](const RecentBook& book) { return book.path == path; });
  if (it != recentBooks.end()) {
    keptProgress = it->progressPercentMilli;
    recentBooks.erase(it);
  }

  recentBooks.insert(recentBooks.begin(), {path, title, author, coverBmpPath, keptProgress});

  // Trim to max size
  if (recentBooks.size() > MAX_RECENT_BOOKS) {
    recentBooks.resize(MAX_RECENT_BOOKS);
  }

  if (!saveToFile()) {
    LOG_ERR("RBS", "Failed to persist recent book add: %s (count=%d)", path.c_str(), getCount());
  } else {
    LOG_INF("RBS", "Added recent (%d total): %s", getCount(), path.c_str());
  }
}

void RecentBooksStore::updateBook(const std::string& path, const std::string& title, const std::string& author,
                                  const std::string& coverBmpPath) {
  ensureLoaded();
  auto it =
      std::find_if(recentBooks.begin(), recentBooks.end(), [&](const RecentBook& book) { return book.path == path; });
  if (it != recentBooks.end()) {
    RecentBook& book = *it;
    book.title = title;
    book.author = author;
    book.coverBmpPath = coverBmpPath;
    saveToFile();
  }
}

bool RecentBooksStore::removeByPath(const std::string& path) {
  ensureLoaded();
  auto it =
      std::find_if(recentBooks.begin(), recentBooks.end(), [&](const RecentBook& book) { return book.path == path; });
  if (it == recentBooks.end()) {
    return false;
  }
  recentBooks.erase(it);
  if (!saveToFile()) {
    LOG_ERR("RBS", "Failed to persist removal of recent book: %s", path.c_str());
  }
  return true;
}

void RecentBooksStore::updatePath(const std::string& oldPath, const std::string& newPath,
                                  const std::string& oldCachePath, const std::string& newCachePath) {
  ensureLoaded();
  auto it = std::find_if(recentBooks.begin(), recentBooks.end(),
                         [&](const RecentBook& book) { return book.path == oldPath; });
  if (it == recentBooks.end()) {
    return;
  }
  it->path = newPath;
  if (!oldCachePath.empty() && !it->coverBmpPath.empty() && it->coverBmpPath.rfind(oldCachePath, 0) == 0) {
    it->coverBmpPath = newCachePath + it->coverBmpPath.substr(oldCachePath.size());
  }
  saveToFile();
}

bool RecentBooksStore::isMissing(const RecentBook& book) { return !Storage.exists(book.path.c_str()); }

bool RecentBooksStore::pruneMissing() {
  ensureLoaded();
  const size_t before = recentBooks.size();
  if (before == 0) {
    return false;
  }

  // Never wipe the whole list on a single exists() pass. After flash / SD settle
  // glitches, Storage.exists can fail for every path and we used to save an empty
  // recent.json — permanently erasing the user's library of recents.
  std::vector<RecentBook> kept;
  kept.reserve(before);
  size_t missing = 0;
  for (const auto& book : recentBooks) {
    if (isMissing(book)) {
      ++missing;
    } else {
      kept.push_back(book);
    }
  }
  if (missing == 0) {
    return false;
  }
  if (kept.empty()) {
    LOG_ERR("RBS",
            "Refusing to prune all %u recent books (exists failed for every path) — "
            "keeping list; check SD / paths",
            static_cast<unsigned>(before));
    return false;
  }

  recentBooks.swap(kept);
  LOG_DBG("RBS", "Pruned %u missing recent(s); %u remain", static_cast<unsigned>(missing),
          static_cast<unsigned>(recentBooks.size()));
  return true;
}

RecentBook RecentBooksStore::getDataFromBook(std::string path) const {
  std::string lastBookFileName = "";
  const size_t lastSlash = path.find_last_of('/');
  if (lastSlash != std::string::npos) {
    lastBookFileName = path.substr(lastSlash + 1);
  }

  LOG_DBG("RBS", "Loading recent book: %s", path.c_str());

  // If epub, try to load the metadata for title/author and cover.
  // Use buildIfMissing=false to avoid heavy epub loading on boot; getTitle()/getAuthor() may be
  // blank until the book is opened, and entries with missing title are omitted from recent list.
  if (FsHelpers::hasEpubExtension(lastBookFileName)) {
    Epub epub(path, CrossPointPaths::kPackageCacheRoot);
    epub.load(false, true);
    return RecentBook{path, epub.getTitle(), epub.getAuthor(), epub.getThumbBmpPath()};
  } else if (FsHelpers::hasXtcExtension(lastBookFileName)) {
    // Handle XTC file
    Xtc xtc(path, CrossPointPaths::kPackageCacheRoot);
    if (xtc.load()) {
      return RecentBook{path, xtc.getTitle(), xtc.getAuthor(), xtc.getThumbBmpPath()};
    }
  } else if (FsHelpers::hasTxtExtension(lastBookFileName) || FsHelpers::hasMarkdownExtension(lastBookFileName)) {
    return RecentBook{path, lastBookFileName, "", ""};
  }
  return RecentBook{path, "", "", ""};
}

bool RecentBooksStore::setProgressMilli(const std::string& path, const uint16_t progressPercentMilli) {
  ensureLoaded();
  for (auto& b : recentBooks) {
    if (b.path == path) {
      if (b.progressPercentMilli == progressPercentMilli) return false;
      b.progressPercentMilli = progressPercentMilli;
      return true;
    }
  }
  return false;
}
