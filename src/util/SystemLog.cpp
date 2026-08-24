#include "util/SystemLog.h"

#include <Arduino.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <common/FsApiConstants.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "CrossPointSettings.h"
#include "util/CrossPointLogPaths.h"

namespace SystemLog {
namespace {

// Cap per file keeps open/write snappy even on huge cards.
constexpr size_t kMaxFileBytes = 2u * 1024u * 1024u;  // 2 MiB
constexpr size_t kBufCapacity = 2048;
constexpr uint32_t kFlushIntervalMs = 4000;
constexpr uint32_t kHeapSampleIntervalMs = 60000;
// After armHangWatch: denser heartbeats so freezes land between known ticks.
constexpr uint32_t kHangWatchIntervalMs = 5000;
constexpr uint32_t kHangWatchDurationMs = 120000;

char gBuf[kBufCapacity];
size_t gBufLen = 0;
uint32_t gLastFlushMs = 0;
uint32_t gLastHeapSampleMs = 0;
uint32_t gSessionT0 = 0;
uint32_t gHangWatchUntilMs = 0;
char gHangWatchReason[28] = "";
uint16_t gFileIndex = 1;
size_t gFileBytes = 0;
bool gOpen = false;
bool gEnabled = false;
uint8_t gLevel = 0;
char gPath[48] = "/.crosspoint-logs/log_00001.log";

// Compact name for field logs (id still logged for exact SETTINGS.uiTheme).
const char* themeNameForId(const uint8_t id) {
  switch (static_cast<CrossPointSettings::UI_THEME>(id)) {
    case CrossPointSettings::UI_THEME::CLASSIC:
      return "classic";
    case CrossPointSettings::UI_THEME::LYRA:
      return "lyra";
    case CrossPointSettings::UI_THEME::LYRA_3_COVERS:
      return "lyra3";
    case CrossPointSettings::UI_THEME::ROUNDEDRAFF:
      return "roundedraff";
    case CrossPointSettings::UI_THEME::MINIMAL:
      return "minimal";
    case CrossPointSettings::UI_THEME::STATS_LIFE:
      return "stats_life";
    case CrossPointSettings::UI_THEME::LYRA_CAROUSEL:
      return "lyra_carousel";
    case CrossPointSettings::UI_THEME::DASHBOARD_MAGAZINE:
      return "dash_magazine";
    case CrossPointSettings::UI_THEME::DASHBOARD_CARD:
      return "dash_card";
    case CrossPointSettings::UI_THEME::BARE:
      return "bare";
    case CrossPointSettings::UI_THEME::DASHBOARD_RECENTS:
      return "dash_recents";
    case CrossPointSettings::UI_THEME::DASHBOARD_SCROLL:
      return "dash_scroll";
    case CrossPointSettings::UI_THEME::STATS:
      return "stats";
    case CrossPointSettings::UI_THEME::PENUMBRA:
      return "penumbra";
    case CrossPointSettings::UI_THEME::GHOST:
      return "ghost";
    default:
      return "other";
  }
}

void buildPath(const uint16_t index) {
  snprintf(gPath, sizeof(gPath), CrossPointLogPaths::kSystemLogPattern, static_cast<unsigned>(index));
}

bool readIndexFile() {
  HalFile f;
  if (!Storage.openFileForRead("SLOG", CrossPointLogPaths::kIndex, f)) return false;
  char buf[16] = {};
  const int n = f.read(buf, sizeof(buf) - 1);
  f.close();
  if (n <= 0) return false;
  const long v = strtol(buf, nullptr, 10);
  if (v < 1 || v > 99999) return false;
  gFileIndex = static_cast<uint16_t>(v);
  return true;
}

void writeIndexFile() {
  char buf[16];
  snprintf(buf, sizeof(buf), "%u\n", static_cast<unsigned>(gFileIndex));
  Storage.writeFile(CrossPointLogPaths::kIndex, String(buf));
}

size_t fileSizeIfExists(const char* path) {
  if (!Storage.exists(path)) return 0;
  HalFile f = Storage.open(path, O_RDONLY);
  if (!f) return 0;
  const size_t sz = f.size();
  f.close();
  return sz;
}

void openForAppend() {
  buildPath(gFileIndex);
  gFileBytes = fileSizeIfExists(gPath);
  gOpen = true;
}

void rotateIfNeeded(const size_t upcoming) {
  if (gFileBytes + gBufLen + upcoming < kMaxFileBytes) return;
  // Flush current buffer into the old file first, then start a new one.
  if (gBufLen > 0 && gOpen) {
    HalFile f = Storage.open(gPath, static_cast<oflag_t>(O_WRONLY | O_CREAT | O_APPEND));
    if (f) {
      f.write(gBuf, gBufLen);
      f.close();
      gFileBytes += gBufLen;
    }
    gBufLen = 0;
  }
  if (gFileIndex < 99999) gFileIndex++;
  writeIndexFile();
  openForAppend();
  // Tiny header on the new file.
  char hdr[96];
  snprintf(hdr, sizeof(hdr), "==== crosspoint system log file %05u (continued) ====\n",
           static_cast<unsigned>(gFileIndex));
  HalFile f = Storage.open(gPath, static_cast<oflag_t>(O_WRONLY | O_CREAT | O_APPEND));
  if (f) {
    f.print(hdr);
    gFileBytes += strlen(hdr);
    f.close();
  }
}

void flushUnlocked() {
  if (!gEnabled || gBufLen == 0 || !gOpen) {
    gBufLen = 0;
    return;
  }
  rotateIfNeeded(0);
  HalFile f = Storage.open(gPath, static_cast<oflag_t>(O_WRONLY | O_CREAT | O_APPEND));
  if (!f) {
    // Drop buffer rather than blocking forever — logging must not brick sleep/wake.
    gBufLen = 0;
    return;
  }
  f.write(reinterpret_cast<const uint8_t*>(gBuf), gBufLen);
  f.close();
  gFileBytes += gBufLen;
  gBufLen = 0;
  gLastFlushMs = millis();
}

void appendLine(const char* line) {
  if (!gEnabled || !line) return;
  const size_t n = strlen(line);
  if (n == 0) return;

  // Large single line: flush and write directly (rare).
  if (n + 1 >= kBufCapacity) {
    flushUnlocked();
    rotateIfNeeded(n + 1);
    HalFile f = Storage.open(gPath, static_cast<oflag_t>(O_WRONLY | O_CREAT | O_APPEND));
    if (f) {
      f.print(line);
      if (line[n - 1] != '\n') f.print("\n");
      gFileBytes += n + 1;
      f.close();
    }
    return;
  }

  if (gBufLen + n + 1 >= kBufCapacity) {
    flushUnlocked();
  }
  rotateIfNeeded(n + 1);
  memcpy(gBuf + gBufLen, line, n);
  gBufLen += n;
  if (n == 0 || line[n - 1] != '\n') {
    gBuf[gBufLen++] = '\n';
  }

  // Time-based flush so multi-hour captures survive a crash reasonably well.
  if (millis() - gLastFlushMs >= kFlushIntervalMs) {
    flushUnlocked();
  }
}

}  // namespace

void begin() {
  gLevel = SETTINGS.systemLogLevel;
  if (gLevel >= CrossPointSettings::SYSTEM_LOG_LEVEL_COUNT) {
    gLevel = CrossPointSettings::SYSTEM_LOG_OFF;
  }
  gEnabled = (gLevel != CrossPointSettings::SYSTEM_LOG_OFF);
  gBufLen = 0;
  gSessionT0 = millis();
  gLastFlushMs = gSessionT0;
  gLastHeapSampleMs = gSessionT0;
  gOpen = false;

  if (!gEnabled) return;
  if (!Storage.ready()) {
    gEnabled = false;
    return;
  }

  if (!Storage.exists(CrossPointLogPaths::kDir) && Storage.exists(CrossPointLogPaths::kLegacyCasperDir)) {
    Storage.rename(CrossPointLogPaths::kLegacyCasperDir, CrossPointLogPaths::kDir);
  }
  if (!Storage.ensureDirectoryExists(CrossPointLogPaths::kDir)) {
    gEnabled = false;
    return;
  }
  // Sweep a mistaken root qr_timing.log if an older build left one.
  if (Storage.exists(CrossPointLogPaths::kLegacyRootQrTiming)) {
    if (Storage.exists(CrossPointLogPaths::kQrTiming)) {
      Storage.remove(CrossPointLogPaths::kLegacyRootQrTiming);
    } else {
      Storage.rename(CrossPointLogPaths::kLegacyRootQrTiming, CrossPointLogPaths::kQrTiming);
    }
  }
  if (!readIndexFile()) {
    gFileIndex = 1;
    writeIndexFile();
  }
  // If the current file is already huge, start a new one immediately.
  buildPath(gFileIndex);
  gFileBytes = fileSizeIfExists(gPath);
  if (gFileBytes >= kMaxFileBytes) {
    if (gFileIndex < 99999) gFileIndex++;
    writeIndexFile();
    gFileBytes = 0;
  }
  openForAppend();

  char hdr[256];
  snprintf(hdr, sizeof(hdr),
           "\n==== SYSTEM LOG SESSION ====\n"
           "t0=%lu device=%s level=%u aa=%u anti_ghost=%d theme=%u(%s) ver=%s build=%s file=%s\n",
           static_cast<unsigned long>(gSessionT0), gpio.deviceIsX3() ? "X3" : "X4", static_cast<unsigned>(gLevel),
           static_cast<unsigned>(SETTINGS.textAntiAliasing), SETTINGS.getRefreshFrequency(),
           static_cast<unsigned>(SETTINGS.uiTheme), themeNameForId(SETTINGS.uiTheme),
#ifdef CROSSPOINT_VERSION
           CROSSPOINT_VERSION,
#else
           "?",
#endif
  // ver= is the product version and repeats across builds; build= is the
  // commit that produced this firmware. Without it a capture cannot be
  // matched to a build, which cost several rounds of guesswork.
#ifdef CROSSPOINT_BUILD_ID
           CROSSPOINT_BUILD_ID,
#else
           "?",
#endif
           gPath);
  appendLine(hdr);
  flushUnlocked();
}

void reloadLevel() {
  const uint8_t prev = gLevel;
  gLevel = SETTINGS.systemLogLevel;
  if (gLevel >= CrossPointSettings::SYSTEM_LOG_LEVEL_COUNT) gLevel = CrossPointSettings::SYSTEM_LOG_OFF;
  const bool want = (gLevel != CrossPointSettings::SYSTEM_LOG_OFF);
  if (want && !gEnabled) {
    begin();
    return;
  }
  if (!want && gEnabled) {
    log("SYS", "LOG_STOP (disabled in settings)");
    end();
    return;
  }
  if (want && prev != gLevel) {
    log("SYS", "LOG_LEVEL_CHANGE %u -> %u", static_cast<unsigned>(prev), static_cast<unsigned>(gLevel));
  }
}

void log(const char* tag, const char* fmt, ...) {
  if (!gEnabled) return;
  char body[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(body, sizeof(body), fmt, ap);
  va_end(ap);

  char line[200];
  const uint32_t dt = millis() - gSessionT0;
  snprintf(line, sizeof(line), "+%6lums | %-6s | %s", static_cast<unsigned long>(dt), tag ? tag : "?", body);
  appendLine(line);
}

void logTiming(const char* tag, const char* fmt, ...) {
  if (!timingEnabled()) return;
  char body[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(body, sizeof(body), fmt, ap);
  va_end(ap);
  log(tag, "%s", body);
}

void logVerbose(const char* tag, const char* fmt, ...) {
  if (!verboseEnabled()) return;
  char body[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(body, sizeof(body), fmt, ap);
  va_end(ap);
  log(tag, "%s", body);
}

void logTimed(const char* tag, const uint32_t durationMs, const char* fmt, ...) {
  if (!timingEnabled()) return;
  char body[140];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(body, sizeof(body), fmt, ap);
  va_end(ap);
  log(tag, "took=%lums | %s", static_cast<unsigned long>(durationMs), body);
}

void flush() {
  if (!gEnabled) return;
  flushUnlocked();
}

void end() {
  if (!gEnabled) return;
  log("SYS", "LOG_END");
  flushUnlocked();
  gEnabled = false;
  gOpen = false;
}

bool enabled() { return gEnabled; }
bool timingEnabled() { return gEnabled && gLevel >= CrossPointSettings::SYSTEM_LOG_TIMING; }
bool verboseEnabled() { return gEnabled && gLevel >= CrossPointSettings::SYSTEM_LOG_VERBOSE; }

void maybeSampleHeap() {
  if (!timingEnabled()) return;
  const uint32_t now = millis();
  const bool hangWatch = (gHangWatchUntilMs != 0 && now < gHangWatchUntilMs);
  const uint32_t interval = hangWatch ? kHangWatchIntervalMs : kHeapSampleIntervalMs;
  if (now - gLastHeapSampleMs < interval) return;
  gLastHeapSampleMs = now;
  if (hangWatch) {
    log("ALIVE", "watch=%s free=%u min=%u maxAlloc=%u", gHangWatchReason[0] ? gHangWatchReason : "?",
        static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMinFreeHeap()),
        static_cast<unsigned>(ESP.getMaxAllocHeap()));
    flush();  // survive hard freeze within the next 5s window
  } else {
    if (gHangWatchUntilMs != 0 && now >= gHangWatchUntilMs) {
      gHangWatchUntilMs = 0;
      gHangWatchReason[0] = '\0';
    }
    log("MEM", "free=%u min=%u maxAlloc=%u", static_cast<unsigned>(ESP.getFreeHeap()),
        static_cast<unsigned>(ESP.getMinFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
  }
}

void armHangWatch(const char* reason) {
  if (!timingEnabled()) return;
  gHangWatchUntilMs = millis() + kHangWatchDurationMs;
  gLastHeapSampleMs = 0;  // next maybeSampleHeap fires ASAP
  if (reason && reason[0]) {
    snprintf(gHangWatchReason, sizeof(gHangWatchReason), "%s", reason);
  } else {
    snprintf(gHangWatchReason, sizeof(gHangWatchReason), "watch");
  }
  log("ALIVE", "arm watch=%s for=%lums fre=%u", gHangWatchReason, static_cast<unsigned long>(kHangWatchDurationMs),
      static_cast<unsigned>(ESP.getFreeHeap()));
  flush();
}

void disarmHangWatch() {
  if (gHangWatchUntilMs == 0 && gHangWatchReason[0] == '\0') return;
  gHangWatchUntilMs = 0;
  gHangWatchReason[0] = '\0';
}

void logCritical(const char* tag, const char* fmt, ...) {
  if (!timingEnabled()) return;
  char body[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(body, sizeof(body), fmt, ap);
  va_end(ap);
  log(tag ? tag : "CRIT", "%s", body);
  flush();
}

void logThemeChange(const uint8_t fromId, const uint8_t toId, const uint32_t reloadMs) {
  if (!timingEnabled()) return;
  log("THEME", "change %u(%s) -> %u(%s) reload=%lums fre=%u", static_cast<unsigned>(fromId), themeNameForId(fromId),
      static_cast<unsigned>(toId), themeNameForId(toId), static_cast<unsigned long>(reloadMs),
      static_cast<unsigned>(ESP.getFreeHeap()));
  // Flush so a slow-theme session that reboots mid-test still captures the switch.
  flush();
}

}  // namespace SystemLog
