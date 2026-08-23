#pragma once

#include <cctype>
#include <cstdio>
#include <cstring>

// On-disk layout for stats backups (no heap, no Arduino). Device I/O lives in
// StatsBackup.cpp. Host tests pin the names so a sleep backup and a restore
// look for the same files.
//
//   /.casper-stats-backup/
//     stats_YYYY-MM-DD.bin          lifetime global (existing)
//     books/YYYY-MM-DD/<folder>.bin raw per-book stats_vN.bin copy (75 bytes)
//     trash/<folder>.bin            last Delete Book Stats payload
//
// Never pack books into one blob: that would load every BookReadingStats into
// DRAM. Copy one 75-byte file at a time.
namespace statsbackup {

inline constexpr const char* kDir = "/.casper-stats-backup";
inline constexpr const char* kBooksDir = "/.casper-stats-backup/books";
inline constexpr const char* kTrashDir = "/.casper-stats-backup/trash";
inline constexpr int kKeepSnaps = 7;
inline constexpr size_t kMinBookFileBytes = 11;
inline constexpr size_t kMaxBookFileBytes = 128;  // stats_v6 is 75; reject junk
inline constexpr int kMaxBooksPerSnap = 512;

inline bool startsWith(const char* s, const char* pfx) {
  if (!s || !pfx) return false;
  const size_t n = std::strlen(pfx);
  return std::strncmp(s, pfx, n) == 0;
}

inline bool isBookCacheFolderName(const char* name) {
  if (!name || name[0] == '\0') return false;
  return startsWith(name, "epub_") || startsWith(name, "txt_") || startsWith(name, "xtc_") || startsWith(name, "book_");
}

inline bool bookBackupFileName(const char* cacheFolder, char* out, const size_t outLen) {
  if (!cacheFolder || !out || outLen == 0) return false;
  if (!isBookCacheFolderName(cacheFolder)) return false;
  const int n = std::snprintf(out, outLen, "%s.bin", cacheFolder);
  return n > 0 && static_cast<size_t>(n) < outLen;
}

// stats_2026-08-23.bin / stats_2026-08-23_1430.bin → 2026-08-23
// stats_backup_003.bin → backup_003
inline bool snapIdFromGlobalFileName(const char* fileName, char* out, const size_t outLen) {
  if (!fileName || !out || outLen < 2) return false;
  if (std::strncmp(fileName, "stats_", 6) != 0) return false;
  const char* rest = fileName + 6;
  const size_t restLen = std::strlen(rest);
  if (restLen < 8) return false;
  if (std::strcmp(rest + restLen - 4, ".bin") != 0) return false;
  const size_t stemLen = restLen - 4;
  if (stemLen == 0) return false;

  if (stemLen >= 10 && std::isdigit(static_cast<unsigned char>(rest[0])) && rest[4] == '-' && rest[7] == '-') {
    if (outLen < 11) return false;
    std::memcpy(out, rest, 10);
    out[10] = '\0';
    return true;
  }

  if (stemLen + 1 > outLen) return false;
  std::memcpy(out, rest, stemLen);
  out[stemLen] = '\0';
  return true;
}

}  // namespace statsbackup
