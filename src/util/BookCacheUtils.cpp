#include "BookCacheUtils.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Txt.h>
#include <Xtc.h>

#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr size_t MAX_STATS_FILES_TO_PRESERVE = 8;
constexpr char STATS_PREFIX[] = "stats";
constexpr char STATS_SUFFIX[] = ".bin";

// Fixed user-state files that must survive cache clear (CrossInk parity).
struct FixedPreserve {
  const char* name;
  const char* tmpName;
};
constexpr FixedPreserve kFixedPreserve[] = {
    {"progress.bin", "clear_preserve_progress.bin"},
    {"progress.bin.bak", "clear_preserve_progress.bin.bak"},
    {"reader_settings.bin", "clear_preserve_reader_settings.bin"},
    {"dictionary_history.txt", "clear_preserve_dictionary_history.txt"},
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
  // Progress / resume position + per-book reader settings + dict history.
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
}

}  // namespace

bool isBookCacheDirectoryName(const char* name) {
  if (!name) {
    return false;
  }

  constexpr char EPUB_PREFIX[] = "epub_";
  constexpr char TXT_PREFIX[] = "txt_";
  constexpr char XTC_PREFIX[] = "xtc_";

  return strncmp(name, EPUB_PREFIX, sizeof(EPUB_PREFIX) - 1) == 0 ||
         strncmp(name, TXT_PREFIX, sizeof(TXT_PREFIX) - 1) == 0 ||
         strncmp(name, XTC_PREFIX, sizeof(XTC_PREFIX) - 1) == 0;
}

void clearBookCache(const std::string& path) {
  std::string cachePath;
  if (FsHelpers::hasEpubExtension(path)) {
    cachePath = Epub(path, "/.crosspoint").getCachePath();
  } else if (FsHelpers::hasXtcExtension(path)) {
    cachePath = Xtc(path, "/.crosspoint").getCachePath();
  } else if (FsHelpers::hasTxtExtension(path)) {
    cachePath = Txt(path, "/.crosspoint").getCachePath();
  } else {
    return;
  }
  if (cachePath.empty()) return;
  // Prefer stats-preserving clear when the directory exists.
  if (Storage.exists(cachePath.c_str())) {
    (void)clearBookCacheDirectoryPreservingStats(cachePath);
  } else if (FsHelpers::hasEpubExtension(path)) {
    Epub(path, "/.crosspoint").clearCache();
  } else if (FsHelpers::hasXtcExtension(path)) {
    Xtc(path, "/.crosspoint").clearCache();
  } else if (FsHelpers::hasTxtExtension(path)) {
    Txt(path, "/.crosspoint").clearCache();
  }
  LOG_DBG("BookCache", "Done checking metadata cache for: %s", path.c_str());
}

bool clearBookCacheDirectoryPreservingStats(const std::string& cachePath) {
  if (cachePath.empty()) return false;
  if (!Storage.exists(cachePath.c_str())) {
    LOG_DBG("BookCache", "Cache does not exist: %s", cachePath.c_str());
    return true;
  }

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

  const bool clearOk = Storage.removeDir(cachePath.c_str());
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
