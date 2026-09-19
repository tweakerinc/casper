#pragma once

#include <cstdint>

// Which ESP reset reasons dump /.crosspoint-logs/crash_report.txt.
// Field v0.2.0: cover JPEG ran 17s then rst=6 (TASK_WDT) with no report;
// a later rst=4 (PANIC) wrote the file with an empty reason and
// "Report written (RTC): unavailable".
namespace crashreport {

// ESP-IDF esp_reset_reason_t values (keep numeric so host tests need no IDF).
inline constexpr int kResetPanic = 4;
inline constexpr int kResetIntWdt = 5;
inline constexpr int kResetTaskWdt = 6;
inline constexpr int kResetWdt = 7;
inline constexpr int kResetCpuLockup = 15;

inline constexpr bool dumpOnReset(const int resetReason) {
  return resetReason == kResetPanic || resetReason == kResetIntWdt || resetReason == kResetTaskWdt ||
         resetReason == kResetWdt || resetReason == kResetCpuLockup;
}

inline constexpr const char* resetReasonName(const int resetReason) {
  switch (resetReason) {
    case 0:
      return "unknown";
    case 1:
      return "poweron";
    case 2:
      return "ext";
    case 3:
      return "sw";
    case 4:
      return "panic";
    case 5:
      return "int_wdt";
    case 6:
      return "task_wdt";
    case 7:
      return "wdt";
    case 8:
      return "deepsleep";
    case 9:
      return "brownout";
    case 15:
      return "cpu_lockup";
    default:
      return "other";
  }
}

}  // namespace crashreport
