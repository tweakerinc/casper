#pragma once

#include <cstdio>
#include <cstring>

// On-disk layout for stats (no heap, no Arduino). Device I/O lives in
// StatsBackup.cpp / BookReadingStats.cpp. Host tests pin the names.
//
//   /.casper-stats-backup/stats_YYYY-MM-DD.bin   lifetime global only
//   /.crosspoint/epub_<hash>/.trash/stats_vN.bin  Delete Book Stats (rename)
//
// Never walk the library to copy every book's stats — that is a sleep-path
// stall and a crash risk on a large SD card. Recover Stats moves .trash back.
namespace statsbackup {

inline constexpr const char* kDir = "/.casper-stats-backup";
inline constexpr const char* kTrashFolder = ".trash";
inline constexpr int kKeepSnaps = 7;

inline bool trashDirPath(const char* cacheDir, char* out, const size_t outLen) {
  if (!cacheDir || cacheDir[0] == '\0' || !out || outLen == 0) return false;
  const int n = std::snprintf(out, outLen, "%s/%s", cacheDir, kTrashFolder);
  return n > 0 && static_cast<size_t>(n) < outLen;
}

inline bool trashFilePath(const char* cacheDir, const char* fileName, char* out, const size_t outLen) {
  if (!cacheDir || cacheDir[0] == '\0' || !fileName || fileName[0] == '\0' || !out || outLen == 0) return false;
  const int n = std::snprintf(out, outLen, "%s/%s/%s", cacheDir, kTrashFolder, fileName);
  return n > 0 && static_cast<size_t>(n) < outLen;
}

}  // namespace statsbackup
