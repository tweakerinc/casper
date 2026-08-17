#pragma once

// On-disk roots — same layout as CrossPoint / CrossInk (/.crosspoint).

namespace CasperPaths {

inline constexpr const char* kRoot = "/.crosspoint";

// App config (PersistableStore + sidecars)
inline constexpr const char* kSettings = "/.crosspoint/settings.json";
inline constexpr const char* kState = "/.crosspoint/state.json";
inline constexpr const char* kRecent = "/.crosspoint/recent.json";
inline constexpr const char* kWifi = "/.crosspoint/wifi.json";
inline constexpr const char* kOpds = "/.crosspoint/opds.json";
inline constexpr const char* kButtonMap = "/.crosspoint/button_map.txt";

// Lifetime / global stats
inline constexpr const char* kGlobalStats = "/.crosspoint/global_stats.bin";
inline constexpr const char* kGlobalStatsBak = "/.crosspoint/global_stats.bin.bak";

// Libraries
inline constexpr const char* kBookmarksDir = "/.crosspoint/bookmarks/";
inline constexpr const char* kClippingsDir = "/.crosspoint/clippings";

// Transient
inline constexpr const char* kDictTmp = "/.crosspoint/dict.tmp";
inline constexpr const char* kSleepFrame = "/.crosspoint/sleep_frame.bin";
inline constexpr const char* kSleepPngTmp = "/.crosspoint/sleep_from_png.bmp";
inline constexpr const char* kOtaCache = "/.crosspoint/ota-firmware.bin";

// KOReader sync credentials
inline constexpr const char* kKoreader = "/.crosspoint/koreader.json";

// Multi-device synced stats (optional SD folder)
inline constexpr const char* kSyncedStatsDir = "/.crosspoint/synced_stats";

// Logs (Casper field diagnostics — separate from app config tree)
inline constexpr const char* kLogsDir = "/.casper-logs";
// Optional lifetime-stats copies (v0.1.8 shipped path; not under /.crosspoint).
inline constexpr const char* kStatsBackupDir = "/.casper-stats-backup";

// Package + progress/stats: /.crosspoint/epub_<std::hash>/ (shipped v0.1.8 layout).
inline constexpr const char* kPackageCacheRoot = "/.crosspoint";

}  // namespace CasperPaths
