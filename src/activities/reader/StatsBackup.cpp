#include "StatsBackup.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "BookReadingStats.h"
#include "GlobalReadingStats.h"
#include "ReadingStatsUtils.h"
#include "util/CasperBookStore.h"
#include "util/CasperPaths.h"
#include "util/TaskWatchdog.h"

namespace {
constexpr char LOG_TAG[] = "SBACK";
// Prefer Casper path; fall back to legacy Casper if not migrated yet.
constexpr char GLOBAL_STATS_PATH[] = "/.crosspoint/global_stats.bin";
constexpr const char* BACKUP_DIR = statsbackup::kDir;
constexpr int DEFAULT_BACKUP_KEEP_COUNT = statsbackup::kKeepSnaps;

struct BackupName {
  char value[64] = {};
};

bool isStatsBackupFileName(const char* name) {
  if (!name || strncmp(name, "stats_", 6) != 0) return false;
  const size_t len = strlen(name);
  return len > 10 && strcmp(name + len - 4, ".bin") == 0;
}

bool copyString(const char* src, char* dst, const size_t dstLen) {
  if (!dst || dstLen == 0) return false;
  const int written = snprintf(dst, dstLen, "%s", src ? src : "");
  return written > 0 && static_cast<size_t>(written) < dstLen;
}

bool buildDatedBackupName(const ReadingStatsDateTime& dt, const bool manual, char* out, const size_t outLen) {
  int written = 0;
  if (manual) {
    written = snprintf(out, outLen, "stats_%04u-%02u-%02u_%02u%02u.bin", static_cast<unsigned>(dt.date.year),
                       static_cast<unsigned>(dt.date.month), static_cast<unsigned>(dt.date.day),
                       static_cast<unsigned>(dt.hour), static_cast<unsigned>(dt.minute));
  } else {
    written = snprintf(out, outLen, "stats_%04u-%02u-%02u.bin", static_cast<unsigned>(dt.date.year),
                       static_cast<unsigned>(dt.date.month), static_cast<unsigned>(dt.date.day));
  }
  return written > 0 && static_cast<size_t>(written) < outLen;
}

bool parseIncrementingIndex(const char* name, uint32_t& outIndex) {
  constexpr char prefix[] = "stats_backup_";
  constexpr size_t prefixLen = sizeof(prefix) - 1;
  if (!name || strncmp(name, prefix, prefixLen) != 0) return false;
  const char* digits = name + prefixLen;
  const char* suffix = strstr(digits, ".bin");
  if (!suffix || suffix == digits || suffix[4] != '\0') return false;

  uint32_t value = 0;
  for (const char* p = digits; p < suffix; ++p) {
    if (!std::isdigit(static_cast<unsigned char>(*p))) return false;
    value = value * 10u + static_cast<uint32_t>(*p - '0');
  }
  if (value == 0) return false;
  outIndex = value;
  return true;
}

bool nextIncrementingBackupName(char* out, const size_t outLen) {
  HalFile dir = Storage.open(BACKUP_DIR);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    const int written = snprintf(out, outLen, "stats_backup_%03u.bin", 1u);
    return written > 0 && static_cast<size_t>(written) < outLen;
  }

  char name[128];
  uint32_t maxIndex = 0;
  for (HalFile file = dir.openNextFile(); file; file = dir.openNextFile()) {
    const bool isDirectory = file.isDirectory();
    const size_t nameLen = file.getName(name, sizeof(name));
    file.close();
    if (isDirectory || nameLen == 0) continue;

    uint32_t index = 0;
    if (parseIncrementingIndex(name, index) && index > maxIndex) {
      maxIndex = index;
    }
  }
  dir.close();

  const int written = snprintf(out, outLen, "stats_backup_%03u.bin", static_cast<unsigned>(maxIndex + 1));
  return written > 0 && static_cast<size_t>(written) < outLen;
}

bool chooseBackupName(const bool manual, char* out, const size_t outLen) {
  ReadingStatsDateTime now;
  if (getCurrentLocalReadingStatsDateTime(now)) {
    return buildDatedBackupName(now, manual, out, outLen);
  }
  return nextIncrementingBackupName(out, outLen);
}

bool readStatsFile(std::array<uint8_t, GlobalReadingStats::CURRENT_FILE_SIZE>& buffer, size_t& outSize) {
  outSize = 0;

  const char* path = GLOBAL_STATS_PATH;
  HalFile file;
  if (!Storage.openFileForRead(LOG_TAG, path, file)) {
    LOG_ERR(LOG_TAG, "Could not open stats file for backup: %s", GLOBAL_STATS_PATH);
    return false;
  }

  const size_t fileSize = file.fileSize();
  if (fileSize < GlobalReadingStats::MIN_SUPPORTED_FILE_SIZE || fileSize > buffer.size()) {
    LOG_ERR(LOG_TAG, "Stats file has unsupported size for backup: %u bytes", static_cast<unsigned>(fileSize));
    file.close();
    return false;
  }

  const int read = file.read(buffer.data(), fileSize);
  file.close();
  if (read != static_cast<int>(fileSize)) {
    LOG_ERR(LOG_TAG, "Failed to read stats file for backup: %d/%u bytes", read, static_cast<unsigned>(fileSize));
    return false;
  }

  outSize = fileSize;
  return true;
}

bool writeBackupFile(const char* path, const uint8_t* data, const size_t size) {
  const std::string tmpPath = std::string(path) + ".tmp";
  if (Storage.exists(tmpPath.c_str()) && !Storage.remove(tmpPath.c_str())) {
    LOG_ERR(LOG_TAG, "Could not remove stale backup temp file: %s", tmpPath.c_str());
    return false;
  }

  HalFile file;
  if (!Storage.openFileForWrite(LOG_TAG, tmpPath.c_str(), file)) {
    LOG_ERR(LOG_TAG, "Could not open backup temp file: %s", tmpPath.c_str());
    return false;
  }

  const size_t written = file.write(data, size);
  if (written != size) {
    LOG_ERR(LOG_TAG, "Short write for backup temp file %s: %u/%u bytes", tmpPath.c_str(),
            static_cast<unsigned>(written), static_cast<unsigned>(size));
    file.close();
    Storage.remove(tmpPath.c_str());
    return false;
  }

  file.flush();
  file.close();

  if (Storage.exists(path) && !Storage.remove(path)) {
    LOG_ERR(LOG_TAG, "Could not replace backup file: %s", path);
    Storage.remove(tmpPath.c_str());
    return false;
  }

  if (!Storage.rename(tmpPath.c_str(), path)) {
    LOG_ERR(LOG_TAG, "Could not publish backup file: %s", path);
    Storage.remove(tmpPath.c_str());
    return false;
  }

  return true;
}

bool findLiveStatsFile(const char* cacheDir, char* out, const size_t outLen) {
  if (!cacheDir || !out || outLen == 0) return false;
  static constexpr const char* kNames[] = {"stats_v6.bin", "stats_v5.bin", "stats_v4.bin", "stats_v3.bin",
                                           "stats_v2.bin", "stats_v1.bin", "stats.bin"};
  for (const char* name : kNames) {
    const int n = snprintf(out, outLen, "%s/%s", cacheDir, name);
    if (n > 0 && static_cast<size_t>(n) < outLen && Storage.exists(out)) return true;
  }
  return false;
}

bool copySmallStatsFile(const char* srcPath, const char* destPath) {
  if (!srcPath || !destPath) return false;
  HalFile in;
  if (!Storage.openFileForRead(LOG_TAG, srcPath, in)) return false;
  const size_t sz = in.fileSize();
  if (sz < statsbackup::kMinBookFileBytes || sz > statsbackup::kMaxBookFileBytes) {
    LOG_ERR(LOG_TAG, "Skip stats copy size=%u %s", static_cast<unsigned>(sz), srcPath);
    in.close();
    return false;
  }
  uint8_t buf[statsbackup::kMaxBookFileBytes];
  const int n = in.read(buf, sz);
  in.close();
  if (n != static_cast<int>(sz)) {
    LOG_ERR(LOG_TAG, "Short read stats copy %s", srcPath);
    return false;
  }
  return writeBackupFile(destPath, buf, sz);
}

bool copyLiveStatsTo(const char* destPath, const char* cacheDir) {
  char src[192];
  if (!findLiveStatsFile(cacheDir, src, sizeof(src))) return false;
  return copySmallStatsFile(src, destPath);
}

bool bookFileNameForPath(const char* bookPath, char* out, const size_t outLen) {
  if (!bookPath || bookPath[0] == '\0') return false;
  const std::string folder = CasperBook::cacheFolderName(bookPath);
  return statsbackup::bookBackupFileName(folder.c_str(), out, outLen);
}

bool trashPathForBook(const char* bookPath, char* out, const size_t outLen) {
  char fileName[80];
  if (!bookFileNameForPath(bookPath, fileName, sizeof(fileName))) return false;
  const int n = snprintf(out, outLen, "%s/%s", statsbackup::kTrashDir, fileName);
  return n > 0 && static_cast<size_t>(n) < outLen;
}

int backupAllBookStats(const char* snapId) {
  if (!snapId || snapId[0] == '\0') return 0;
  if (!Storage.ensureDirectoryExists(statsbackup::kBooksDir)) {
    LOG_ERR(LOG_TAG, "Could not create book stats backup dir");
    return 0;
  }
  char snapDir[192];
  const int snapN = snprintf(snapDir, sizeof(snapDir), "%s/%s", statsbackup::kBooksDir, snapId);
  if (snapN <= 0 || static_cast<size_t>(snapN) >= sizeof(snapDir)) return 0;
  if (!Storage.ensureDirectoryExists(snapDir)) {
    LOG_ERR(LOG_TAG, "Could not create book snap dir %s", snapDir);
    return 0;
  }

  HalFile root = Storage.open(CasperPaths::kRoot);
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    return 0;
  }

  char name[128];
  int copied = 0;
  for (HalFile entry = root.openNextFile(); entry; entry = root.openNextFile()) {
    resetTaskWatchdogIfSubscribed();
    const bool isDir = entry.isDirectory();
    const size_t nameLen = entry.getName(name, sizeof(name));
    entry.close();
    if (!isDir || nameLen == 0 || !statsbackup::isBookCacheFolderName(name)) continue;
    if (copied >= statsbackup::kMaxBooksPerSnap) {
      LOG_ERR(LOG_TAG, "Book stats backup cap %d — remaining folders skipped", statsbackup::kMaxBooksPerSnap);
      break;
    }

    char cacheDir[192];
    char backupName[80];
    char dest[192];
    const int cN = snprintf(cacheDir, sizeof(cacheDir), "%s/%s", CasperPaths::kRoot, name);
    if (cN <= 0 || static_cast<size_t>(cN) >= sizeof(cacheDir)) continue;
    if (!statsbackup::bookBackupFileName(name, backupName, sizeof(backupName))) continue;
    const int dN = snprintf(dest, sizeof(dest), "%s/%s", snapDir, backupName);
    if (dN <= 0 || static_cast<size_t>(dN) >= sizeof(dest)) continue;
    if (copyLiveStatsTo(dest, cacheDir)) ++copied;
  }
  root.close();
  LOG_DBG(LOG_TAG, "Wrote %d book stats into %s", copied, snapDir);
  return copied;
}

bool removeDirContentsThenRmdir(const char* path) {
  if (!path || path[0] == '\0') return true;
  HalFile dir = Storage.open(path);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return !Storage.exists(path) || Storage.remove(path);
  }
  char name[128];
  for (;;) {
    resetTaskWatchdogIfSubscribed();
    dir.rewindDirectory();
    bool removed = false;
    for (HalFile file = dir.openNextFile(); file; file = dir.openNextFile()) {
      const bool isDir = file.isDirectory();
      file.getName(name, sizeof(name));
      file.close();
      if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) continue;
      char child[192];
      const int n = snprintf(child, sizeof(child), "%s/%s", path, name);
      if (n <= 0 || static_cast<size_t>(n) >= sizeof(child)) continue;
      if (isDir) {
        (void)removeDirContentsThenRmdir(child);
      } else if (!Storage.remove(child)) {
        LOG_ERR(LOG_TAG, "Failed to prune %s", child);
        dir.close();
        return false;
      }
      removed = true;
      break;
    }
    if (!removed) break;
  }
  dir.close();
  return Storage.rmdir(path);
}

int pruneBookSnaps(const int keep) {
  if (keep < 0) return 0;
  HalFile dir = Storage.open(statsbackup::kBooksDir);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return 0;
  }

  char name[128];
  std::vector<BackupName> names;
  names.reserve(16);
  for (HalFile file = dir.openNextFile(); file; file = dir.openNextFile()) {
    const bool isDirectory = file.isDirectory();
    const size_t nameLen = file.getName(name, sizeof(name));
    file.close();
    if (!isDirectory || nameLen == 0) continue;
    BackupName snap;
    if (copyString(name, snap.value, sizeof(snap.value))) names.push_back(snap);
  }
  dir.close();

  if (static_cast<int>(names.size()) <= keep) return 0;
  std::sort(names.begin(), names.end(),
            [](const BackupName& lhs, const BackupName& rhs) { return strcmp(lhs.value, rhs.value) < 0; });

  int removed = 0;
  const int toRemove = static_cast<int>(names.size()) - keep;
  for (int i = 0; i < toRemove; ++i) {
    char path[192];
    const int n = snprintf(path, sizeof(path), "%s/%s", statsbackup::kBooksDir, names[static_cast<size_t>(i)].value);
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(path)) continue;
    if (removeDirContentsThenRmdir(path)) ++removed;
  }
  if (removed > 0) LOG_DBG(LOG_TAG, "Pruned %d old book-stats snap(s)", removed);
  return removed;
}

bool copyBackupOverLive(const char* srcPath, const char* bookPath) {
  if (!srcPath || !Storage.exists(srcPath)) return false;
  const std::string cacheDir = CasperBook::bookDirForPath(bookPath);
  if (cacheDir.empty()) return false;
  Storage.ensureDirectoryExists(cacheDir.c_str());
  char dest[192];
  const int n = snprintf(dest, sizeof(dest), "%s/stats_v6.bin", cacheDir.c_str());
  if (n <= 0 || static_cast<size_t>(n) >= sizeof(dest)) return false;
  if (!copySmallStatsFile(srcPath, dest)) return false;
  (void)BookReadingStats::load(cacheDir);
  return true;
}

}  // namespace

bool backupGlobalStats(const bool manual, char* outFileName, const size_t outFileNameLen) {
  if (!Storage.ensureDirectoryExists(BACKUP_DIR)) {
    LOG_ERR(LOG_TAG, "Could not create stats backup directory: %s", BACKUP_DIR);
    return false;
  }

  char fileName[64];
  if (!chooseBackupName(manual, fileName, sizeof(fileName))) {
    LOG_ERR(LOG_TAG, "Could not choose stats backup filename");
    return false;
  }

  std::array<uint8_t, GlobalReadingStats::CURRENT_FILE_SIZE> data{};
  size_t dataSize = 0;
  if (!readStatsFile(data, dataSize)) return false;

  char backupPath[128];
  const int pathWritten = snprintf(backupPath, sizeof(backupPath), "%s/%s", BACKUP_DIR, fileName);
  if (pathWritten <= 0 || static_cast<size_t>(pathWritten) >= sizeof(backupPath)) {
    LOG_ERR(LOG_TAG, "Could not build backup path");
    return false;
  }

  if (!writeBackupFile(backupPath, data.data(), dataSize)) return false;
  pruneBackups(DEFAULT_BACKUP_KEEP_COUNT);

  char snapId[32];
  if (statsbackup::snapIdFromGlobalFileName(fileName, snapId, sizeof(snapId))) {
    (void)backupAllBookStats(snapId);
    (void)pruneBookSnaps(statsbackup::kKeepSnaps);
  } else {
    LOG_ERR(LOG_TAG, "Could not derive book snap id from %s", fileName);
  }

  if (outFileName != nullptr && outFileNameLen > 0) {
    copyString(fileName, outFileName, outFileNameLen);
  }
  LOG_DBG(LOG_TAG, "Wrote stats backup: %s", backupPath);
  return true;
}

int pruneBackups(const int keep) {
  if (keep < 0) return 0;

  HalFile dir = Storage.open(BACKUP_DIR);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return 0;
  }

  char name[128];
  std::vector<BackupName> names;
  names.reserve(16);
  for (HalFile file = dir.openNextFile(); file; file = dir.openNextFile()) {
    const bool isDirectory = file.isDirectory();
    const size_t nameLen = file.getName(name, sizeof(name));
    file.close();
    if (isDirectory || nameLen == 0 || !isStatsBackupFileName(name)) continue;

    BackupName backupName;
    if (copyString(name, backupName.value, sizeof(backupName.value))) {
      names.push_back(backupName);
    }
  }
  dir.close();

  if (static_cast<int>(names.size()) <= keep) return 0;

  std::sort(names.begin(), names.end(),
            [](const BackupName& lhs, const BackupName& rhs) { return strcmp(lhs.value, rhs.value) < 0; });

  int removed = 0;
  const int toRemove = static_cast<int>(names.size()) - keep;
  for (int i = 0; i < toRemove; ++i) {
    char path[128];
    const int pathWritten = snprintf(path, sizeof(path), "%s/%s", BACKUP_DIR, names[static_cast<size_t>(i)].value);
    if (pathWritten <= 0 || static_cast<size_t>(pathWritten) >= sizeof(path)) continue;
    if (Storage.remove(path)) {
      removed++;
    } else {
      LOG_ERR(LOG_TAG, "Failed to prune stats backup: %s", path);
    }
  }

  if (removed > 0) {
    LOG_DBG(LOG_TAG, "Pruned %d old stats backup(s)", removed);
  }
  return removed;
}

bool stashDeletedBookStats(const char* bookPath) {
  if (!bookPath || bookPath[0] == '\0') return false;
  if (!Storage.ensureDirectoryExists(statsbackup::kTrashDir)) {
    LOG_ERR(LOG_TAG, "Could not create stats trash dir");
    return false;
  }
  char dest[192];
  if (!trashPathForBook(bookPath, dest, sizeof(dest))) return false;
  const std::string cacheDir = CasperBook::bookDirForPath(bookPath);
  if (cacheDir.empty()) return false;
  if (!copyLiveStatsTo(dest, cacheDir.c_str())) {
    LOG_DBG(LOG_TAG, "No live book stats to stash for %s", bookPath);
    return false;
  }
  LOG_INF(LOG_TAG, "Stashed deleted book stats %s", dest);
  return true;
}

bool restoreBookStats(const char* bookPath) {
  if (!bookPath || bookPath[0] == '\0') return false;
  char trash[192];
  if (trashPathForBook(bookPath, trash, sizeof(trash)) && copyBackupOverLive(trash, bookPath)) {
    LOG_INF(LOG_TAG, "Restored book stats from trash");
    return true;
  }

  char fileName[80];
  if (!bookFileNameForPath(bookPath, fileName, sizeof(fileName))) return false;

  HalFile dir = Storage.open(statsbackup::kBooksDir);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return false;
  }
  char name[128];
  std::vector<BackupName> snaps;
  snaps.reserve(16);
  for (HalFile file = dir.openNextFile(); file; file = dir.openNextFile()) {
    const bool isDirectory = file.isDirectory();
    const size_t nameLen = file.getName(name, sizeof(name));
    file.close();
    if (!isDirectory || nameLen == 0) continue;
    BackupName snap;
    if (copyString(name, snap.value, sizeof(snap.value))) snaps.push_back(snap);
  }
  dir.close();
  std::sort(snaps.begin(), snaps.end(),
            [](const BackupName& lhs, const BackupName& rhs) { return strcmp(lhs.value, rhs.value) > 0; });

  for (const auto& snap : snaps) {
    resetTaskWatchdogIfSubscribed();
    char src[192];
    const int n = snprintf(src, sizeof(src), "%s/%s/%s", statsbackup::kBooksDir, snap.value, fileName);
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(src)) continue;
    if (copyBackupOverLive(src, bookPath)) {
      LOG_INF(LOG_TAG, "Restored book stats from snap %s", snap.value);
      return true;
    }
  }
  return false;
}

bool hasRestorableBookStats(const char* bookPath) {
  if (!bookPath || bookPath[0] == '\0') return false;
  char trash[192];
  if (trashPathForBook(bookPath, trash, sizeof(trash)) && Storage.exists(trash)) return true;

  char fileName[80];
  if (!bookFileNameForPath(bookPath, fileName, sizeof(fileName))) return false;
  HalFile dir = Storage.open(statsbackup::kBooksDir);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return false;
  }
  char name[128];
  bool found = false;
  for (HalFile file = dir.openNextFile(); file; file = dir.openNextFile()) {
    const bool isDirectory = file.isDirectory();
    const size_t nameLen = file.getName(name, sizeof(name));
    file.close();
    if (!isDirectory || nameLen == 0) continue;
    char src[192];
    const int n = snprintf(src, sizeof(src), "%s/%s/%s", statsbackup::kBooksDir, name, fileName);
    if (n > 0 && static_cast<size_t>(n) < sizeof(src) && Storage.exists(src)) {
      found = true;
      break;
    }
  }
  dir.close();
  return found;
}
