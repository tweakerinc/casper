#pragma once

// All Casper field logs / crash dumps live under this hidden SD folder.
// Never write logs to the volume root (e.g. /qr_timing.log).
namespace CasperLogPaths {
inline constexpr const char* kDir = "/.casper-logs";
inline constexpr const char* kIndex = "/.casper-logs/index.txt";
inline constexpr const char* kSystemLogPattern = "/.casper-logs/log_%05u.log";  // snprintf
inline constexpr const char* kQrTiming = "/.casper-logs/qr_timing.log";
inline constexpr const char* kCrashReport = "/.casper-logs/crash_report.txt";
// Legacy mistaken root path — migrate then remove if found.
inline constexpr const char* kLegacyRootQrTiming = "/qr_timing.log";
}  // namespace CasperLogPaths
