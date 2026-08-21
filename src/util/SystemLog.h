#pragma once

#include <cstdint>

// Buffered, rotating system log on SD: /.casper-logs/log_NNNNN.log
// Hidden folder (leading dot) for logs, QR timing, and crash reports.
// Designed for multi-hour captures without tanking page turns:
//   - lines go to a small RAM buffer
//   - flush every few seconds, when the buffer fills, or on sleep
//   - rotate to a new file when the active file exceeds ~2 MB
//
// Levels (SETTINGS.systemLogLevel) — UI is Enable Logging Off/On (On = Timing):
//   0 = Off     (default; no field logs)
//   1 = Timing  (boot, sleep/wake, open book, page turn, network, heap samples)
//   2 = Verbose (+ activity enter/exit, extra detail)
// Crash reports (/.casper-logs/crash_report.txt) are always written when a panic occurs.

namespace SystemLog {

// Call after SD + SETTINGS are ready.
void begin();

// Optional: re-read SETTINGS.systemLogLevel without reopening (e.g. after settings change).
void reloadLevel();

// Append one line: "+ms | TAG | message"
void log(const char* tag, const char* fmt, ...);

// Same, but only when level >= Timing.
void logTiming(const char* tag, const char* fmt, ...);

// Same, but only when level >= Verbose.
void logVerbose(const char* tag, const char* fmt, ...);

// Timed step: "TAG | took=Nms | message"
void logTimed(const char* tag, uint32_t durationMs, const char* fmt, ...);

// Force buffer to SD (sleep, crash path, long idle).
void flush();

// Close current session cleanly.
void end();

bool enabled();
bool timingEnabled();
bool verboseEnabled();

// Periodic heap sample (call from main loop; rate-limited inside).
// While a hang-watch is armed, samples every ~5s so freezes pin the last phase.
void maybeSampleHeap();

// After a critical paint (e.g. penumbra HALF), arm dense ALIVE ticks + force-flush
// so the next freeze leaves a clear last-known-good breadcrumb on SD.
void armHangWatch(const char* reason);
// Clear the 120s ALIVE window once the watched work finished (chapter hop).
void disarmHangWatch();

// Log + flush immediately (for pre/post display markers that must survive a hang).
void logCritical(const char* tag, const char* fmt, ...);

// Settings → Theme: log id/name and how long UITheme::reload took (slow themes stand out).
void logThemeChange(uint8_t fromId, uint8_t toId, uint32_t reloadMs);

}  // namespace SystemLog
