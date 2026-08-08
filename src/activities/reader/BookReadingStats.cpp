#include "BookReadingStats.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <cstring>
#include <functional>

namespace {
// Binary layout v1 (11 bytes):
//   [0]     version (= 1)
//   [1-2]   sessionCount        uint16_t LE
//   [3-6]   totalReadingSeconds uint32_t LE
//   [7-10]  totalPagesTurned    uint32_t LE
//
// Binary layout v2 (12 bytes):
//   [0]     version (= 2)
//   [1-2]   sessionCount        uint16_t LE
//   [3-6]   totalReadingSeconds uint32_t LE
//   [7-10]  totalPagesTurned    uint32_t LE
//   [11]    isCompleted         uint8_t
//
// Binary layout v3 (16 bytes):
//   [0]      version (= 3)
//   [1-2]    sessionCount              uint16_t LE
//   [3-6]    totalReadingSeconds       uint32_t LE
//   [7-10]   totalPagesTurned          uint32_t LE
//   [11]     isCompleted               uint8_t
//   [12-13]  avgSecondsPerForwardPage  uint16_t LE
//   [14-15]  paceSampleCount           uint16_t LE
//
// Binary layout v4 (69 bytes):
//   [0]      version (= 4)
//   [1-2]    sessionCount              uint16_t LE
//   [3-6]    totalReadingSeconds       uint32_t LE
//   [7-10]   totalPagesTurned          uint32_t LE
//   [11]     isCompleted               uint8_t
//   [12-13]  avgSecondsPerForwardPage  uint16_t LE
//   [14-15]  paceSampleCount           uint16_t LE
//   [16]     flags bit0=startDateManual bit1=finishedDateManual
//   [17-18]  startDate.year            uint16_t LE
//   [19]     startDate.month           uint8_t
//   [20]     startDate.day             uint8_t
//   [21-22]  finishedDate.year         uint16_t LE
//   [23]     finishedDate.month        uint8_t
//   [24]     finishedDate.day          uint8_t
//   [25-40]  timeOfDaySeconds[4]       uint32_t LE each
//   [41-68]  dayOfWeekSeconds[7]       uint32_t LE each
//
// Binary layout v5 (73 bytes):
//   [0-68]   v4 fields
//   [69-72]  estimatedTimeLeftSeconds  uint32_t LE, 0 means unavailable
//
// Binary layout v6 (75 bytes):
//   [0-72]   v5 fields
//   [73-74]  progressPercentMilli      uint16_t LE, 0–10000 = 0.00–100.00%, 0xFFFF = unknown
static constexpr uint8_t STATS_FILE_VERSION = 6;
static constexpr uint8_t STATS_FILE_VERSION_V2 = 2;
static constexpr uint8_t STATS_FILE_VERSION_V1 = 1;
static constexpr uint8_t STATS_FILE_VERSION_V3 = 3;
static constexpr uint8_t STATS_FILE_VERSION_V4 = 4;
static constexpr uint8_t STATS_FILE_VERSION_V5 = 5;
static constexpr int STATS_FILE_SIZE_V1 = 11;
static constexpr int STATS_FILE_SIZE_V2 = 12;
static constexpr int STATS_FILE_SIZE_V3 = 16;
static constexpr int STATS_FILE_SIZE_V4 = 69;
static constexpr int STATS_FILE_SIZE_V5 = 73;
static constexpr int STATS_FILE_SIZE = 75;
static constexpr uint16_t MAX_PACE_SAMPLE_COUNT = 1000;
static constexpr uint8_t FLAG_START_DATE_MANUAL = 1u << 0;
static constexpr uint8_t FLAG_FINISHED_DATE_MANUAL = 1u << 1;
static constexpr const char* LEGACY_STATS_FILE_NAME = "stats.bin";
static constexpr const char* CROSSPOINT_CACHE_ROOT = "/.crosspoint";
// Cap directory scan so a corrupted cache folder cannot stall book open.
static constexpr size_t MAX_STATS_DIR_SCAN = 16;

std::string statsFileNameForVersion(const uint8_t version) {
  char buf[16];
  snprintf(buf, sizeof(buf), "stats_v%u.bin", version);
  return std::string(buf);
}

// CrossInk EPUB cache dirs use FNV-1a 64-bit of the full book path (ZipFile::fnvHash64).
// Casper / CrossPoint 1.5 use std::hash → different epub_<n> folder names on the same SD.
uint64_t fnv1a64Path(const std::string& path) {
  uint64_t hash = 14695981039346656037ull;
  for (size_t i = 0; i < path.size(); ++i) {
    hash ^= static_cast<uint8_t>(path[i]);
    hash *= 1099511628211ull;
  }
  return hash;
}

std::string stdHashEpubCachePath(const std::string& bookPath) {
  return std::string(CROSSPOINT_CACHE_ROOT) + "/epub_" + std::to_string(std::hash<std::string>{}(bookPath));
}

std::string crossInkFnvEpubCachePath(const std::string& bookPath) {
  return std::string(CROSSPOINT_CACHE_ROOT) + "/epub_" + std::to_string(fnv1a64Path(bookPath));
}

bool isStatsFileName(const char* name) {
  if (!name) return false;
  static constexpr char kPrefix[] = "stats";
  static constexpr char kSuffix[] = ".bin";
  const size_t nameLen = strlen(name);
  constexpr size_t prefixLen = sizeof(kPrefix) - 1;
  constexpr size_t suffixLen = sizeof(kSuffix) - 1;
  return nameLen >= prefixLen + suffixLen && strncmp(name, kPrefix, prefixLen) == 0 &&
         strcmp(name + nameLen - suffixLen, kSuffix) == 0;
}

uint16_t readLe16(const uint8_t* data, const int offset) {
  return static_cast<uint16_t>(data[offset]) | (static_cast<uint16_t>(data[offset + 1]) << 8);
}

uint32_t readLe32(const uint8_t* data, const int offset) {
  return static_cast<uint32_t>(data[offset]) | (static_cast<uint32_t>(data[offset + 1]) << 8) |
         (static_cast<uint32_t>(data[offset + 2]) << 16) | (static_cast<uint32_t>(data[offset + 3]) << 24);
}

void readCommonStats(const uint8_t* data, BookReadingStats& stats) {
  stats.sessionCount = readLe16(data, 1);
  stats.totalReadingSeconds = readLe32(data, 3);
  stats.totalPagesTurned = readLe32(data, 7);
}

void writeLe16(uint8_t* data, const int offset, const uint16_t value) {
  data[offset] = value & 0xFF;
  data[offset + 1] = (value >> 8) & 0xFF;
}

void writeLe32(uint8_t* data, const int offset, const uint32_t value) {
  data[offset] = value & 0xFF;
  data[offset + 1] = (value >> 8) & 0xFF;
  data[offset + 2] = (value >> 16) & 0xFF;
  data[offset + 3] = (value >> 24) & 0xFF;
}

ReadingStatsDate readDate(const uint8_t* data, const int offset) {
  ReadingStatsDate date;
  date.year = readLe16(data, offset);
  date.month = data[offset + 2];
  date.day = data[offset + 3];
  if (!date.isValid()) {
    date.clear();
  }
  return date;
}

// Parse one stats blob already read into `data` (n bytes). Returns empty on mismatch.
BookReadingStats parseStatsBlob(const uint8_t* data, const int n) {
  BookReadingStats stats;
  if (n == STATS_FILE_SIZE_V1 && data[0] == STATS_FILE_VERSION_V1) {
    readCommonStats(data, stats);
    return stats;
  }

  if (n == STATS_FILE_SIZE_V2 && data[0] == STATS_FILE_VERSION_V2) {
    readCommonStats(data, stats);
    stats.isCompleted = data[11] != 0;
    return stats;
  }

  if (n == STATS_FILE_SIZE_V3 && data[0] == STATS_FILE_VERSION_V3) {
    readCommonStats(data, stats);
    stats.isCompleted = data[11] != 0;
    stats.avgSecondsPerForwardPage = readLe16(data, 12);
    stats.paceSampleCount = readLe16(data, 14);
    return stats;
  }

  const bool isV4 = (n == STATS_FILE_SIZE_V4 && data[0] == STATS_FILE_VERSION_V4);
  const bool isV5 = (n == STATS_FILE_SIZE_V5 && data[0] == STATS_FILE_VERSION_V5);
  const bool isV6 = (n == STATS_FILE_SIZE && data[0] == STATS_FILE_VERSION);
  if (!isV4 && !isV5 && !isV6) {
    return stats;
  }
  readCommonStats(data, stats);
  stats.isCompleted = data[11] != 0;
  stats.avgSecondsPerForwardPage = readLe16(data, 12);
  stats.paceSampleCount = readLe16(data, 14);
  const uint8_t flags = data[16];
  stats.startDateManual = (flags & FLAG_START_DATE_MANUAL) != 0;
  stats.finishedDateManual = (flags & FLAG_FINISHED_DATE_MANUAL) != 0;
  stats.startDate = readDate(data, 17);
  stats.finishedDate = readDate(data, 21);
  for (size_t i = 0; i < stats.timeOfDaySeconds.size(); ++i) {
    stats.timeOfDaySeconds[i] = readLe32(data, 25 + static_cast<int>(i) * 4);
  }
  for (size_t i = 0; i < stats.dayOfWeekSeconds.size(); ++i) {
    stats.dayOfWeekSeconds[i] = readLe32(data, 41 + static_cast<int>(i) * 4);
  }
  if (isV5 || isV6) {
    stats.estimatedTimeLeftSeconds = readLe32(data, 69);
  }
  if (isV6) {
    stats.progressPercentMilli = readLe16(data, 73);
  }
  return stats;
}

bool parseKnownStatsBlob(const uint8_t* data, const int n, BookReadingStats& out) {
  if (n <= 0) return false;
  if (n == STATS_FILE_SIZE_V1 && data[0] == STATS_FILE_VERSION_V1) {
    out = parseStatsBlob(data, n);
    return true;
  }
  if (n == STATS_FILE_SIZE_V2 && data[0] == STATS_FILE_VERSION_V2) {
    out = parseStatsBlob(data, n);
    return true;
  }
  if (n == STATS_FILE_SIZE_V3 && data[0] == STATS_FILE_VERSION_V3) {
    out = parseStatsBlob(data, n);
    return true;
  }
  if (n == STATS_FILE_SIZE_V4 && data[0] == STATS_FILE_VERSION_V4) {
    out = parseStatsBlob(data, n);
    return true;
  }
  if (n == STATS_FILE_SIZE_V5 && data[0] == STATS_FILE_VERSION_V5) {
    out = parseStatsBlob(data, n);
    return true;
  }
  if (n == STATS_FILE_SIZE && data[0] == STATS_FILE_VERSION) {
    out = parseStatsBlob(data, n);
    return true;
  }
  return false;
}

// Load a specific versioned filename only (no fallback chain).
// Quiet: missing files are normal during version probes — never log them.
bool loadStatsFileNamed(const std::string& cachePath, const std::string& fileName, BookReadingStats& out) {
  if (cachePath.empty() || fileName.empty()) {
    return false;
  }
  const std::string full = cachePath + "/" + fileName;
  // exists() is silent; openFileForRead("STATS", ...) spam-logs every missing v5..v1 probe.
  if (!Storage.exists(full.c_str())) {
    return false;
  }
  HalFile f = Storage.open(full.c_str(), O_RDONLY);
  if (!f) {
    return false;
  }
  uint8_t data[STATS_FILE_SIZE] = {};
  const int n = f.read(data, STATS_FILE_SIZE);
  f.close();
  return parseKnownStatsBlob(data, n, out);
}

// Prefer the file with more lifetime data (seconds, then sessions, then pages).
bool isRicherStats(const BookReadingStats& a, const BookReadingStats& b) {
  if (a.totalReadingSeconds != b.totalReadingSeconds) {
    return a.totalReadingSeconds > b.totalReadingSeconds;
  }
  if (a.sessionCount != b.sessionCount) {
    return a.sessionCount > b.sessionCount;
  }
  if (a.totalPagesTurned != b.totalPagesTurned) {
    return a.totalPagesTurned > b.totalPagesTurned;
  }
  if (a.isCompleted != b.isCompleted) {
    return a.isCompleted;
  }
  // Prefer known progress over unknown when lifetime totals match.
  if (a.progressPercentMilli != b.progressPercentMilli) {
    if (a.progressPercentMilli == 0xFFFF) return false;
    if (b.progressPercentMilli == 0xFFFF) return true;
  }
  return false;
}

bool hasAnyStatsPayload(const BookReadingStats& s) {
  return s.sessionCount > 0 || s.totalReadingSeconds > 0 || s.totalPagesTurned > 0 || s.isCompleted ||
         s.progressPercentMilli != 0xFFFF || s.startDate.isValid() || s.finishedDate.isValid();
}

// "Empty shell" = reopen noise with no meaningful lifetime *or* progress.
// X4 hard-disables session tracking, so stats files often only contain
// progressPercentMilli — that must NOT be discarded or home bars stay at 0%.
bool looksLikeEmptyShell(const BookReadingStats& s) {
  if (s.progressPercentMilli != 0xFFFF) {
    return false;  // known book position is real data (home Recents bars)
  }
  return s.sessionCount == 0 && s.totalReadingSeconds < 60 && s.totalPagesTurned <= 1 && !s.isCompleted;
}

// Merge progress/ETA from a thin newer file into richer lifetime totals.
void preferRicherKeepProgress(BookReadingStats& best, const BookReadingStats& candidate) {
  if (!isRicherStats(candidate, best)) {
    // Candidate is thinner: still take progress/ETA if best lacks them.
    if (best.progressPercentMilli == 0xFFFF && candidate.progressPercentMilli != 0xFFFF) {
      best.progressPercentMilli = candidate.progressPercentMilli;
    }
    if (best.estimatedTimeLeftSeconds == 0 && candidate.estimatedTimeLeftSeconds != 0) {
      best.estimatedTimeLeftSeconds = candidate.estimatedTimeLeftSeconds;
    }
    return;
  }
  const uint16_t progressMilli = best.progressPercentMilli;
  const uint32_t eta = best.estimatedTimeLeftSeconds;
  best = candidate;
  if (progressMilli != 0xFFFF && best.progressPercentMilli == 0xFFFF) {
    best.progressPercentMilli = progressMilli;
  }
  if (eta != 0 && best.estimatedTimeLeftSeconds == 0) {
    best.estimatedTimeLeftSeconds = eta;
  }
}

void considerNamedStatsFile(const std::string& cachePath, const std::string& fileName, BookReadingStats& best,
                            bool& have) {
  BookReadingStats candidate;
  if (!loadStatsFileNamed(cachePath, fileName, candidate)) {
    return;
  }
  if (!have) {
    best = candidate;
    have = true;
    return;
  }
  preferRicherKeepProgress(best, candidate);
}

// Walk the cache folder for any stats*.bin (covers odd CrossInk / fork names).
void considerDirectoryStatsFiles(const std::string& cachePath, BookReadingStats& best, bool& have) {
  auto dir = Storage.open(cachePath.c_str());
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return;
  }

  size_t scanned = 0;
  char name[96];
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    const bool isDir = file.isDirectory();
    file.getName(name, sizeof(name));
    file.close();
    if (isDir || !isStatsFileName(name)) continue;
    if (++scanned > MAX_STATS_DIR_SCAN) break;
    considerNamedStatsFile(cachePath, name, best, have);
  }
  dir.close();
}

// Load the richest parsable stats blob under one cache directory.
// Cheap path: if current versioned file is already "rich", skip older names + dir walk.
// Do NOT probe stats_v5..v1 individually — each miss used to log "File does not exist"
// and hammer the SD on every Home/recents paint. One dir walk covers legacy names.
BookReadingStats loadBestInCacheDir(const std::string& cachePath) {
  BookReadingStats best;
  bool have = false;
  if (cachePath.empty()) {
    return best;
  }
  // No cache folder → no stats (single silent exists; no version spam).
  if (!Storage.exists(cachePath.c_str())) {
    return best;
  }

  // Prefer current filename first (Casper writes stats_vN.bin on every save/migrate).
  const std::string currentName = statsFileNameForVersion(STATS_FILE_VERSION);
  considerNamedStatsFile(cachePath, currentName, best, have);
  if (have && hasAnyStatsPayload(best) && !looksLikeEmptyShell(best)) {
    return best;
  }

  // One directory listing for any stats*.bin (legacy / CrossInk / odd forks).
  considerDirectoryStatsFiles(cachePath, best, have);
  // Last resort: legacy unversioned name if the dir walk missed it (empty dir edge).
  if (!have) {
    considerNamedStatsFile(cachePath, LEGACY_STATS_FILE_NAME, best, have);
  }
  return best;
}

void ensureCacheDir(const std::string& cachePath) {
  if (cachePath.empty() || Storage.exists(cachePath.c_str())) {
    return;
  }
  Storage.mkdir(cachePath.c_str());
}

// Candidate cache dirs for one book: Casper/1.5 path first, then CrossInk FNV EPUB.
void collectBookCacheCandidates(const std::string& bookPath, std::string* paths, size_t& count, const size_t max) {
  count = 0;
  auto pushUnique = [&](const std::string& p) {
    if (p.empty() || count >= max) return;
    for (size_t i = 0; i < count; ++i) {
      if (paths[i] == p) return;
    }
    paths[count++] = p;
  };

  pushUnique(BookReadingStats::cachePathForBook(bookPath));

  // CrossInk stores EPUBs under epub_<fnv64>; Casper uses epub_<std::hash>.
  if (FsHelpers::hasEpubExtension(bookPath)) {
    pushUnique(crossInkFnvEpubCachePath(bookPath));
    // Also try std::hash explicitly (same as cachePathForBook for EPUB, but
    // cheap insurance if Epub constructor ever diverges).
    pushUnique(stdHashEpubCachePath(bookPath));
  }
}

}  // namespace

std::string BookReadingStats::cachePathForBook(const std::string& bookPath) {
  if (bookPath.empty()) {
    return {};
  }
  if (FsHelpers::hasEpubExtension(bookPath)) {
    return Epub(bookPath, CROSSPOINT_CACHE_ROOT).getCachePath();
  }
  if (FsHelpers::hasXtcExtension(bookPath)) {
    return std::string(CROSSPOINT_CACHE_ROOT) + "/xtc_" + std::to_string(std::hash<std::string>{}(bookPath));
  }
  if (FsHelpers::hasTxtExtension(bookPath) || FsHelpers::hasMarkdownExtension(bookPath)) {
    return std::string(CROSSPOINT_CACHE_ROOT) + "/txt_" + std::to_string(std::hash<std::string>{}(bookPath));
  }
  return {};
}

// Session memo for loadForBook — Home/recents used to re-scan the same 4–5 books
// several times per paint (each scan was multi-version SD probes).
// cachePath is the Casper primary folder so save() can update one slot without
// wiping the whole memo (or re-hashing every path).
struct LoadForBookMemo {
  static constexpr size_t kCap = 8;
  std::string path[kCap];
  std::string cachePath[kCap];
  BookReadingStats stats[kCap];
  bool valid[kCap] = {};
  size_t next = 0;
};
LoadForBookMemo g_loadForBookMemo;

void memoStore(const std::string& bookPath, const BookReadingStats& stats) {
  if (bookPath.empty()) return;
  const std::string primary = BookReadingStats::cachePathForBook(bookPath);
  for (size_t i = 0; i < LoadForBookMemo::kCap; ++i) {
    if (g_loadForBookMemo.valid[i] && g_loadForBookMemo.path[i] == bookPath) {
      g_loadForBookMemo.stats[i] = stats;
      g_loadForBookMemo.cachePath[i] = primary;
      return;
    }
  }
  const size_t i = g_loadForBookMemo.next % LoadForBookMemo::kCap;
  g_loadForBookMemo.path[i] = bookPath;
  g_loadForBookMemo.cachePath[i] = primary;
  g_loadForBookMemo.stats[i] = stats;
  g_loadForBookMemo.valid[i] = true;
  ++g_loadForBookMemo.next;
}

// After a successful disk write: refresh memo for that cache folder only.
void memoUpdateByCachePath(const std::string& cachePath, const BookReadingStats& stats) {
  if (cachePath.empty()) return;
  for (size_t i = 0; i < LoadForBookMemo::kCap; ++i) {
    if (g_loadForBookMemo.valid[i] && g_loadForBookMemo.cachePath[i] == cachePath) {
      g_loadForBookMemo.stats[i] = stats;
    }
  }
}

bool memoLookup(const std::string& bookPath, BookReadingStats& out) {
  for (size_t i = 0; i < LoadForBookMemo::kCap; ++i) {
    if (g_loadForBookMemo.valid[i] && g_loadForBookMemo.path[i] == bookPath) {
      out = g_loadForBookMemo.stats[i];
      return true;
    }
  }
  return false;
}

void memoInvalidate(const std::string& bookPath) {
  if (bookPath.empty()) {
    for (size_t i = 0; i < LoadForBookMemo::kCap; ++i) g_loadForBookMemo.valid[i] = false;
    return;
  }
  for (size_t i = 0; i < LoadForBookMemo::kCap; ++i) {
    if (g_loadForBookMemo.valid[i] && g_loadForBookMemo.path[i] == bookPath) {
      g_loadForBookMemo.valid[i] = false;
    }
  }
}

// One-time CrossInk / legacy scan marker under the Casper cache folder.
// Present ⇒ we already looked for alternate cache dirs once; do not thrash SD
// again until the user clears cache (which deletes this folder/marker).
static constexpr const char* kLegacyScanMarker = "stats_legacy_scanned";

bool legacyScanAlreadyDone(const std::string& primaryPath) {
  if (primaryPath.empty()) return false;
  return Storage.exists((primaryPath + "/" + kLegacyScanMarker).c_str());
}

void markLegacyScanDone(const std::string& primaryPath) {
  if (primaryPath.empty()) return;
  ensureCacheDir(primaryPath);
  const std::string markPath = primaryPath + "/" + kLegacyScanMarker;
  if (Storage.exists(markPath.c_str())) return;
  HalFile f = Storage.open(markPath.c_str(), O_RDWR | O_CREAT | O_TRUNC);
  if (!f) return;
  const char one = '1';
  f.write(reinterpret_cast<const uint8_t*>(&one), 1);
  f.close();
}

BookReadingStats BookReadingStats::loadForBook(const std::string& bookPath) {
  if (bookPath.empty()) {
    return {};
  }

  BookReadingStats memoed;
  if (memoLookup(bookPath, memoed)) {
    return memoed;
  }

  constexpr size_t kMaxPaths = 4;
  std::string paths[kMaxPaths];
  size_t pathCount = 0;
  collectBookCacheCandidates(bookPath, paths, pathCount, kMaxPaths);
  if (pathCount == 0) {
    memoStore(bookPath, {});
    return {};
  }

  const std::string& primaryPath = paths[0];
  const std::string currentName = statsFileNameForVersion(STATS_FILE_VERSION);

  // 1) Fast path: Casper's current stats file under the primary cache.
  // Accept progress-only files (X4) even if lifetime totals are zero.
  {
    BookReadingStats primaryFast;
    if (loadStatsFileNamed(primaryPath, currentName, primaryFast) && hasAnyStatsPayload(primaryFast)) {
      if (!looksLikeEmptyShell(primaryFast) || primaryFast.progressPercentMilli != 0xFFFF) {
        memoStore(bookPath, primaryFast);
        return primaryFast;
      }
    }
  }

  // 2) Primary folder only (no CrossInk yet): any stats*.bin already migrated here.
  {
    const BookReadingStats primaryOnly = loadBestInCacheDir(primaryPath);
    if (hasAnyStatsPayload(primaryOnly) && !looksLikeEmptyShell(primaryOnly)) {
      // Promote to current filename if needed (single-dir, cheap).
      BookReadingStats currentOnly;
      if (!loadStatsFileNamed(primaryPath, currentName, currentOnly) || looksLikeEmptyShell(currentOnly) ||
          isRicherStats(primaryOnly, currentOnly)) {
        ensureCacheDir(primaryPath);
        primaryOnly.save(primaryPath);
      }
      // We have real Casper stats — no need for future CrossInk hunts.
      markLegacyScanDone(primaryPath);
      memoStore(bookPath, primaryOnly);
      return primaryOnly;
    }
  }

  // 3) Already did the one-time CrossInk/legacy hunt for this book (or user never
  // had stats). Deleting the cache folder removes the marker and allows a re-scan.
  if (legacyScanAlreadyDone(primaryPath)) {
    memoStore(bookPath, {});
    return {};
  }

  // 4) One-time CrossInk / alternate-path scan (first flash, first open, or after
  // clear cache). Not repeated on every Home paint or reboot.
  BookReadingStats best;
  bool have = false;
  size_t bestPathIndex = 0;

  for (size_t i = 0; i < pathCount; ++i) {
    const BookReadingStats candidate = loadBestInCacheDir(paths[i]);
    if (!hasAnyStatsPayload(candidate)) {
      continue;
    }
    if (!have) {
      best = candidate;
      have = true;
      bestPathIndex = i;
      continue;
    }
    if (isRicherStats(candidate, best)) {
      preferRicherKeepProgress(best, candidate);
      bestPathIndex = i;
    } else {
      preferRicherKeepProgress(best, candidate);
    }
  }

  // Always mark so we never re-walk CrossInk folders for this book.
  markLegacyScanDone(primaryPath);

  if (!have) {
    memoStore(bookPath, {});
    return {};
  }

  // Land the winner under Casper's primary cache path.
  if (hasAnyStatsPayload(best) && !looksLikeEmptyShell(best)) {
    ensureCacheDir(primaryPath);
    best.save(primaryPath);
    if (bestPathIndex != 0) {
      LOG_DBG("STATS", "Migrated book stats from CrossInk/legacy cache %s -> %s", paths[bestPathIndex].c_str(),
              primaryPath.c_str());
    } else {
      LOG_DBG("STATS", "Normalized book stats into %s", primaryPath.c_str());
    }
  }

  memoStore(bookPath, best);
  return best;
}

BookReadingStats BookReadingStats::load(const std::string& cachePath) {
  BookReadingStats stats = loadBestInCacheDir(cachePath);
  if (!hasAnyStatsPayload(stats)) {
    return stats;
  }

  // Promote the richest found blob to the current versioned filename when the
  // primary file is missing or a thin reopen shell (CrossInk v5 <-> Casper v6).
  const std::string currentName = statsFileNameForVersion(STATS_FILE_VERSION);
  BookReadingStats currentOnly;
  const bool haveCurrent = loadStatsFileNamed(cachePath, currentName, currentOnly);
  if (!haveCurrent || looksLikeEmptyShell(currentOnly) || isRicherStats(stats, currentOnly)) {
    const bool sameTotals = haveCurrent && currentOnly.totalReadingSeconds == stats.totalReadingSeconds &&
                            currentOnly.sessionCount == stats.sessionCount &&
                            currentOnly.totalPagesTurned == stats.totalPagesTurned &&
                            currentOnly.isCompleted == stats.isCompleted &&
                            currentOnly.progressPercentMilli == stats.progressPercentMilli &&
                            currentOnly.estimatedTimeLeftSeconds == stats.estimatedTimeLeftSeconds;
    if (!sameTotals) {
      ensureCacheDir(cachePath);
      stats.save(cachePath);
      LOG_DBG("STATS", "Wrote normalized %s under %s", currentName.c_str(), cachePath.c_str());
    }
  }

  return stats;
}

float BookReadingStats::getProgressPercent() const {
  // Mark Finished (or auto end-of-book) always displays as 100%, even if a bad
  // reopen overwrote progressPercentMilli with a partial-section estimate.
  if (isCompleted) {
    return 100.0f;
  }
  if (progressPercentMilli == 0xFFFF) {
    return -1.0f;
  }
  float pct = static_cast<float>(progressPercentMilli) / 100.0f;
  if (pct < 0.0f) {
    return 0.0f;
  }
  if (pct > 100.0f) {
    return 100.0f;
  }
  return pct;
}

void BookReadingStats::setProgressPercent(float percent) {
  if (percent < 0.0f) {
    progressPercentMilli = 0xFFFF;
    return;
  }
  if (percent > 100.0f) {
    percent = 100.0f;
  }
  progressPercentMilli = static_cast<uint16_t>(percent * 100.0f + 0.5f);
  if (progressPercentMilli > 10000) {
    progressPercentMilli = 10000;
  }
}

void BookReadingStats::recordForwardPageRead(uint32_t seconds) {
  if (seconds == 0) {
    return;
  }
  if (seconds > UINT16_MAX) {
    seconds = UINT16_MAX;
  }

  const uint16_t sample = static_cast<uint16_t>(seconds);
  if (paceSampleCount == 0 || avgSecondsPerForwardPage == 0) {
    avgSecondsPerForwardPage = sample;
    paceSampleCount = 1;
    return;
  }

  const uint16_t weight = paceSampleCount < MAX_PACE_SAMPLE_COUNT ? paceSampleCount : MAX_PACE_SAMPLE_COUNT;
  const uint32_t nextAverage =
      (static_cast<uint32_t>(avgSecondsPerForwardPage) * weight + sample) / (static_cast<uint32_t>(weight) + 1U);
  avgSecondsPerForwardPage = static_cast<uint16_t>(nextAverage);
  if (paceSampleCount < MAX_PACE_SAMPLE_COUNT) {
    paceSampleCount++;
  }
}

void BookReadingStats::recordReadingSpan(const ReadingStatsDateTime& localStart, const uint32_t seconds) {
  recordReadingSpanIntoBuckets(timeOfDaySeconds, dayOfWeekSeconds, localStart, seconds);
}

void BookReadingStats::formatDuration(uint32_t seconds, char* buf, size_t len) {
  if (seconds < 60) {
    snprintf(buf, len, "%s", tr(STR_STATS_LESS_THAN_MIN));
    return;
  }
  const uint32_t hours = seconds / 3600;
  const uint32_t minutes = (seconds % 3600) / 60;
  if (hours == 0) {
    snprintf(buf, len, "%lu min", static_cast<unsigned long>(minutes));
  } else {
    snprintf(buf, len, "%luh %lu min", static_cast<unsigned long>(hours), static_cast<unsigned long>(minutes));
  }
}

void BookReadingStats::save(const std::string& cachePath) const {
  if (cachePath.empty()) {
    LOG_ERR("STATS", "save: empty cache path");
    return;
  }
  ensureCacheDir(cachePath);
  const std::string statsFileName = statsFileNameForVersion(STATS_FILE_VERSION);
  const std::string fullPath = cachePath + "/" + statsFileName;
  HalFile f;
  if (!Storage.openFileForWrite("STATS", fullPath, f)) {
    LOG_ERR("STATS", "Could not write %s", fullPath.c_str());
    return;
  }
  uint8_t data[STATS_FILE_SIZE];
  memset(data, 0, sizeof(data));
  data[0] = STATS_FILE_VERSION;
  writeLe16(data, 1, sessionCount);
  writeLe32(data, 3, totalReadingSeconds);
  writeLe32(data, 7, totalPagesTurned);
  data[11] = isCompleted ? 1 : 0;
  writeLe16(data, 12, avgSecondsPerForwardPage);
  writeLe16(data, 14, paceSampleCount);
  data[16] = (startDateManual ? FLAG_START_DATE_MANUAL : 0u) | (finishedDateManual ? FLAG_FINISHED_DATE_MANUAL : 0u);
  writeLe16(data, 17, startDate.isValid() ? startDate.year : 0);
  data[19] = startDate.isValid() ? startDate.month : 0;
  data[20] = startDate.isValid() ? startDate.day : 0;
  writeLe16(data, 21, finishedDate.isValid() ? finishedDate.year : 0);
  data[23] = finishedDate.isValid() ? finishedDate.month : 0;
  data[24] = finishedDate.isValid() ? finishedDate.day : 0;
  for (size_t i = 0; i < timeOfDaySeconds.size(); ++i) {
    writeLe32(data, 25 + static_cast<int>(i) * 4, timeOfDaySeconds[i]);
  }
  for (size_t i = 0; i < dayOfWeekSeconds.size(); ++i) {
    writeLe32(data, 41 + static_cast<int>(i) * 4, dayOfWeekSeconds[i]);
  }
  writeLe32(data, 69, estimatedTimeLeftSeconds);
  writeLe16(data, 73, progressPercentMilli);
  const size_t written = f.write(data, STATS_FILE_SIZE);
  f.flush();
  f.close();
  if (written != STATS_FILE_SIZE) {
    LOG_ERR("STATS", "Short write %s (%u/%d)", fullPath.c_str(), static_cast<unsigned>(written), STATS_FILE_SIZE);
    return;
  }
  // Keep memo warm for this book only — do not wipe other books (Home resume).
  memoUpdateByCachePath(cachePath, *this);
  LOG_INF("STATS", "Saved %s progressMilli=%u (%.1f%%)", fullPath.c_str(), static_cast<unsigned>(progressPercentMilli),
          progressPercentMilli == 0xFFFF ? -1.0 : static_cast<double>(progressPercentMilli) / 100.0);
}

bool BookReadingStats::remove(const std::string& cachePath) {
  if (cachePath.empty()) {
    return true;
  }
  bool ok = true;

  auto removeOne = [&](const std::string& fileName) {
    const std::string path = cachePath + "/" + fileName;
    if (!Storage.exists(path.c_str())) {
      return;
    }
    if (!Storage.remove(path.c_str())) {
      LOG_ERR("STATS", "Could not delete %s", fileName.c_str());
      ok = false;
    }
  };

  for (int v = static_cast<int>(STATS_FILE_VERSION); v >= 1; --v) {
    removeOne(statsFileNameForVersion(static_cast<uint8_t>(v)));
  }
  removeOne(LEGACY_STATS_FILE_NAME);

  // Catch any other stats*.bin left by forks / renames.
  auto dir = Storage.open(cachePath.c_str());
  if (dir && dir.isDirectory()) {
    char name[96];
    for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
      const bool isDir = file.isDirectory();
      file.getName(name, sizeof(name));
      file.close();
      if (isDir || !isStatsFileName(name)) continue;
      removeOne(name);
    }
  }
  if (dir) dir.close();

  return ok;
}

bool BookReadingStats::removeForBook(const std::string& bookPath) {
  constexpr size_t kMaxPaths = 4;
  std::string paths[kMaxPaths];
  size_t pathCount = 0;
  collectBookCacheCandidates(bookPath, paths, pathCount, kMaxPaths);
  bool ok = true;
  for (size_t i = 0; i < pathCount; ++i) {
    if (!remove(paths[i])) {
      ok = false;
    }
  }
  memoInvalidate(bookPath);
  return ok;
}
