#include "HalClock.h"

#include "FatDateTimePolicy.h"

#include <Logging.h>
#include <WiFi.h>
#include <esp_attr.h>
#include <esp_sntp.h>
#include <time.h>

HalClock halClock;  // Singleton instance

namespace {
constexpr uint32_t kClockStampMagic = 0xC10CDA7Eu;
RTC_NOINIT_ATTR uint32_t gClockStampMagic;
RTC_NOINIT_ATTR uint16_t gStampYear;
RTC_NOINIT_ATTR uint8_t gStampMonth;
RTC_NOINIT_ATTR uint8_t gStampDay;
RTC_NOINIT_ATTR uint8_t gStampHour;
RTC_NOINIT_ATTR uint8_t gStampMinute;

void rememberStamp(const uint16_t year, const uint8_t month, const uint8_t day, const uint8_t hour,
                   const uint8_t minute) {
  gStampYear = year;
  gStampMonth = month;
  gStampDay = day;
  gStampHour = hour;
  gStampMinute = minute;
  gClockStampMagic = kClockStampMagic;
}

bool recallStamp(uint16_t& year, uint8_t& month, uint8_t& day, uint8_t& hour, uint8_t& minute) {
  if (gClockStampMagic != kClockStampMagic) return false;
  const auto fw = fatdate::firmwareFallback(__DATE__);
  if (!fatdate::yearIsPlausible(gStampYear, fw.year)) return false;
  year = gStampYear;
  month = gStampMonth;
  day = gStampDay;
  hour = gStampHour;
  minute = gStampMinute;
  return true;
}

uint16_t normalizeYear(uint16_t year) {
  if (year >= 1900 && year < 2000) {
    return static_cast<uint16_t>(2000 + (year % 100));
  }
  if (year < 100) {
    return static_cast<uint16_t>(2000 + year);
  }
  return year;
}
}  // namespace

void HalClock::begin() {
  _available = _sdkRtc.begin();
  LOG_INF("CLK", _available ? "SDK RTC found" : "RTC not found");

  const auto fw = fatdate::firmwareFallback(__DATE__);
  Rtc::DateTime dt{};
  bool readOk = false;
  if (_available) {
    for (int i = 0; i < 3 && !readOk; ++i) {
      if (_sdkRtc.now(dt)) {
        readOk = true;
      } else if (i < 2) {
        delay(20);
      }
    }
  }
  if (readOk) {
    dt.year = normalizeYear(dt.year);
  }
  if (fatdate::shouldSeedRtc(_available, readOk, dt.year, dt.month, dt.day, fw)) {
    if (readOk) {
      // Keep hour/minute; only the calendar day was stale/junk.
      dt.year = fw.year;
      dt.month = fw.month;
      dt.day = fw.day;
    } else {
      dt.year = fw.year;
      dt.month = fw.month;
      dt.day = fw.day;
      dt.hour = 0;
      dt.minute = 0;
      dt.second = 0;
      dt.weekday = 0;
    }
    if (_available && _sdkRtc.set(dt)) {
      LOG_INF("CLK", "RTC seeded from firmware %04u-%02u-%02u (was %s)", static_cast<unsigned>(dt.year),
              static_cast<unsigned>(dt.month), static_cast<unsigned>(dt.day), readOk ? "behind/invalid" : "unset/VL");
      readOk = true;
    } else if (!_available) {
      LOG_INF("CLK", "No RTC — civil time falls back to firmware %04u-%02u-%02u", static_cast<unsigned>(fw.year),
              static_cast<unsigned>(fw.month), static_cast<unsigned>(fw.day));
    } else {
      LOG_ERR("CLK", "RTC seed write failed");
    }
  }

  if (readOk) {
    const unsigned long now = millis();
    _cachedYear = dt.year;
    _cachedMonth = dt.month;
    _cachedDay = dt.day;
    _cachedHour = dt.hour;
    _cachedMinute = dt.minute;
    _cachedWeekday = static_cast<uint8_t>(dt.weekday % 7U);
    _lastPollMs = now;
    _lastDatePollMs = now;
    _hasCachedTime = true;
    _hasCachedDateTime = true;
    rememberStamp(_cachedYear, _cachedMonth, _cachedDay, _cachedHour, _cachedMinute);
  }
}

bool HalClock::getTime(uint8_t& hour, uint8_t& minute) const {
  if (!_available) return false;

  const unsigned long now = millis();
  if (_lastPollMs != 0 && (now - _lastPollMs) < CLOCK_POLL_MS) {
    hour = _cachedHour;
    minute = _cachedMinute;
    return true;
  }

  Rtc::DateTime dt;
  if (!_sdkRtc.now(dt)) {
    if (!_hasCachedTime) return false;
    _lastPollMs = now;
    hour = _cachedHour;
    minute = _cachedMinute;
    return true;
  }
  _cachedHour = dt.hour;
  _cachedMinute = dt.minute;
  _cachedYear = normalizeYear(dt.year);
  _cachedMonth = dt.month;
  _cachedDay = dt.day;
  _cachedWeekday = static_cast<uint8_t>(dt.weekday % 7U);
  _lastPollMs = now;
  _lastDatePollMs = now;
  _hasCachedTime = true;
  _hasCachedDateTime = true;
  rememberStamp(_cachedYear, _cachedMonth, _cachedDay, _cachedHour, _cachedMinute);
  hour = _cachedHour;
  minute = _cachedMinute;
  return true;
}

bool HalClock::getDateTime(uint16_t& year, uint8_t& month, uint8_t& day, uint8_t& hour, uint8_t& minute,
                           uint8_t* weekday) const {
  if (!_available) return false;

  // Cache full date/time like getTime(). SD FAT timestamps call this on every
  // file write — without caching a flaky PCF8563 was hit constantly and could
  // hang the UI (especially on battery before Wire had a timeout).
  const unsigned long now = millis();
  if (_hasCachedDateTime && _lastDatePollMs != 0 && (now - _lastDatePollMs) < CLOCK_POLL_MS) {
    year = _cachedYear;
    month = _cachedMonth;
    day = _cachedDay;
    hour = _cachedHour;
    minute = _cachedMinute;
    if (weekday) *weekday = _cachedWeekday;
    return true;
  }

  Rtc::DateTime dt;
  if (!_sdkRtc.now(dt)) {
    // Soft-fail: serve last good sample so FAT/Penumbra keep working if one
    // I2C poll times out.
    if (!_hasCachedDateTime) return false;
    year = _cachedYear;
    month = _cachedMonth;
    day = _cachedDay;
    hour = _cachedHour;
    minute = _cachedMinute;
    if (weekday) *weekday = _cachedWeekday;
    _lastDatePollMs = now;
    return true;
  }

  year = normalizeYear(dt.year);
  month = dt.month;
  day = dt.day;
  hour = dt.hour;
  minute = dt.minute;
  const uint8_t wd = static_cast<uint8_t>(dt.weekday % 7U);
  if (weekday) {
    // Hardware weekday is 0=Sunday .. 6=Saturday (Rtc.h). Clamp bad BCD.
    *weekday = wd;
  }

  _cachedHour = dt.hour;
  _cachedMinute = dt.minute;
  _cachedYear = year;
  _cachedMonth = month;
  _cachedDay = day;
  _cachedWeekday = wd;
  _lastPollMs = now;
  _lastDatePollMs = now;
  _hasCachedTime = true;
  _hasCachedDateTime = true;
  rememberStamp(year, month, day, hour, minute);
  return true;
}

void HalClock::getDateTimeOrFallback(uint16_t& year, uint8_t& month, uint8_t& day, uint8_t& hour, uint8_t& minute,
                                     const char** sourceOut) const {
  if (getDateTime(year, month, day, hour, minute)) {
    if (sourceOut) *sourceOut = "rtc";
    return;
  }
  if (recallStamp(year, month, day, hour, minute)) {
    if (sourceOut) *sourceOut = "last-good";
    return;
  }
  const auto fw = fatdate::firmwareFallback(__DATE__);
  year = fw.year;
  month = fw.month;
  day = fw.day;
  hour = fw.hour;
  minute = fw.minute;
  if (sourceOut) *sourceOut = "firmware";
}

bool HalClock::formatTime(char* buf, size_t bufSize, uint8_t utcOffsetQuarterHoursBiased, bool use12Hour) const {
  if (bufSize < (use12Hour ? 9u : 6u)) return false;
  uint8_t h, m;
  if (!getTime(h, m)) return false;

  // Apply UTC offset: convert biased value to signed quarter-hours.
  // Clamp against corrupted persisted values so display time can't drift outside [-12:00, +14:00].
  if (utcOffsetQuarterHoursBiased > 104) utcOffsetQuarterHoursBiased = 104;
  int offsetQuarterHours = static_cast<int>(utcOffsetQuarterHoursBiased) - 48;
  int totalMinutes = static_cast<int>(h) * 60 + static_cast<int>(m) + offsetQuarterHours * 15;

  // Wrap around 24 hours
  totalMinutes = ((totalMinutes % 1440) + 1440) % 1440;

  const int hour24 = totalMinutes / 60;
  const int min = totalMinutes % 60;
  if (use12Hour) {
    const bool pm = hour24 >= 12;
    int hour12 = hour24 % 12;
    if (hour12 == 0) hour12 = 12;
    snprintf(buf, bufSize, "%d:%02d %s", hour12, min, pm ? "PM" : "AM");
  } else {
    snprintf(buf, bufSize, "%02d:%02d", hour24, min);
  }
  return true;
}

bool HalClock::syncFromNTP() {
  if (!_available) return false;

  if (WiFi.status() != WL_CONNECTED) {
    LOG_ERR("CLK", "WiFi not connected, cannot sync NTP");
    return false;
  }

  LOG_INF("CLK", "Starting NTP sync...");
  configTzTime("UTC0", "pool.ntp.org", "time.nist.gov");

  // Wait for SNTP sync to complete (up to 5 seconds)
  constexpr int maxAttempts = 50;
  for (int i = 0; i < maxAttempts; i++) {
    if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
      time_t now = time(nullptr);
      struct tm timeinfo;
      gmtime_r(&now, &timeinfo);

      Rtc::DateTime dt;
      dt.year = static_cast<uint16_t>(timeinfo.tm_year + 1900);
      dt.month = static_cast<uint8_t>(timeinfo.tm_mon + 1);
      dt.day = static_cast<uint8_t>(timeinfo.tm_mday);
      dt.hour = static_cast<uint8_t>(timeinfo.tm_hour);
      dt.minute = static_cast<uint8_t>(timeinfo.tm_min);
      dt.second = static_cast<uint8_t>(timeinfo.tm_sec);
      dt.weekday = static_cast<uint8_t>(timeinfo.tm_wday);
      if (_sdkRtc.set(dt)) {
        _lastPollMs = 0;
        _lastDatePollMs = 0;
        _cachedHour = dt.hour;
        _cachedMinute = dt.minute;
        _cachedYear = dt.year;
        _cachedMonth = dt.month;
        _cachedDay = dt.day;
        _cachedWeekday = dt.weekday;
        _hasCachedTime = true;
        _hasCachedDateTime = true;
        rememberStamp(dt.year, dt.month, dt.day, dt.hour, dt.minute);
        LOG_INF("CLK", "RTC set to %04u-%02u-%02u %02u:%02u:%02u UTC", dt.year, dt.month, dt.day, dt.hour, dt.minute,
                dt.second);
        return true;
      }
      return false;
    }
    delay(100);
  }

  LOG_ERR("CLK", "NTP sync timed out");
  return false;
}
