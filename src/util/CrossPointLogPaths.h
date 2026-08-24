#pragma once

// All CrossPoint field logs / crash dumps live under this hidden SD folder.
// Never write logs to the volume root (e.g. /qr_timing.log).
namespace CrossPointLogPaths {
inline constexpr const char* kDir = "/.crosspoint-logs";
inline constexpr const char* kIndex = "/.crosspoint-logs/index.txt";
inline constexpr const char* kSystemLogPattern = "/.crosspoint-logs/log_%05u.log";  // snprintf
inline constexpr const char* kQrTiming = "/.crosspoint-logs/qr_timing.log";
inline constexpr const char* kCrashReport = "/.crosspoint-logs/crash_report.txt";
// Leftover Casper-branded log folder — rename once if the CrossPoint dir is absent.
inline constexpr const char* kLegacyCasperDir = "/.casper-logs";
// Legacy mistaken root path — migrate then remove if found.
inline constexpr const char* kLegacyRootQrTiming = "/qr_timing.log";
}  // namespace CrossPointLogPaths
