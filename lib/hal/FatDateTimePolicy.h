#pragma once

#include <cstdint>

// FAT timestamps and crash-report wall clocks. Field logs showed
// "2 years ago" (callback fallback 2024-01-01) and "4 weeks ago" (PCF8563
// still running a date from before this firmware was built, or VL-unset
// so getDateTime failed while the Home clock still showed HH:MM from a
// time-only cache).
namespace fatdate {

inline constexpr uint16_t kMaxFatYear = 2107;

// Years before the firmware's compile year-1 are junk (unset PCF8563, old
// kFallbackYear=2024, century-bit leftovers). 2025 is kept when this binary
// was built in 2026 so a clock that is merely a few months slow still works.
inline constexpr uint16_t minPlausibleYear(const uint16_t compileYear) {
  return compileYear > 1 ? static_cast<uint16_t>(compileYear - 1) : compileYear;
}

inline constexpr bool yearIsPlausible(const uint16_t year, const uint16_t compileYear) {
  return year >= minPlausibleYear(compileYear) && year <= kMaxFatYear;
}

inline constexpr uint16_t yearFromDateString(const char* date) {
  // __DATE__ is "Mmm dd yyyy" (day may be space-padded).
  uint16_t y = 0;
  if (!date) return 2026;
  int n = 0;
  while (date[n] != '\0') ++n;
  if (n < 4) return 2026;
  const char* p = date + (n - 4);
  for (int i = 0; i < 4; ++i) {
    if (p[i] < '0' || p[i] > '9') return 2026;
    y = static_cast<uint16_t>(y * 10 + (p[i] - '0'));
  }
  return y >= 1980 ? y : 2026;
}

inline constexpr uint8_t monthFromDateString(const char* date) {
  if (!date || date[0] == '\0' || date[1] == '\0' || date[2] == '\0') return 1;
  const char a = date[0], b = date[1], c = date[2];
  if (a == 'J' && b == 'a' && c == 'n') return 1;
  if (a == 'F' && b == 'e' && c == 'b') return 2;
  if (a == 'M' && b == 'a' && c == 'r') return 3;
  if (a == 'A' && b == 'p' && c == 'r') return 4;
  if (a == 'M' && b == 'a' && c == 'y') return 5;
  if (a == 'J' && b == 'u' && c == 'n') return 6;
  if (a == 'J' && b == 'u' && c == 'l') return 7;
  if (a == 'A' && b == 'u' && c == 'g') return 8;
  if (a == 'S' && b == 'e' && c == 'p') return 9;
  if (a == 'O' && b == 'c' && c == 't') return 10;
  if (a == 'N' && b == 'o' && c == 'v') return 11;
  if (a == 'D' && b == 'e' && c == 'c') return 12;
  return 1;
}

inline constexpr uint8_t dayFromDateString(const char* date) {
  if (!date) return 1;
  int n = 0;
  while (date[n] != '\0') ++n;
  if (n < 6) return 1;
  const char tens = date[4];
  const char ones = date[5];
  uint8_t d = 0;
  if (tens >= '0' && tens <= '9') d = static_cast<uint8_t>((tens - '0') * 10);
  if (ones >= '0' && ones <= '9') d = static_cast<uint8_t>(d + (ones - '0'));
  if (d < 1 || d > 31) return 1;
  return d;
}

struct CivilTime {
  uint16_t year;
  uint8_t month;
  uint8_t day;
  uint8_t hour;
  uint8_t minute;
};

inline constexpr CivilTime firmwareFallback(const char* date) {
  return CivilTime{yearFromDateString(date), monthFromDateString(date), dayFromDateString(date), 0, 0};
}

inline constexpr int packedDate(const uint16_t year, const uint8_t month, const uint8_t day) {
  return static_cast<int>(year) * 10000 + static_cast<int>(month) * 100 + static_cast<int>(day);
}

// True when the chip's calendar day is earlier than this binary's compile date.
// After a flash that is the "4 weeks ago" field report: RTC kept running from
// an older seed while FAT/crash stamps used that stale day.
inline constexpr bool rtcBehindFirmware(const uint16_t rtcYear, const uint8_t rtcMonth, const uint8_t rtcDay,
                                        const uint16_t fwYear, const uint8_t fwMonth, const uint8_t fwDay) {
  return packedDate(rtcYear, rtcMonth, rtcDay) < packedDate(fwYear, fwMonth, fwDay);
}

inline constexpr bool shouldSeedRtc(const bool rtcPresent, const bool readOk, const uint16_t rtcYear,
                                    const uint8_t rtcMonth, const uint8_t rtcDay, const CivilTime fw) {
  if (!rtcPresent) return false;
  if (!readOk) return true;
  if (!yearIsPlausible(rtcYear, fw.year)) return true;
  return rtcBehindFirmware(rtcYear, rtcMonth, rtcDay, fw.year, fw.month, fw.day);
}

}  // namespace fatdate
