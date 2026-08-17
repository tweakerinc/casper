#pragma once

#include <Arduino.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <common/FsApiConstants.h>

#include <cstdarg>
#include <cstdio>

#include "CasperSettings.h"
#include "util/CasperLogPaths.h"

// Append-only Quick Resume timing log: /.casper-logs/qr_timing.log only.
// Gated by Settings → Enable Logging (same as SystemLog). Crash reports are separate.

namespace QrTimingLog {

inline bool gActive = false;
inline uint32_t gT0 = 0;
inline bool gMigratedRoot = false;

// Ensure /.casper-logs is a directory. Never write logs to the volume root.
inline bool ensureLogDir() {
  if (!Storage.ensureDirectoryExists(CasperLogPaths::kDir)) {
    return false;
  }
  // One-shot: move a mistaken root /qr_timing.log into the log folder.
  if (!gMigratedRoot) {
    gMigratedRoot = true;
    if (Storage.exists(CasperLogPaths::kLegacyRootQrTiming)) {
      // Prefer keep existing nested log; just delete root junk if nested already exists.
      if (Storage.exists(CasperLogPaths::kQrTiming)) {
        Storage.remove(CasperLogPaths::kLegacyRootQrTiming);
      } else if (!Storage.rename(CasperLogPaths::kLegacyRootQrTiming, CasperLogPaths::kQrTiming)) {
        Storage.remove(CasperLogPaths::kLegacyRootQrTiming);
      }
    }
  }
  return true;
}

inline void appendRaw(const char* text) {
  if (!text || !text[0]) return;
  if (!ensureLogDir()) return;
  HalFile f = Storage.open(CasperLogPaths::kQrTiming, static_cast<oflag_t>(O_WRONLY | O_CREAT | O_APPEND));
  if (!f) return;
  f.print(text);
  f.close();
}

inline void begin(const char* reason) {
  // Gated with System → Enable Logging (same switch as SystemLog). Crash reports
  // are independent and always write.
  if (SETTINGS.systemLogLevel == CasperSettings::SYSTEM_LOG_OFF) {
    gActive = false;
    return;
  }
  gActive = true;
  gT0 = millis();
  char header[256];
  snprintf(header, sizeof(header),
           "\n==== QR timing session ====\n"
           "t0_millis=%lu reason=%s device=%s aa=%u anti_ghost_pages=%d ver=%s\n",
           static_cast<unsigned long>(gT0), reason ? reason : "?", gpio.deviceIsX3() ? "X3" : "X4",
           static_cast<unsigned>(SETTINGS.textAntiAliasing), SETTINGS.getRefreshFrequency(),
#ifdef CASPER_VERSION
           CASPER_VERSION
#else
           "?"
#endif
  );
  appendRaw(header);
  // Also mark absolute boot age (millis since reset == wall since wake).
  char boot[64];
  snprintf(boot, sizeof(boot), "+%5lums | BOOT_MARK (millis since reset)\n", static_cast<unsigned long>(millis()));
  appendRaw(boot);
}

inline void line(const char* fmt, ...) {
  if (!gActive) return;
  char body[192];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(body, sizeof(body), fmt, ap);
  va_end(ap);

  char out[240];
  const uint32_t dt = millis() - gT0;
  snprintf(out, sizeof(out), "+%5lums | %s\n", static_cast<unsigned long>(dt), body);
  appendRaw(out);
}

inline void end(const char* note = nullptr) {
  if (!gActive) return;
  if (note && note[0]) {
    line("SESSION_END %s", note);
  } else {
    line("SESSION_END");
  }
  gActive = false;
}

inline bool active() { return gActive; }

}  // namespace QrTimingLog
