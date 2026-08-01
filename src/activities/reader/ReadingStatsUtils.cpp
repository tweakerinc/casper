#include "ReadingStatsUtils.h"

#include <HalClock.h>

#include <algorithm>
#include <cstdint>

#include "CrossPointSettings.h"

namespace {
constexpr const char* MONTH_NAMES[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                       "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

bool isBitSet(const std::array<uint8_t, READING_HISTORY_BYTES>& bits, const size_t bitIndex) {
  if (bitIndex >= READING_HISTORY_DAYS) {
    return false;
  }
  return (bits[bitIndex / 8] & static_cast<uint8_t>(1u << (bitIndex % 8))) != 0;
}

void setBit(std::array<uint8_t, READING_HISTORY_BYTES>& bits, const size_t bitIndex) {
  if (bitIndex >= READING_HISTORY_DAYS) {
    return;
  }
  bits[bitIndex / 8] |= static_cast<uint8_t>(1u << (bitIndex % 8));
}

void clearBits(std::array<uint8_t, READING_HISTORY_BYTES>& bits) { bits.fill(0); }

void shiftHistoryOlder(std::array<uint8_t, READING_HISTORY_BYTES>& bits, const size_t shiftDays) {
  if (shiftDays == 0) {
    return;
  }
  if (shiftDays >= READING_HISTORY_DAYS) {
    clearBits(bits);
    return;
  }

  std::array<uint8_t, READING_HISTORY_BYTES> shifted = {};
  for (size_t bitIndex = 0; bitIndex + shiftDays < READING_HISTORY_DAYS; ++bitIndex) {
    if (isBitSet(bits, bitIndex)) {
      setBit(shifted, bitIndex + shiftDays);
    }
  }
  bits = shifted;
}

uint32_t secondsUntilNextBucketBoundary(const ReadingStatsDateTime& dt) {
  const uint32_t currentSecondOfDay =
      static_cast<uint32_t>(dt.hour) * 3600u + static_cast<uint32_t>(dt.minute) * 60u + dt.second;
  uint32_t nextBoundarySecondOfDay = 24u * 3600u;
  if (dt.hour < 5) {
    nextBoundarySecondOfDay = 5u * 3600u;
  } else if (dt.hour < 12) {
    nextBoundarySecondOfDay = 12u * 3600u;
  } else if (dt.hour < 17) {
    nextBoundarySecondOfDay = 17u * 3600u;
  } else if (dt.hour < 21) {
    nextBoundarySecondOfDay = 21u * 3600u;
  }

  if (nextBoundarySecondOfDay <= currentSecondOfDay) {
    return 1;
  }
  return nextBoundarySecondOfDay - currentSecondOfDay;
}
}  // namespace

bool ReadingStatsDate::isValid() const { return isValidReadingStatsDate(*this); }

void ReadingStatsDate::clear() {
  year = 0;
  month = 0;
  day = 0;
}

bool ReadingStatsDateTime::isValid() const { return date.isValid(); }

bool isLeapYear(const uint16_t year) { return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0; }

uint8_t daysInMonth(const uint16_t year, const uint8_t month) {
  static constexpr uint8_t DAYS[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month < 1 || month > 12) {
    return 0;
  }
  if (month == 2 && isLeapYear(year)) {
    return 29;
  }
  return DAYS[month - 1];
}

bool isValidReadingStatsDate(const ReadingStatsDate& date) {
  if (date.year < 2000 || date.year > 2099) {
    return false;
  }
  const uint8_t monthDays = daysInMonth(date.year, date.month);
  return monthDays > 0 && date.day >= 1 && date.day <= monthDays;
}

int compareReadingStatsDate(const ReadingStatsDate& lhs, const ReadingStatsDate& rhs) {
  if (lhs.year != rhs.year) {
    return lhs.year < rhs.year ? -1 : 1;
  }
  if (lhs.month != rhs.month) {
    return lhs.month < rhs.month ? -1 : 1;
  }
  if (lhs.day != rhs.day) {
    return lhs.day < rhs.day ? -1 : 1;
  }
  return 0;
}

void addDaysToReadingStatsDate(ReadingStatsDate& date, int delta) {
  if (!date.isValid() || delta == 0) {
    return;
  }

  while (delta > 0) {
    const uint8_t monthDays = daysInMonth(date.year, date.month);
    if (date.day < monthDays) {
      date.day++;
    } else {
      date.day = 1;
      if (date.month < 12) {
        date.month++;
      } else {
        date.month = 1;
        date.year++;
      }
    }
    delta--;
  }

  while (delta < 0) {
    if (date.day > 1) {
      date.day--;
    } else {
      if (date.month > 1) {
        date.month--;
      } else {
        date.month = 12;
        date.year--;
      }
      date.day = daysInMonth(date.year, date.month);
    }
    delta++;
  }
}

void addSecondsToReadingStatsDateTime(ReadingStatsDateTime& dt, uint32_t seconds) {
  if (!dt.isValid() || seconds == 0) {
    return;
  }

  uint32_t secondOfDay =
      static_cast<uint32_t>(dt.hour) * 3600u + static_cast<uint32_t>(dt.minute) * 60u + dt.second + seconds;
  const uint32_t daysToAdvance = secondOfDay / (24u * 3600u);
  secondOfDay %= (24u * 3600u);
  if (daysToAdvance > 0) {
    addDaysToReadingStatsDate(dt.date, static_cast<int>(daysToAdvance));
  }

  dt.hour = static_cast<uint8_t>(secondOfDay / 3600u);
  secondOfDay %= 3600u;
  dt.minute = static_cast<uint8_t>(secondOfDay / 60u);
  dt.second = static_cast<uint8_t>(secondOfDay % 60u);
}

uint32_t readingStatsDayIndex(const ReadingStatsDate& date) {
  uint32_t dayIndex = 0;
  for (uint16_t year = 2000; year < date.year; ++year) {
    dayIndex += isLeapYear(year) ? 366u : 365u;
  }
  for (uint8_t month = 1; month < date.month; ++month) {
    dayIndex += daysInMonth(date.year, month);
  }
  dayIndex += static_cast<uint32_t>(date.day - 1);
  return dayIndex;
}

bool readingStatsDateFromDayIndex(uint32_t dayIndex, ReadingStatsDate& outDate) {
  outDate = {};
  uint16_t year = 2000;
  while (year <= 2099) {
    const uint32_t yearDays = isLeapYear(year) ? 366u : 365u;
    if (dayIndex < yearDays) {
      break;
    }
    dayIndex -= yearDays;
    year++;
  }
  if (year > 2099) {
    return false;
  }

  uint8_t month = 1;
  while (month <= 12) {
    const uint8_t monthDays = daysInMonth(year, month);
    if (dayIndex < monthDays) {
      break;
    }
    dayIndex -= monthDays;
    month++;
  }
  if (month > 12) {
    return false;
  }

  outDate.year = year;
  outDate.month = month;
  outDate.day = static_cast<uint8_t>(dayIndex + 1u);
  return true;
}

uint8_t readingStatsDayOfWeekIndex(const ReadingStatsDate& date) {
  // 2000-01-01 was a Saturday. Convert so Monday = 0.
  const uint32_t dayIndex = readingStatsDayIndex(date);
  return static_cast<uint8_t>((5u + dayIndex) % 7u);
}

ReadingTimeBucket readingTimeBucketForHour(const uint8_t hour) {
  if (hour >= 5 && hour < 12) {
    return ReadingTimeBucket::Morning;
  }
  if (hour >= 12 && hour < 17) {
    return ReadingTimeBucket::Afternoon;
  }
  if (hour >= 17 && hour < 21) {
    return ReadingTimeBucket::Evening;
  }
  return ReadingTimeBucket::Night;
}

bool getCurrentLocalReadingStatsDateTime(ReadingStatsDateTime& outDateTime) {
  uint16_t year = 0;
  uint8_t month = 0;
  uint8_t day = 0;
  uint8_t hour = 0;
  uint8_t minute = 0;
  if (!halClock.getDateTime(year, month, day, hour, minute)) {
    outDateTime = {};
    return false;
  }

  outDateTime.date = {year, month, day};
  outDateTime.hour = hour;
  outDateTime.minute = minute;
  outDateTime.second = 0;
  if (!outDateTime.isValid()) {
    outDateTime = {};
    return false;
  }

  const int offsetQuarterHours = static_cast<int>(SETTINGS.clockUtcOffsetQ) - 48;
  const int offsetMinutes = offsetQuarterHours * 15;
  int totalMinutes = static_cast<int>(outDateTime.hour) * 60 + static_cast<int>(outDateTime.minute) + offsetMinutes;

  while (totalMinutes < 0) {
    addDaysToReadingStatsDate(outDateTime.date, -1);
    totalMinutes += 24 * 60;
  }
  while (totalMinutes >= 24 * 60) {
    addDaysToReadingStatsDate(outDateTime.date, 1);
    totalMinutes -= 24 * 60;
  }

  outDateTime.hour = static_cast<uint8_t>(totalMinutes / 60);
  outDateTime.minute = static_cast<uint8_t>(totalMinutes % 60);
  return outDateTime.isValid();
}

uint16_t readingSpanDaysInclusive(const ReadingStatsDate& start, const ReadingStatsDate& end) {
  if (!start.isValid() || !end.isValid() || compareReadingStatsDate(end, start) < 0) {
    return 0;
  }
  const uint32_t startDay = readingStatsDayIndex(start);
  const uint32_t endDay = readingStatsDayIndex(end);
  return static_cast<uint16_t>(endDay - startDay + 1u);
}

uint16_t readingSpanDaysElapsed(const ReadingStatsDate& start, const ReadingStatsDate& end) {
  if (!start.isValid() || !end.isValid() || compareReadingStatsDate(end, start) < 0) {
    return 0;
  }
  const uint32_t startDay = readingStatsDayIndex(start);
  const uint32_t endDay = readingStatsDayIndex(end);
  return static_cast<uint16_t>(endDay - startDay);
}

void formatReadingStatsShortDate(const ReadingStatsDate& date, char* buf, const size_t len) {
  if (!buf || len == 0) {
    return;
  }
  if (!date.isValid()) {
    snprintf(buf, len, "-");
    return;
  }
  snprintf(buf, len, "%s %u", MONTH_NAMES[date.month - 1], static_cast<unsigned>(date.day));
}

void formatReadingStatsMonthToken(const ReadingStatsDate& date, char* buf, const size_t len) {
  if (!buf || len == 0) {
    return;
  }
  if (!date.isValid()) {
    snprintf(buf, len, "-");
    return;
  }
  snprintf(buf, len, "%s", MONTH_NAMES[date.month - 1]);
}

void recordReadingSpanIntoBuckets(std::array<uint32_t, READING_TIME_BUCKET_COUNT>& timeOfDaySeconds,
                                  std::array<uint32_t, READING_DAY_OF_WEEK_COUNT>& dayOfWeekSeconds,
                                  const ReadingStatsDateTime& localStart, const uint32_t seconds) {
  if (!localStart.isValid() || seconds == 0) {
    return;
  }

  ReadingStatsDateTime cursor = localStart;
  uint32_t remaining = seconds;
  while (remaining > 0) {
    const uint8_t bucketIndex = static_cast<uint8_t>(readingTimeBucketForHour(cursor.hour));
    const uint8_t dayIndex = readingStatsDayOfWeekIndex(cursor.date);
    const uint32_t segment =
        remaining < secondsUntilNextBucketBoundary(cursor) ? remaining : secondsUntilNextBucketBoundary(cursor);
    timeOfDaySeconds[bucketIndex] += segment;
    dayOfWeekSeconds[dayIndex] += segment;
    remaining -= segment;
    addSecondsToReadingStatsDateTime(cursor, segment);
  }
}

void markReadingHistoryDay(uint32_t& anchorDay, std::array<uint8_t, READING_HISTORY_BYTES>& bits,
                           const uint32_t dayIndex) {
  if (anchorDay == 0 && !isBitSet(bits, 0)) {
    anchorDay = dayIndex;
    clearBits(bits);
    setBit(bits, 0);
    return;
  }

  if (dayIndex > anchorDay) {
    shiftHistoryOlder(bits, dayIndex - anchorDay);
    anchorDay = dayIndex;
  }

  const uint32_t delta = anchorDay - dayIndex;
  if (delta >= READING_HISTORY_DAYS) {
    return;
  }
  setBit(bits, static_cast<size_t>(delta));
}

void recordReadingSpanIntoHistory(uint32_t& anchorDay, std::array<uint8_t, READING_HISTORY_BYTES>& bits,
                                  const ReadingStatsDateTime& localStart, const uint32_t seconds) {
  if (!localStart.isValid() || seconds == 0) {
    return;
  }

  ReadingStatsDateTime cursor = localStart;
  uint32_t remaining = seconds;
  while (remaining > 0) {
    markReadingHistoryDay(anchorDay, bits, readingStatsDayIndex(cursor.date));
    const uint32_t secondsUntilMidnight =
        (24u * 3600u) - (static_cast<uint32_t>(cursor.hour) * 3600u + static_cast<uint32_t>(cursor.minute) * 60u +
                         static_cast<uint32_t>(cursor.second));
    const uint32_t segment = remaining < secondsUntilMidnight ? remaining : secondsUntilMidnight;
    remaining -= segment;
    addSecondsToReadingStatsDateTime(cursor, segment);
  }
}

void mergeReadingHistory(uint32_t& targetAnchorDay, std::array<uint8_t, READING_HISTORY_BYTES>& targetBits,
                         const uint32_t sourceAnchorDay, const std::array<uint8_t, READING_HISTORY_BYTES>& sourceBits) {
  if (sourceAnchorDay == 0 && !isBitSet(sourceBits, 0)) {
    return;
  }

  if (targetAnchorDay == 0 && !isBitSet(targetBits, 0)) {
    targetAnchorDay = sourceAnchorDay;
    targetBits = sourceBits;
    return;
  }

  if (sourceAnchorDay > targetAnchorDay) {
    shiftHistoryOlder(targetBits, sourceAnchorDay - targetAnchorDay);
    targetAnchorDay = sourceAnchorDay;
  }

  for (size_t bitIndex = 0; bitIndex < READING_HISTORY_DAYS; ++bitIndex) {
    if (!isBitSet(sourceBits, bitIndex)) {
      continue;
    }
    const uint32_t dayIndex = sourceAnchorDay - static_cast<uint32_t>(bitIndex);
    if (dayIndex > targetAnchorDay) {
      continue;
    }
    const uint32_t delta = targetAnchorDay - dayIndex;
    if (delta < READING_HISTORY_DAYS) {
      setBit(targetBits, static_cast<size_t>(delta));
    }
  }
}

uint16_t computeReadingHistoryLongestStreak(const uint32_t anchorDay,
                                            const std::array<uint8_t, READING_HISTORY_BYTES>& bits) {
  if (anchorDay == 0 && !isBitSet(bits, 0)) {
    return 0;
  }

  uint16_t best = 0;
  uint16_t current = 0;
  for (int bitIndex = static_cast<int>(READING_HISTORY_DAYS) - 1; bitIndex >= 0; --bitIndex) {
    if (isBitSet(bits, static_cast<size_t>(bitIndex))) {
      current++;
      if (current > best) {
        best = current;
      }
    } else {
      current = 0;
    }
  }
  return best;
}

uint16_t computeReadingHistoryCurrentStreak(uint32_t anchorDay, const std::array<uint8_t, READING_HISTORY_BYTES>& bits,
                                            const ReadingStatsDate* today) {
  if (anchorDay == 0 && !isBitSet(bits, 0)) {
    return 0;
  }
  if (!isBitSet(bits, 0)) {
    return 0;
  }

  if (today && today->isValid()) {
    const uint32_t todayDay = readingStatsDayIndex(*today);
    if (anchorDay + 1u < todayDay) {
      return 0;
    }
  }

  uint16_t streak = 0;
  for (size_t bitIndex = 0; bitIndex < READING_HISTORY_DAYS; ++bitIndex) {
    if (!isBitSet(bits, bitIndex)) {
      break;
    }
    streak++;
  }
  return streak;
}

uint16_t computeReadingHistoryDaysRead(const std::array<uint8_t, READING_HISTORY_BYTES>& bits) {
  // Popcount over history bytes — cheap enough for paint paths, no per-bit loop.
  uint16_t days = 0;
  for (const uint8_t byte : bits) {
#if defined(__GNUC__) || defined(__clang__)
    days = static_cast<uint16_t>(days + static_cast<uint16_t>(__builtin_popcount(static_cast<unsigned>(byte))));
#else
    uint8_t v = byte;
    while (v) {
      days = static_cast<uint16_t>(days + (v & 1u));
      v = static_cast<uint8_t>(v >> 1);
    }
#endif
  }
  return days;
}

uint32_t estimateSecondsPerPage(const uint16_t avgSecondsPerForwardPage, const uint16_t paceSampleCount,
                                const uint32_t totalReadingSeconds, const uint32_t totalPagesTurned) {
  uint32_t fromDwell = 0;
  uint32_t fromLife = 0;
  // Short-term dwell (need several samples).
  if (paceSampleCount >= 5 && avgSecondsPerForwardPage > 0) {
    fromDwell = avgSecondsPerForwardPage;
  }
  // Long-term: whole-book clock / pages turned (stable anchor for multi-hour ETAs).
  if (totalPagesTurned >= 8 && totalReadingSeconds >= 180) {
    fromLife = (totalReadingSeconds + totalPagesTurned / 2u) / totalPagesTurned;
  }
  if (fromLife > 0 && fromDwell > 0) {
    // Lifetime primary; dwell is a light nudge only (max() caused 13h vs 16h thrash).
    return (fromLife * 4u + fromDwell) / 5u;
  }
  if (fromLife > 0) return fromLife;
  if (fromDwell > 0) return fromDwell;
  if (paceSampleCount >= 3 && avgSecondsPerForwardPage > 0) {
    return avgSecondsPerForwardPage;
  }
  return 0;
}

float estimateRemainingBookPages(const int chapterPages, const int currentPage1Based, const float bookProgress01,
                                 const float chapterStartProgress01, const float chapterEndProgress01) {
  if (chapterPages <= 0 || currentPage1Based < 0) {
    return 0.0f;
  }
  const int remainingChapter = std::max(0, chapterPages - currentPage1Based);
  const float remainingBookFrac = std::max(0.0f, 1.0f - bookProgress01);
  if (remainingBookFrac <= 0.0f) {
    return 0.0f;
  }
  const float chapterSpan = chapterEndProgress01 - chapterStartProgress01;
  if (chapterPages > 0 && chapterSpan > 0.001f) {
    // Density of this chapter: pages per unit of book progress → scale to rest of book.
    const float pagesPerBookFrac = static_cast<float>(chapterPages) / chapterSpan;
    return remainingBookFrac * pagesPerBookFrac;
  }
  if (bookProgress01 > 0.005f && bookProgress01 < 0.995f && remainingChapter > 0) {
    return static_cast<float>(remainingChapter) * (remainingBookFrac / std::max(0.01f, bookProgress01));
  }
  return static_cast<float>(remainingChapter);
}

bool estimateTimeLeftFromPages(const float remainingPages, const uint32_t secondsPerPage, uint32_t& outSeconds) {
  outSeconds = 0;
  if (remainingPages <= 0.0f || secondsPerPage == 0) {
    return false;
  }
  const double product = static_cast<double>(remainingPages) * static_cast<double>(secondsPerPage);
  if (product <= 0.0 || product > static_cast<double>(UINT32_MAX)) {
    outSeconds = product > 0.0 ? UINT32_MAX : 0;
    return outSeconds > 0;
  }
  outSeconds = static_cast<uint32_t>(product + 0.5);
  return outSeconds > 0;
}

bool estimateTimeLeftFromProgress(const uint32_t totalReadingSeconds, const float progressPercent,
                                  uint32_t& outSeconds) {
  outSeconds = 0;
  // Only use when well into a book — early % is a poor proxy for remaining effort.
  if (totalReadingSeconds < 300 || progressPercent < 15.0f || progressPercent >= 100.0f) {
    return false;
  }
  const float progress = progressPercent / 100.0f;
  const float estimate = (static_cast<float>(totalReadingSeconds) * (1.0f - progress)) / progress;
  if (estimate <= 0.0f) {
    return false;
  }
  outSeconds = static_cast<uint32_t>(estimate + 0.5f);
  return outSeconds > 0;
}

bool estimateBookTimeLeftSeconds(const float remainingPages, const uint32_t secondsPerPage,
                                 const uint32_t totalReadingSeconds, const float progressPercent,
                                 uint32_t& outSeconds) {
  outSeconds = 0;
  uint32_t fromPages = 0;
  uint32_t fromProgress = 0;
  const bool hasPages = estimateTimeLeftFromPages(remainingPages, secondsPerPage, fromPages);
  const bool hasProgress = estimateTimeLeftFromProgress(totalReadingSeconds, progressPercent, fromProgress);

  if (!hasPages && !hasProgress) {
    return false;
  }
  if (!hasProgress) {
    outSeconds = fromPages;
    return true;
  }
  if (!hasPages) {
    outSeconds = fromProgress;
    return true;
  }

  // Both available. Page density often undercounts remaining work (e.g. halfway
  // through a long book with 12h spent but only ~1h page×pace left because the
  // current chapter's page/progress ratio is sparse). When the two disagree by
  // more than ~40%, trust progress-ratio (elapsed × remaining fraction).
  const uint32_t hi = std::max(fromPages, fromProgress);
  const uint32_t lo = std::min(fromPages, fromProgress);
  if (lo * 5u < hi * 3u) {
    // Strong disagreement — progress tracks multi-hour reality better.
    outSeconds = fromProgress;
  } else {
    // Close enough: light blend, lean progress (2:1).
    outSeconds = (fromPages + fromProgress * 2u) / 3u;
  }
  return outSeconds > 0;
}

uint32_t smoothTimeLeftSeconds(const uint32_t prevSeconds, const uint32_t rawSeconds) {
  if (rawSeconds == 0) {
    return prevSeconds;
  }
  if (prevSeconds == 0) {
    return rawSeconds;
  }
  // Large model correction (e.g. page×pace said 51m, progress-ratio says 6h):
  // do not crawl at 3 min/step for hours — move most of the way to raw.
  if (rawSeconds > prevSeconds * 2u + 600u || prevSeconds > rawSeconds * 2u + 600u) {
    return (prevSeconds + rawSeconds * 3u) / 4u;
  }
  // Cap one-step move: max(3 minutes, ~6% of previous). Stops 13h ↔ 16h thrash
  // when chapter density or a single dwell sample jumps.
  const uint32_t maxStep = std::max<uint32_t>(180u, prevSeconds / 16u);
  uint32_t target = rawSeconds;
  if (rawSeconds > prevSeconds + maxStep) {
    target = prevSeconds + maxStep;
  } else if (rawSeconds + maxStep < prevSeconds) {
    target = prevSeconds - maxStep;
  }
  // Light EMA after the clamp (3/4 previous, 1/4 target).
  return (prevSeconds * 3u + target) / 4u;
}
