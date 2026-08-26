#pragma once

// All CrossPoint field logs / crash dumps live under this hidden SD folder.
// Never write logs to the volume root (e.g. /qr_timing.log).
namespace CrossPointLogPaths {
inline constexpr const char* kDir = "/.crosspoint-logs";
inline constexpr const char* kIndex = "/.crosspoint-logs/index.txt";
inline constexpr const char* kSystemLogPattern = "/.crosspoint-logs/log_%05u.log";  // snprintf
inline constexpr const char* kQrTiming = "/.crosspoint-logs/qr_timing.log";
inline constexpr const char* kCrashReport = "/.crosspoint-logs/crash_report.txt";
// Visible fallback when a leading-dot folder cannot be created or written
// (some FAT hosts / Windows "hide protected files" make the hidden dir look empty).
inline constexpr const char* kVisibleDir = "/casper-logs";
inline constexpr const char* kVisibleIndex = "/casper-logs/index.txt";
inline constexpr const char* kVisibleSystemLogPattern = "/casper-logs/log_%05u.log";
inline constexpr const char* kVisibleQrTiming = "/casper-logs/qr_timing.log";
// Leftover Casper-branded log folder — rename once if the CrossPoint dir is absent.
inline constexpr const char* kLegacyCasperDir = "/.casper-logs";
// Legacy mistaken root path — migrate then remove if found.
inline constexpr const char* kLegacyRootQrTiming = "/qr_timing.log";
}  // namespace CrossPointLogPaths
