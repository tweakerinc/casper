#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

constexpr size_t READING_TIME_BUCKET_COUNT = 4;
constexpr size_t READING_DAY_OF_WEEK_COUNT = 7;
constexpr size_t READING_HISTORY_DAYS = 730;
constexpr size_t READING_HISTORY_BYTES = (READING_HISTORY_DAYS + 7) / 8;

enum class ReadingTimeBucket : uint8_t { Morning = 0, Afternoon, Evening, Night };

struct ReadingStatsDate {
  uint16_t year = 0;
  uint8_t month = 0;
  uint8_t day = 0;

  bool isValid() const;
  void clear();
};

struct ReadingStatsDateTime {
  ReadingStatsDate date;
  uint8_t hour = 0;
  uint8_t minute = 0;
  uint8_t second = 0;

  bool isValid() const;
};

bool isLeapYear(uint16_t year);
uint8_t daysInMonth(uint16_t year, uint8_t month);
bool isValidReadingStatsDate(const ReadingStatsDate& date);
int compareReadingStatsDate(const ReadingStatsDate& lhs, const ReadingStatsDate& rhs);
void addDaysToReadingStatsDate(ReadingStatsDate& date, int delta);
void addSecondsToReadingStatsDateTime(ReadingStatsDateTime& dt, uint32_t seconds);
uint32_t readingStatsDayIndex(const ReadingStatsDate& date);
bool readingStatsDateFromDayIndex(uint32_t dayIndex, ReadingStatsDate& outDate);
uint8_t readingStatsDayOfWeekIndex(const ReadingStatsDate& date);  // Monday = 0
ReadingTimeBucket readingTimeBucketForHour(uint8_t hour);
bool getCurrentLocalReadingStatsDateTime(ReadingStatsDateTime& outDateTime);
uint16_t readingSpanDaysInclusive(const ReadingStatsDate& start, const ReadingStatsDate& end);
uint16_t readingSpanDaysElapsed(const ReadingStatsDate& start, const ReadingStatsDate& end);
void formatReadingStatsShortDate(const ReadingStatsDate& date, char* buf, size_t len);
void formatReadingStatsMonthToken(const ReadingStatsDate& date, char* buf, size_t len);

void recordReadingSpanIntoBuckets(std::array<uint32_t, READING_TIME_BUCKET_COUNT>& timeOfDaySeconds,
                                  std::array<uint32_t, READING_DAY_OF_WEEK_COUNT>& dayOfWeekSeconds,
                                  const ReadingStatsDateTime& localStart, uint32_t seconds);
void markReadingHistoryDay(uint32_t& anchorDay, std::array<uint8_t, READING_HISTORY_BYTES>& bits, uint32_t dayIndex);
void recordReadingSpanIntoHistory(uint32_t& anchorDay, std::array<uint8_t, READING_HISTORY_BYTES>& bits,
                                  const ReadingStatsDateTime& localStart, uint32_t seconds);
void mergeReadingHistory(uint32_t& targetAnchorDay, std::array<uint8_t, READING_HISTORY_BYTES>& targetBits,
                         uint32_t sourceAnchorDay, const std::array<uint8_t, READING_HISTORY_BYTES>& sourceBits);
uint16_t computeReadingHistoryLongestStreak(uint32_t anchorDay, const std::array<uint8_t, READING_HISTORY_BYTES>& bits);
uint16_t computeReadingHistoryCurrentStreak(uint32_t anchorDay, const std::array<uint8_t, READING_HISTORY_BYTES>& bits,
                                            const ReadingStatsDate* today);
// Count of distinct calendar days with any reading activity in the history window.
uint16_t computeReadingHistoryDaysRead(const std::array<uint8_t, READING_HISTORY_BYTES>& bits);

// ---- Time-left (reader footer + dashboard share one model) ------------------
// Primary model: remaining_pages × seconds_per_page.
// Progress-only (time × remaining%/done%) underestimates long books early because
// EPUB % often advances faster than real reading effort (front matter, short spines).

// Seconds per page from dwell average and/or total time ÷ pages turned.
// Prefers the long-term rate (stable); lightly blends dwell so a few slow/fast
// pages do not swing multi-hour ETAs. Returns 0 if not enough data.
// totalReadingSeconds may include the open session (live total).
uint32_t estimateSecondsPerPage(uint16_t avgSecondsPerForwardPage, uint16_t paceSampleCount,
                                uint32_t totalReadingSeconds, uint32_t totalPagesTurned);

// Extrapolate remaining pages in the whole book from current chapter density.
// Progress values are 0..1. currentPage1Based is 1..chapterPages.
float estimateRemainingBookPages(int chapterPages, int currentPage1Based, float bookProgress01,
                                 float chapterStartProgress01, float chapterEndProgress01);

// remainingPages × secondsPerPage. Returns false if inputs are unusable.
bool estimateTimeLeftFromPages(float remainingPages, uint32_t secondsPerPage, uint32_t& outSeconds);

// Progress-ratio ETA: totalReading × (1−p)/p. Solid once you've read a while with
// a trustworthy progress % — matches "I'm halfway with 12h spent → ~12h left".
bool estimateTimeLeftFromProgress(uint32_t totalReadingSeconds, float progressPercent, uint32_t& outSeconds);

// Book-level ETA for home / status bar / session save.
// Page×pace undercounts badly when chapter page density ≠ book progress (partial
// sections, uneven spine). Progress-ratio is preferred when both disagree.
bool estimateBookTimeLeftSeconds(float remainingPages, uint32_t secondsPerPage, uint32_t totalReadingSeconds,
                                 float progressPercent, uint32_t& outSeconds);

// Dampen page-to-page ETA jumps. prevSeconds=0 means "no history" (return raw).
// Caps relative step then applies a light EMA so 13h does not leap to 16h on one turn.
uint32_t smoothTimeLeftSeconds(uint32_t prevSeconds, uint32_t rawSeconds);
