#include "BookCacheUtils.h"

#include <BookPathId.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Txt.h>
#include <Xtc.h>

#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "util/CasperPaths.h"

bool wipeCacheDirectory(const std::string& path);

namespace {

constexpr size_t MAX_STATS_FILES_TO_PRESERVE = 16;
constexpr char STATS_PREFIX[] = "stats";
constexpr char STATS_SUFFIX[] = ".bin";
constexpr char TRASH_DIR[] = ".trash";

// Fixed user-state files that must survive cache clear (legacy parity).
struct FixedPreserve {
  const char* name;
  const char* tmpName;
};
constexpr FixedPreserve kFixedPreserve[] = {
    {"progress.bin", "clear_preserve_progress.bin"},
    {"progress.bin.bak", "clear_preserve_progress.bin.bak"},
    {"reader_settings.bin", "clear_preserve_reader_settings.bin"},
    {"dictionary_history.txt", "clear_preserve_dictionary_history.txt"},
    {"meta.txt", "clear_preserve_meta.txt"},
};

bool isStatsFileName(const char* name) {
  if (!name) return false;
  const size_t nameLen = strlen(name);
  constexpr size_t prefixLen = sizeof(STATS_PREFIX) - 1;
  constexpr size_t suffixLen = sizeof(STATS_SUFFIX) - 1;
  return nameLen >= prefixLen + suffixLen && strncmp(name, STATS_PREFIX, prefixLen) == 0 &&
         strcmp(name + nameLen - suffixLen, STATS_SUFFIX) == 0;
}

struct PreservedFile {
  std::string name;
  std::string tmpName;
};

bool preserveFile(const std::string& cachePath, const PreservedFile& file, bool& moved) {
  moved = false;
  const std::string sourcePath = cachePath + "/" + file.name;
  const std::string tmpPath = cachePath + "." + file.tmpName;
  if (Storage.exists(tmpPath.c_str()) && !Storage.remove(tmpPath.c_str())) {
    LOG_ERR("BookCache", "Failed to remove stale preserve temp: %s", tmpPath.c_str());
    return false;
  }
  if (!Storage.exists(sourcePath.c_str())) return true;
  if (!Storage.rename(sourcePath.c_str(), tmpPath.c_str())) {
    LOG_ERR("BookCache", "Failed to preserve: %s", sourcePath.c_str());
    return false;
  }
  moved = true;
  return true;
}

bool restoreFile(const std::string& cachePath, const PreservedFile& file, const bool moved) {
  if (!moved) return true;
  const std::string tmpPath = cachePath + "." + file.tmpName;
  if (!Storage.exists(tmpPath.c_str())) return true;
  Storage.mkdir(cachePath.c_str());
  const size_t slash = file.name.find_last_of('/');
  if (slash != std::string::npos) {
    const std::string parent = cachePath + "/" + file.name.substr(0, slash);
    Storage.ensureDirectoryExists(parent.c_str());
  }
  const std::string finalPath = cachePath + "/" + file.name;
  if (Storage.exists(finalPath.c_str())) {
    Storage.remove(finalPath.c_str());
  }
  if (!Storage.rename(tmpPath.c_str(), finalPath.c_str())) {
    LOG_ERR("BookCache", "Failed to restore: %s", finalPath.c_str());
    return false;
  }
  return true;
}

void collectPreservedFiles(const std::string& cachePath, std::vector<PreservedFile>& out) {
  out.clear();
  for (const auto& f : kFixedPreserve) {
    out.push_back({f.name, f.tmpName});
  }

  auto dir = Storage.open(cachePath.c_str());
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return;
  }

  size_t statsCount = 0;
  char name[96];
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    const bool isDir = file.isDirectory();
    file.getName(name, sizeof(name));
    file.close();
    if (isDir || !isStatsFileName(name)) continue;
    if (statsCount >= MAX_STATS_FILES_TO_PRESERVE) continue;
    out.push_back({name, std::string("clear_preserve_") + name});
    ++statsCount;
  }
  dir.close();

  // Recover Stats lives in <cache>/.trash/ — keep it across Delete Book Cache.
  const std::string trashPath = cachePath + "/" + TRASH_DIR;
  auto trash = Storage.open(trashPath.c_str());
  if (!trash || !trash.isDirectory()) {
    if (trash) trash.close();
    return;
  }
  size_t trashCount = 0;
  for (auto file = trash.openNextFile(); file; file = trash.openNextFile()) {
    const bool isDir = file.isDirectory();
    file.getName(name, sizeof(name));
    file.close();
    if (isDir || !isStatsFileName(name)) continue;
    if (trashCount >= MAX_STATS_FILES_TO_PRESERVE) continue;
    out.push_back({std::string(TRASH_DIR) + "/" + name, std::string("clear_preserve_trash_") + name});
    ++trashCount;
  }
  trash.close();
}

bool wipeDirBestEffort(const std::string& path) {
  if (path.empty() || !Storage.exists(path.c_str())) return true;
  if (!wipeCacheDirectory(path)) {
    LOG_ERR("BookCache", "Failed to remove: %s", path.c_str());
    return false;
  }
  return true;
}

// Unified ownership: /.crosspoint/book_<id>/ — wipe package + rivulet, keep progress/stats.
bool clearBookOwnershipDir(const std::string& bookRoot) {
  if (bookRoot.empty()) return false;
  if (!Storage.exists(bookRoot.c_str())) {
    LOG_DBG("BookCache", "Book dir does not exist: %s", bookRoot.c_str());
    return true;
  }

  std::vector<PreservedFile> files;
  collectPreservedFiles(bookRoot, files);
  std::vector<bool> moved(files.size(), false);

  bool preserveOk = true;
  for (size_t i = 0; i < files.size(); ++i) {
    bool m = false;
    if (!preserveFile(bookRoot, files[i], m)) preserveOk = false;
    moved[i] = m;
  }
  if (!preserveOk) {
    for (size_t i = 0; i < files.size(); ++i) {
      (void)restoreFile(bookRoot, files[i], moved[i]);
    }
    LOG_ERR("BookCache", "Aborted ownership clear; could not preserve state: %s", bookRoot.c_str());
    return false;
  }

  // Derived caches only — not the book_ shell (progress restored below).
  const bool pkgOk = wipeDirBestEffort(bookRoot + "/package");
  const bool rivOk = wipeDirBestEffort(bookRoot + "/rivulet");

  // Also drop any loose derived files at book root (not in preserve list).
  // removeDir of whole book_ would be simpler but risks losing unlisted state;
  // package + rivulet cover book.bin / IR / HTML / thumbs.

  bool restoreOk = true;
  for (size_t i = 0; i < files.size(); ++i) {
    if (!restoreFile(bookRoot, files[i], moved[i])) restoreOk = false;
  }

  LOG_INF("BookCache", "cleared book ownership pkg=%d rivulet=%d restore=%d path=%s", pkgOk ? 1 : 0, rivOk ? 1 : 0,
          restoreOk ? 1 : 0, bookRoot.c_str());
  return pkgOk && rivOk && restoreOk;
}

bool startsWith(const char* name, const char* prefix) {
  return name && prefix && strncmp(name, prefix, strlen(prefix)) == 0;
}

// If path ends with /package, return parent book_ root; else empty.
std::string bookRootIfPackagePath(const std::string& path) {
  constexpr const char kSuffix[] = "/package";
  constexpr size_t kSuffixLen = sizeof(kSuffix) - 1;
  if (path.size() <= kSuffixLen) return {};
  if (path.compare(path.size() - kSuffixLen, kSuffixLen, kSuffix) != 0) return {};
  std::string root = path.substr(0, path.size() - kSuffixLen);
  // Expect .../book_<16hex>
  const size_t slash = root.find_last_of('/');
  if (slash == std::string::npos || slash + 1 >= root.size()) return {};
  if (!startsWith(root.c_str() + slash + 1, "book_")) return {};
  return root;
}

}  // namespace

bool wipeCacheDirectory(const std::string& path) {
  if (path.empty() || !Storage.exists(path.c_str())) return true;

  std::vector<std::pair<std::string, bool>> stack;
  stack.reserve(16);
  stack.push_back({path, false});

  bool ok = true;
  while (!stack.empty()) {
    std::string current = std::move(stack.back().first);
    const bool postOrder = stack.back().second;
    stack.pop_back();

    if (postOrder) {
      if (Storage.exists(current.c_str()) && !Storage.rmdir(current.c_str())) {
        LOG_ERR("BookCache", "rmdir failed: %s", current.c_str());
        ok = false;
      }
      continue;
    }

    auto dir = Storage.open(current.c_str());
    if (!dir || !dir.isDirectory()) {
      if (dir) dir.close();
      if (Storage.exists(current.c_str()) && !Storage.remove(current.c_str())) {
        LOG_ERR("BookCache", "remove failed: %s", current.c_str());
        ok = false;
      }
      continue;
    }

    stack.push_back({current, true});

    std::vector<std::string> files;
    std::vector<std::string> dirs;
    files.reserve(32);
    dirs.reserve(8);
    char name[128];
    for (auto entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
      entry.getName(name, sizeof(name));
      const bool isDir = entry.isDirectory();
      entry.close();
      if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) continue;
      if (isDir) {
        dirs.push_back(name);
      } else {
        files.push_back(name);
      }
    }
    dir.close();

    for (const auto& f : files) {
      const std::string child = current + "/" + f;
      if (!Storage.remove(child.c_str())) {
        LOG_ERR("BookCache", "Failed to remove: %s", child.c_str());
        ok = false;
      }
    }
    for (const auto& d : dirs) {
      stack.push_back({current + "/" + d, false});
    }
  }
  return ok;
}

bool isBookCacheDirectoryName(const char* name) {
  if (!name) return false;
  // Unified Casper ownership (Rivulet + package).
  if (startsWith(name, "book_")) return true;
  // Legacy path-hash packages.
  return startsWith(name, "epub_") || startsWith(name, "txt_") || startsWith(name, "xtc_");
}

void clearBookCache(const std::string& path) {
  if (path.empty()) return;

  if (FsHelpers::hasEpubExtension(path) || FsHelpers::hasXtcExtension(path) || FsHelpers::hasTxtExtension(path)) {
    // v0.1.8 primary: epub_/xtc_/txt_<std::hash> under /.crosspoint
    const char* root = CasperPaths::kPackageCacheRoot;
    std::string cachePath;
    if (FsHelpers::hasEpubExtension(path)) {
      cachePath = root + std::string("/epub_") + std::to_string(std::hash<std::string>{}(path));
    } else if (FsHelpers::hasXtcExtension(path)) {
      cachePath = root + std::string("/xtc_") + std::to_string(std::hash<std::string>{}(path));
    } else {
      cachePath = root + std::string("/txt_") + std::to_string(std::hash<std::string>{}(path));
    }
    if (Storage.exists(cachePath.c_str())) {
      (void)clearBookCacheDirectoryPreservingStats(cachePath);
    }
    // Belt: even if the package-dir wipe failed partway, rivulet/ must go or
    // the next open reuses a finished page map and never rebuilds the chapter.
    const std::string rivulet = cachePath + "/rivulet";
    if (Storage.exists(rivulet.c_str())) {
      const bool rivOk = wipeCacheDirectory(rivulet);
      LOG_INF("BookCache", "wiped rivulet=%d path=%s", rivOk ? 1 : 0, rivulet.c_str());
    }
    // Optional WIP book_<fnv> folder (never created by v0.1.8 / this build).
    if (BookPathId::isCasperPackageRoot(root)) {
      const std::string bookRoot = BookPathId::bookRoot(path, root);
      if (!bookRoot.empty() && Storage.exists(bookRoot.c_str())) {
        (void)clearBookOwnershipDir(bookRoot);
      }
    }
  }

  LOG_DBG("BookCache", "Done clearing reading cache for: %s", path.c_str());
}

bool clearBookCacheDirectoryPreservingStats(const std::string& cachePath) {
  if (cachePath.empty()) return false;
  if (!Storage.exists(cachePath.c_str())) {
    LOG_DBG("BookCache", "Cache does not exist: %s", cachePath.c_str());
    return true;
  }

  // package/ under book_<id> → clear whole ownership tree (includes rivulet/).
  const std::string ownershipRoot = bookRootIfPackagePath(cachePath);
  if (!ownershipRoot.empty()) {
    return clearBookOwnershipDir(ownershipRoot);
  }

  // book_<id> itself.
  {
    const size_t slash = cachePath.find_last_of('/');
    const char* base = (slash == std::string::npos) ? cachePath.c_str() : cachePath.c_str() + slash + 1;
    if (startsWith(base, "book_")) {
      return clearBookOwnershipDir(cachePath);
    }
  }

  // Legacy epub_*/txt_*/xtc_* package dir: wipe tree, restore preserve list.
  std::vector<PreservedFile> files;
  collectPreservedFiles(cachePath, files);
  std::vector<bool> moved(files.size(), false);

  bool preserveOk = true;
  for (size_t i = 0; i < files.size(); ++i) {
    bool m = false;
    if (!preserveFile(cachePath, files[i], m)) {
      preserveOk = false;
    }
    moved[i] = m;
  }

  if (!preserveOk) {
    for (size_t i = 0; i < files.size(); ++i) {
      (void)restoreFile(cachePath, files[i], moved[i]);
    }
    LOG_ERR("BookCache", "Aborted cache clear; could not preserve stats: %s", cachePath.c_str());
    return false;
  }

  const bool clearOk = wipeCacheDirectory(cachePath);
  bool restoreOk = true;
  for (size_t i = 0; i < files.size(); ++i) {
    if (!restoreFile(cachePath, files[i], moved[i])) restoreOk = false;
  }
  if (!clearOk) {
    LOG_ERR("BookCache", "Failed to clear cache directory: %s", cachePath.c_str());
  } else {
    LOG_DBG("BookCache", "Cleared cache preserving stats: %s", cachePath.c_str());
  }
  return clearOk && restoreOk;
}
