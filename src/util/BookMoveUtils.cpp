#include "BookMoveUtils.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Txt.h>
#include <Xtc.h>

#include "BookmarkStore.h"
#include "ClippingStore.h"
#include "CrossPointState.h"
#include "RecentBooksStore.h"

namespace {
constexpr char READ_FOLDER[] = "/Read";

std::string bookTypeForPath(const std::string& path) {
  if (FsHelpers::hasEpubExtension(path)) return "epub";
  if (FsHelpers::hasXtcExtension(path)) return "xtc";
  if (FsHelpers::hasTxtExtension(path)) return "txt";
  return "";
}

std::string cachePathForBook(const std::string& path) {
  if (FsHelpers::hasEpubExtension(path)) {
    return Epub::cachePathForFilePath(path, "/.crosspoint");
  }
  if (FsHelpers::hasXtcExtension(path)) {
    return Xtc(path, "/.crosspoint").getCachePath();
  }
  if (FsHelpers::hasTxtExtension(path)) {
    return Txt(path, "/.crosspoint").getCachePath();
  }
  return "";
}

// Prefer known recents title/author so bookmark migration keeps metadata.
void titleAuthorFromRecents(const std::string& path, std::string& title, std::string& author) {
  title.clear();
  author.clear();
  for (const auto& book : RECENT_BOOKS.getBooks()) {
    if (book.path == path) {
      title = book.title;
      author = book.author;
      return;
    }
  }
}
}  // namespace

namespace BookMoveUtils {

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

bool migrateMovedEpubState(const std::string& oldPath, const std::string& newPath, const std::string& oldCachePath,
                           const std::string& title, const std::string& author, const bool keepInRecents) {
  bool ok = true;

  const std::string bookType = bookTypeForPath(newPath.empty() ? oldPath : newPath);
  const std::string newCachePath = cachePathForBook(newPath);

  if (!oldCachePath.empty() && !newCachePath.empty() && oldCachePath != newCachePath &&
      Storage.exists(oldCachePath.c_str())) {
    if (Storage.exists(newCachePath.c_str())) {
      // Rare: destination cache already exists. Prefer keeping destination, drop source.
      LOG_ERR("BookMove", "Destination cache already exists, leaving %s in place", newCachePath.c_str());
      ok = false;
    } else if (!Storage.rename(oldCachePath.c_str(), newCachePath.c_str())) {
      LOG_ERR("BookMove", "Failed to rename cache dir %s -> %s (non-fatal)", oldCachePath.c_str(),
              newCachePath.c_str());
      ok = false;
    } else {
      LOG_INF("BookMove", "Migrated cache %s -> %s", oldCachePath.c_str(), newCachePath.c_str());
    }
  }

  if (!bookType.empty()) {
    if (!BookmarkStore::migrateForFilePath(oldPath, newPath, title, author, bookType)) {
      LOG_ERR("BookMove", "Failed to migrate bookmarks for moved book %s -> %s", oldPath.c_str(), newPath.c_str());
      ok = false;
    }

    if (!ClippingStore::migrateForFilePath(oldPath, newPath, title, author, bookType)) {
      LOG_ERR("BookMove", "Failed to migrate clippings for moved book %s -> %s", oldPath.c_str(), newPath.c_str());
      ok = false;
    }
  }

  if (keepInRecents) {
    RECENT_BOOKS.updatePath(oldPath, newPath, oldCachePath, newCachePath);
  } else {
    RECENT_BOOKS.removeByPath(oldPath);
    RECENT_BOOKS.removeByPath(newPath);
  }

  if (APP_STATE.openEpubPath == oldPath) {
    APP_STATE.openEpubPath = newPath;
    APP_STATE.saveToFile();
  }

  return ok;
}

bool migrateRenamedBook(const std::string& oldPath, const std::string& newPath, const bool keepInRecents) {
  if (oldPath.empty() || newPath.empty() || oldPath == newPath) {
    return true;
  }
  const std::string bookType = bookTypeForPath(oldPath);
  if (bookType.empty() || bookTypeForPath(newPath).empty()) {
    // Non-book or type change — nothing to migrate for stats/cache.
    return true;
  }

  const std::string oldCachePath = cachePathForBook(oldPath);
  std::string title;
  std::string author;
  titleAuthorFromRecents(oldPath, title, author);
  if (title.empty()) {
    titleAuthorFromRecents(newPath, title, author);
  }

  return migrateMovedEpubState(oldPath, newPath, oldCachePath, title, author, keepInRecents);
}

}  // namespace BookMoveUtils
