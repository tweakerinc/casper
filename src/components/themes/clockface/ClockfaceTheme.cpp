#include "ClockfaceTheme.h"

#include <EpdFontFamily.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "CrossPointSettings.h"
#include "RecentBooksStore.h"
#include "activities/reader/BookReadingStats.h"
#include "activities/reader/GlobalReadingStats.h"
#include "components/themes/BaseTheme.h"
#include "fontIds.h"
#include "util/StringUtils.h"

namespace {
// Real 72 pt 2-bit Source Serif Bold (digits + colon only) — smooth AA, no pixel scale.
constexpr int kClockFontId = SOURCESERIF4_72_CLOCK_FONT_ID;
constexpr int kDayFontId = SOURCESERIF4_12_FONT_ID;
constexpr int kLabelFontId = SOURCESERIF4_12_FONT_ID;
constexpr int kTitleFontId = SOURCESERIF4_18_FONT_ID;
constexpr int kAuthorFontId = SOURCESERIF4_14_FONT_ID;
// Stats values slightly quieter than title — still readable as a “reading desk” panel.
constexpr int kStatValueFontId = SOURCESERIF4_14_FONT_ID;
constexpr int kStatLabelFontId = SOURCESERIF4_12_FONT_ID;
// Book title on Stats/Lifetime pages — smaller than Now Reading title (18 pt).
constexpr int kStatsBookTitleFontId = SOURCESERIF4_14_FONT_ID;

// Title/author wrap inset (can stay a bit snug). Stats use a wider band.
constexpr int kSideInset = 24;
constexpr int kStatsSideInset = 10;
// Title→author optical gap.
constexpr int kPairGap = 10;
// Clock ink bottom → weekday — more air than title/author (was crowded).
constexpr int kClockToDayGap = 18;
constexpr int kLabelToTitleGap = 14;
constexpr int kTitleMaxLines = 3;
constexpr int kAuthorMaxLines = 2;
constexpr int kRuleThickness = 2;
constexpr int kRuleHalfWidth = 90;
// Value→label pull so pairs read as one unit (same idea as Focus stats).
constexpr int kValueLabelPull = 4;
constexpr int kStatRowGap = 12;
// Title ↔ grid and grid ↔ "STATS" / "LIFETIME" caption share the same air.
constexpr int kStatsStackGap = 16;

const StrId kWeekdayIds[7] = {
    StrId::STR_WEEKDAY_SUNDAY,    StrId::STR_WEEKDAY_MONDAY,   StrId::STR_WEEKDAY_TUESDAY,
    StrId::STR_WEEKDAY_WEDNESDAY, StrId::STR_WEEKDAY_THURSDAY, StrId::STR_WEEKDAY_FRIDAY,
    StrId::STR_WEEKDAY_SATURDAY,
};

bool isLeapYear(const uint16_t year) { return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0; }

uint8_t daysInMonth(const uint16_t year, const uint8_t month) {
  static const uint8_t days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month < 1 || month > 12) return 0;
  if (month == 2 && isLeapYear(year)) return 29;
  return days[month - 1];
}

void adjustDateByDays(uint16_t& year, uint8_t& month, uint8_t& day, const int dayDelta) {
  if (dayDelta > 0) {
    const uint8_t monthDays = daysInMonth(year, month);
    if (day < monthDays) {
      day++;
      return;
    }
    day = 1;
    if (month < 12) {
      month++;
    } else {
      month = 1;
      year++;
    }
  } else if (dayDelta < 0) {
    if (day > 1) {
      day--;
      return;
    }
    if (month > 1) {
      month--;
    } else {
      month = 12;
      year--;
    }
    day = daysInMonth(year, month);
  }
}

uint32_t dayIndexSince2000(uint16_t year, uint8_t month, uint8_t day) {
  uint32_t dayIndex = 0;
  for (uint16_t y = 2000; y < year; ++y) {
    dayIndex += isLeapYear(y) ? 366u : 365u;
  }
  for (uint8_t m = 1; m < month; ++m) {
    dayIndex += daysInMonth(year, m);
  }
  return dayIndex + static_cast<uint32_t>(day - 1);
}

int localWeekdayIndex() {
  uint16_t year = 0;
  uint8_t month = 0, day = 0, hour = 0, minute = 0;
  if (!halClock.isAvailable() || !halClock.getDateTime(year, month, day, hour, minute)) {
    return -1;
  }
  if (year < 2000 || month < 1 || month > 12 || day < 1) return -1;

  const uint8_t offsetQ = std::min<uint8_t>(SETTINGS.clockUtcOffsetQ, 104);
  const int offsetMinutes = (static_cast<int>(offsetQ) - 48) * 15;
  int localMinutes = static_cast<int>(hour) * 60 + static_cast<int>(minute) + offsetMinutes;
  while (localMinutes < 0) {
    adjustDateByDays(year, month, day, -1);
    localMinutes += 24 * 60;
  }
  while (localMinutes >= 24 * 60) {
    adjustDateByDays(year, month, day, 1);
    localMinutes -= 24 * 60;
  }
  return static_cast<int>((6u + dayIndexSince2000(year, month, day)) % 7u);
}

void toUpperAsciiInPlace(char* s) {
  if (!s) return;
  for (; *s; ++s) {
    if (*s >= 'a' && *s <= 'z') *s = static_cast<char>(*s - 'a' + 'A');
  }
}

int drawCenteredWrapped(const GfxRenderer& renderer, const int fontId, const int centerX, int y, const int maxWidth,
                        const char* text, const int maxLines, const EpdFontFamily::Style style) {
  if (!text || !*text) return y;
  auto lines = renderer.wrappedText(fontId, text, maxWidth, maxLines, style);
  const int lineH = renderer.getLineHeight(fontId);
  for (const auto& line : lines) {
    const int lw = renderer.getTextWidth(fontId, line.c_str(), style);
    renderer.drawText(fontId, centerX - lw / 2, y, line.c_str(), true, style);
    y += lineH;
  }
  return y;
}

int measureWrappedHeight(const GfxRenderer& renderer, const int fontId, const int maxWidth, const char* text,
                         const int maxLines, const EpdFontFamily::Style style) {
  if (!text || !*text) return 0;
  const auto lines = renderer.wrappedText(fontId, text, maxWidth, maxLines, style);
  return static_cast<int>(lines.size()) * renderer.getLineHeight(fontId);
}

// Always re-read the RTC (HalClock::formatTime can serve a 10s cache).
bool formatHeroTime(char* buf, size_t bufSize) {
  if (!buf || bufSize < 6) return false;
  const bool use12 = SETTINGS.clockFormat == 1;
  uint16_t year = 0;
  uint8_t month = 0, day = 0, hour = 0, minute = 0;
  if (!halClock.isAvailable() || !halClock.getDateTime(year, month, day, hour, minute)) {
    snprintf(buf, bufSize, use12 ? "9:41" : "09:41");
    return false;
  }
  uint8_t utcOffsetQ = SETTINGS.clockUtcOffsetQ;
  if (utcOffsetQ > 104) utcOffsetQ = 104;
  const int offsetQuarterHours = static_cast<int>(utcOffsetQ) - 48;
  int totalMinutes = static_cast<int>(hour) * 60 + static_cast<int>(minute) + offsetQuarterHours * 15;
  totalMinutes = ((totalMinutes % 1440) + 1440) % 1440;
  const int hour24 = totalMinutes / 60;
  const int min = totalMinutes % 60;
  if (use12) {
    int hour12 = hour24 % 12;
    if (hour12 == 0) hour12 = 12;
    snprintf(buf, bufSize, "%d:%02d", hour12, min);
  } else {
    snprintf(buf, bufSize, "%02d:%02d", hour24, min);
  }
  return true;
}

void drawHeroClockCentered(const GfxRenderer& renderer, const int centerX, const int yTop, const char* timeText) {
  // Font is Bold TTF stored in the REGULAR style slot (digits-only family).
  const int lw = renderer.getTextWidth(kClockFontId, timeText, EpdFontFamily::REGULAR);
  renderer.drawText(kClockFontId, centerX - lw / 2, yTop, timeText, true, EpdFontFamily::REGULAR);
}

float pagesPerMinute(const uint32_t totalPagesTurned, const uint32_t totalReadingSeconds) {
  if (totalReadingSeconds <= 60) return 0.0f;
  return static_cast<float>(totalPagesTurned) * 60.0f / static_cast<float>(totalReadingSeconds);
}

void formatCompactDuration(const uint32_t seconds, char* buf, const size_t len) {
  if (seconds < 60) {
    snprintf(buf, len, "%s", tr(STR_STATS_LESS_THAN_MIN));
    return;
  }
  const uint32_t minutes = (seconds + 30u) / 60u;
  if (minutes < 60) {
    snprintf(buf, len, "%lu min", static_cast<unsigned long>(minutes));
    return;
  }
  const uint32_t hours = minutes / 60u;
  const uint32_t remainder = minutes % 60u;
  if (remainder == 0) {
    snprintf(buf, len, "%luh", static_cast<unsigned long>(hours));
  } else {
    snprintf(buf, len, "%luh %lum", static_cast<unsigned long>(hours), static_cast<unsigned long>(remainder));
  }
}

struct ContentBand {
  int contentTop = 0;
  int contentBottom = 0;
  int midY = 0;
  int halfH = 0;
  int centerX = 0;
  int textMaxW = 0;
};

ContentBand layoutContentBand(const GfxRenderer& renderer) {
  ContentBand b;
  const int pageW = renderer.getScreenWidth();
  const int pageH = renderer.getScreenHeight();
  const auto& metrics = ClockfaceMetrics::values;
  const int footerH = metrics.buttonHintsHeight;
  b.centerX = pageW / 2;
  const bool hasChrome = SETTINGS.systemStatusBarHas(CrossPointSettings::SYS_SLOT_BATTERY) ||
                         SETTINGS.systemStatusBarHas(CrossPointSettings::SYS_SLOT_CLOCK);
  b.contentTop =
      hasChrome ? (BaseTheme::kTopChromeBatteryY +
                   std::max(metrics.batteryHeight + 8, metrics.statusBarVerticalMargin) + 8)
                : 20;
  b.contentBottom = pageH - footerH;
  const int bandH = std::max(1, b.contentBottom - b.contentTop);
  b.midY = b.contentTop + bandH / 2;
  b.halfH = bandH / 2;
  b.textMaxW = std::max(40, pageW - kSideInset * 2);
  return b;
}

int statPairHeight(const GfxRenderer& renderer) {
  const int valueH = renderer.getLineHeight(kStatValueFontId);
  const int labelH = renderer.getLineHeight(kStatLabelFontId);
  return std::max(1, valueH - kValueLabelPull) + labelH;
}

void drawStatCell(const GfxRenderer& renderer, const int x, const int w, const int y, const int h, const char* value,
                  const char* label) {
  // Pad only for truncate budget; glyphs are centered in the full cell width.
  constexpr int kPadX = 2;
  const int textW = std::max(1, w - kPadX * 2);
  const int valueLineH = renderer.getLineHeight(kStatValueFontId);
  const int labelLineH = renderer.getLineHeight(kStatLabelFontId);
  const int pairH = std::max(1, valueLineH - kValueLabelPull) + labelLineH;
  const int textY = y + std::max(0, (h - pairH) / 2);
  const std::string visValue = renderer.truncatedText(kStatValueFontId, value ? value : "-", textW, EpdFontFamily::BOLD);
  const std::string visLabel = renderer.truncatedText(kStatLabelFontId, label ? label : "", textW);
  const int valueW = renderer.getTextWidth(kStatValueFontId, visValue.c_str(), EpdFontFamily::BOLD);
  const int labelW = renderer.getTextWidth(kStatLabelFontId, visLabel.c_str());
  renderer.drawText(kStatValueFontId, x + (w - valueW) / 2, textY, visValue.c_str(), true, EpdFontFamily::BOLD);
  renderer.drawText(kStatLabelFontId, x + (w - labelW) / 2, textY + std::max(1, valueLineH - kValueLabelPull),
                    visLabel.c_str(), true);
}

// Section label (NOW READING / STATS / LIFETIME) — same voice as the clock weekday.
// style: REGULAR for "Now Reading"; BOLD for stats panel footers (try-on).
void drawSectionLabel(const GfxRenderer& renderer, const int centerX, const int y, const char* label,
                      const EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
  if (!label || !*label) return;
  char buf[48];
  snprintf(buf, sizeof(buf), "%s", label);
  toUpperAsciiInPlace(buf);
  const int lw = renderer.getTextWidth(kLabelFontId, buf, style);
  renderer.drawText(kLabelFontId, centerX - lw / 2, y, buf, true, style);
}

// Ink-only 2×N or 3×N grid — airy, page-centered, no plate (native e-ink white).
// Columns are equal width; leftover pixels from gridW % cols are split left/right
// so the block stays optically centered (not right-heavy).
void drawStatGrid(const GfxRenderer& renderer, const int centerX, const int top, const int gridW, const int gridH,
                  const int cols, const int rows, const char* const* values, const char* const* labels) {
  if (cols <= 0 || rows <= 0 || gridW < 40 || gridH < 24) return;
  const int colW = gridW / cols;
  const int usedW = colW * cols;
  const int left = centerX - usedW / 2;
  const int rowH = gridH / rows;
  for (int r = 0; r < rows; ++r) {
    for (int c = 0; c < cols; ++c) {
      const int i = r * cols + c;
      drawStatCell(renderer, left + c * colW, colW, top + r * rowH, rowH, values[i], labels[i]);
    }
  }
}

size_t focusedBookIndex(const std::vector<RecentBook>& books, const int selectorIndex) {
  if (books.empty()) return 0;
  return static_cast<size_t>(
      std::clamp(selectorIndex, 0, static_cast<int>(books.size()) - 1));
}

void drawTitleAuthorPanel(const GfxRenderer& renderer, const ContentBand& band, const std::vector<RecentBook>& books,
                          const int selectorIndex) {
  const int centerX = band.centerX;
  const int midY = band.midY;
  const int halfH = band.halfH;
  const int textMaxW = band.textMaxW;
  const char* label = tr(STR_NOW_READING);
  const int labelH = renderer.getLineHeight(kLabelFontId);
  const int titleLineH = renderer.getLineHeight(kTitleFontId);
  const int titleInkH = renderer.getFontAscenderSize(kTitleFontId);

  if (books.empty()) {
    const char* msg = tr(STR_NO_OPEN_BOOK);
    const int blockH = labelH + kLabelToTitleGap + titleLineH;
    int y = midY + std::max(0, (halfH - blockH) / 2);
    drawSectionLabel(renderer, centerX, y, label);
    y += labelH + kLabelToTitleGap;
    const int mw = renderer.getTextWidth(kTitleFontId, msg, EpdFontFamily::BOLD);
    renderer.drawText(kTitleFontId, centerX - mw / 2, y, msg, true, EpdFontFamily::BOLD);
    return;
  }

  const RecentBook& book = books[focusedBookIndex(books, selectorIndex)];
  const char* title = book.title.empty() ? book.path.c_str() : book.title.c_str();
  const std::string authorDisplay =
      book.author.empty() ? std::string() : StringUtils::formatAuthorDisplayName(book.author);
  const char* author = authorDisplay.empty() ? nullptr : authorDisplay.c_str();

  const int titleH =
      measureWrappedHeight(renderer, kTitleFontId, textMaxW, title, kTitleMaxLines, EpdFontFamily::BOLD);
  const int authorH =
      author ? measureWrappedHeight(renderer, kAuthorFontId, textMaxW, author, kAuthorMaxLines, EpdFontFamily::REGULAR)
             : 0;
  // Match clock → weekday optical gap (kClockToDayGap), not the tighter title↔author pair.
  const int titleToAuthor = author ? (titleInkH + kClockToDayGap + authorH - titleLineH) : 0;
  const int textBlockH = labelH + kLabelToTitleGap + titleH + titleToAuthor;

  int y = midY + std::max(8, (halfH - textBlockH) / 2);
  drawSectionLabel(renderer, centerX, y, label);
  y += labelH + kLabelToTitleGap;
  y = drawCenteredWrapped(renderer, kTitleFontId, centerX, y, textMaxW, title, kTitleMaxLines, EpdFontFamily::BOLD);
  if (author) {
    y = y - titleLineH + titleInkH + kClockToDayGap;
    drawCenteredWrapped(renderer, kAuthorFontId, centerX, y, textMaxW, author, kAuthorMaxLines,
                        EpdFontFamily::REGULAR);
  }
}

const char* dayCountText(const uint16_t days) { return days == 1 ? tr(STR_STATS_DAY) : tr(STR_STATS_DAYS); }

// Single line so the 3×2 stats grid never shifts when titles are long.
constexpr int kStatsTitleMaxLines = 1;

const char* focusedBookTitle(const std::vector<RecentBook>& books, const int selectorIndex) {
  if (books.empty()) return tr(STR_NO_OPEN_BOOK);
  const RecentBook& book = books[focusedBookIndex(books, selectorIndex)];
  return book.title.empty() ? book.path.c_str() : book.title.c_str();
}

// Layout: [book title] —gap— [3×2 grid] —same gap— [STATS / LIFETIME caption].
// Whole stack is vertically centered between the mid rule and the menu buttons.
void drawStatsStylePanel(const GfxRenderer& renderer, const ContentBand& band, const char* bookTitle,
                         const char* footerCaption, const char* const* values, const char* const* labels) {
  const int titleH = measureWrappedHeight(renderer, kStatsBookTitleFontId, band.textMaxW, bookTitle,
                                          kStatsTitleMaxLines, EpdFontFamily::BOLD);
  const int footerH = renderer.getLineHeight(kLabelFontId);
  const int pairH = statPairHeight(renderer);
  const int gridH = pairH * 2 + kStatRowGap;
  const int blockH = titleH + kStatsStackGap + gridH + kStatsStackGap + footerH;

  // Zone: just below mid rule → top of button-hint strip (contentBottom).
  const int zoneTop = band.midY + kRuleThickness;
  const int zoneBottom = band.contentBottom;
  const int zoneH = std::max(1, zoneBottom - zoneTop);
  int y = zoneTop + std::max(0, (zoneH - blockH) / 2);

  y = drawCenteredWrapped(renderer, kStatsBookTitleFontId, band.centerX, y, band.textMaxW, bookTitle,
                          kStatsTitleMaxLines, EpdFontFamily::BOLD);
  y += kStatsStackGap;

  const int pageW = renderer.getScreenWidth();
  const int gridW = std::max(40, pageW - kStatsSideInset * 2);
  drawStatGrid(renderer, band.centerX, y, gridW, gridH, /*cols=*/3, /*rows=*/2, values, labels);
  y += gridH + kStatsStackGap;

  // Bold footer so STATS / LIFETIME read as panel captions (try-on vs Now Reading).
  drawSectionLabel(renderer, band.centerX, y, footerCaption, EpdFontFamily::BOLD);
}

// Book stats — 3×2:
//   Progress | Reading Time | Time Left
//   Sessions | Avg. Session | Pages/Min
void drawBookStatsPanel(const GfxRenderer& renderer, const ContentBand& band, const char* bookTitle,
                        const BookReadingStats* stats, const float progressPercent) {
  const BookReadingStats empty{};
  const BookReadingStats& s = stats != nullptr ? *stats : empty;

  char vProgress[24];
  char vTime[32];
  char vLeft[32];
  char vSessions[24];
  char vAvg[32];
  char vPace[24];
  if (progressPercent >= 0.0f) {
    snprintf(vProgress, sizeof(vProgress), "%d%%", static_cast<int>(progressPercent + 0.5f));
  } else {
    snprintf(vProgress, sizeof(vProgress), "-");
  }
  BookReadingStats::formatDuration(s.totalReadingSeconds, vTime, sizeof(vTime));
  if (!s.isCompleted && s.estimatedTimeLeftSeconds > 0) {
    formatCompactDuration(s.estimatedTimeLeftSeconds, vLeft, sizeof(vLeft));
  } else {
    snprintf(vLeft, sizeof(vLeft), "-");
  }
  snprintf(vSessions, sizeof(vSessions), "%u", static_cast<unsigned>(s.sessionCount));
  const uint32_t avgSecs = s.sessionCount > 0 ? s.totalReadingSeconds / s.sessionCount : 0;
  BookReadingStats::formatDuration(avgSecs, vAvg, sizeof(vAvg));
  snprintf(vPace, sizeof(vPace), "%.1f", pagesPerMinute(s.totalPagesTurned, s.totalReadingSeconds));

  const char* values[6] = {vProgress, vTime, vLeft, vSessions, vAvg, vPace};
  const char* labels[6] = {tr(STR_STATS_PROGRESS_LBL), tr(STR_STATS_TIME_LBL), tr(STR_TIME_LEFT),
                           tr(STR_STATS_SESSIONS_LBL), tr(STR_STATS_AVG_SESSION_LBL), tr(STR_STATS_PAGES_PER_MIN)};
  drawStatsStylePanel(renderer, band, bookTitle, tr(STR_STATS), values, labels);
}

// Lifetime — 3×2:
//   Sessions | Reading Time | Pages/Min
//   Avg. Session | Books Read | Streak
void drawLifetimePanel(const GfxRenderer& renderer, const ContentBand& band, const char* bookTitle,
                     const GlobalReadingStats* globalStats) {
  const GlobalReadingStats empty{};
  const GlobalReadingStats& g = globalStats != nullptr ? *globalStats : empty;

  char vSessions[24];
  char vTime[32];
  char vPace[24];
  char vAvg[32];
  char vBooks[24];
  char vStreak[32];
  snprintf(vSessions, sizeof(vSessions), "%lu", static_cast<unsigned long>(g.totalSessions));
  BookReadingStats::formatDuration(g.totalReadingSeconds, vTime, sizeof(vTime));
  snprintf(vPace, sizeof(vPace), "%.1f", pagesPerMinute(g.totalPagesTurned, g.totalReadingSeconds));
  const uint32_t avgSecs = g.totalSessions > 0 ? g.totalReadingSeconds / g.totalSessions : 0;
  BookReadingStats::formatDuration(avgSecs, vAvg, sizeof(vAvg));
  if (g.completedBooks > 0) {
    snprintf(vBooks, sizeof(vBooks), "%lu", static_cast<unsigned long>(g.completedBooks));
  } else {
    snprintf(vBooks, sizeof(vBooks), "-");
  }
  const uint16_t longest = g.displayLongestReadingStreak();
  if (longest > 0) {
    snprintf(vStreak, sizeof(vStreak), "%u %s", static_cast<unsigned>(longest), dayCountText(longest));
  } else {
    snprintf(vStreak, sizeof(vStreak), "-");
  }

  const char* values[6] = {vSessions, vTime, vPace, vAvg, vBooks, vStreak};
  const char* labels[6] = {tr(STR_STATS_SESSIONS_LBL), tr(STR_STATS_TIME_LBL), tr(STR_STATS_PAGES_PER_MIN),
                           tr(STR_STATS_AVG_SESSION_LBL), tr(STR_STATS_COMPLETED_LBL),
                           tr(STR_STATS_LONGEST_STREAK_LBL)};
  drawStatsStylePanel(renderer, band, bookTitle, tr(STR_STATS_ALL_TIME), values, labels);
}

void drawUnderPanel(const GfxRenderer& renderer, const ContentBand& band, const std::vector<RecentBook>& books,
                    const int selectorIndex, const BookReadingStats* stats, const float progressPercent,
                    const GlobalReadingStats* globalStats) {
  using Mode = ClockfaceThemeUi::UnderMode;
  // Tracking off → title/author only (never show empty stats shells).
  ClockfaceThemeUi::clampUnderModeToTracking();
  const char* title = focusedBookTitle(books, selectorIndex);
  switch (ClockfaceThemeUi::underMode()) {
    case Mode::BookStats:
      drawBookStatsPanel(renderer, band, title, stats, progressPercent);
      break;
    case Mode::Lifetime:
      drawLifetimePanel(renderer, band, title, globalStats);
      break;
    case Mode::TitleAuthor:
    default:
      drawTitleAuthorPanel(renderer, band, books, selectorIndex);
      break;
  }
}

void drawClockHalf(const GfxRenderer& renderer, const ContentBand& band) {
  char timeBuf[16];
  formatHeroTime(timeBuf, sizeof(timeBuf));
  const int clockInkH = renderer.getFontAscenderSize(kClockFontId);
  const int dayH = renderer.getLineHeight(kDayFontId);

  char dayBuf[32] = "";
  const int wd = localWeekdayIndex();
  if (wd >= 0 && wd < 7) {
    snprintf(dayBuf, sizeof(dayBuf), "%s", I18N.get(kWeekdayIds[wd]));
    toUpperAsciiInPlace(dayBuf);
  }

  const int clockBlockH = clockInkH + (dayBuf[0] ? (kClockToDayGap + dayH) : 0);
  // Prefer vertical center in the upper half; pin to top if 72 pt overflows the band.
  const int groupTop = band.contentTop + std::max(0, (band.halfH - clockBlockH) / 2);
  drawHeroClockCentered(renderer, band.centerX, groupTop, timeBuf);
  if (dayBuf[0]) {
    const int dayW = renderer.getTextWidth(kDayFontId, dayBuf, EpdFontFamily::REGULAR);
    renderer.drawText(kDayFontId, band.centerX - dayW / 2, groupTop + clockInkH + kClockToDayGap, dayBuf, true,
                      EpdFontFamily::REGULAR);
  }

  renderer.drawLine(band.centerX - kRuleHalfWidth, band.midY, band.centerX + kRuleHalfWidth, band.midY, kRuleThickness,
                    true);
}
}  // namespace

void ClockfaceTheme::drawRecentBookCover(GfxRenderer& renderer, Rect /*rect*/,
                                         const std::vector<RecentBook>& recentBooks, const int selectorIndex,
                                         bool& coverRendered, bool& coverBufferStored, bool& bufferRestored,
                                         StoreCoverBufferFn /*storeCoverBuffer*/, const BookReadingStats* stats,
                                         float progressPercent, const GlobalReadingStats* globalStats,
                                         const char* /*currentChapterTitle*/) const {
  (void)bufferRestored;

  coverBufferStored = false;
  coverRendered = true;

  const ContentBand band = layoutContentBand(renderer);
  drawClockHalf(renderer, band);
  drawUnderPanel(renderer, band, recentBooks, selectorIndex, stats, progressPercent, globalStats);
}

Rect ClockfaceThemeUi::redrawUnderPanel(GfxRenderer& renderer, const std::vector<RecentBook>& recentBooks,
                                        const int selectorIndex, const BookReadingStats* stats,
                                        const float progressPercent, const GlobalReadingStats* globalStats) {
  const ContentBand band = layoutContentBand(renderer);
  // Full-width lower half (rule stays). Prefer HomeActivity full-frame window path
  // so white paper matches the upper half (tight windows leave uneven white).
  const int clearTop = band.midY + kRuleThickness;
  const int clearH = std::max(0, band.contentBottom - clearTop);
  const Rect dirty{0, clearTop, renderer.getScreenWidth(), clearH};
  if (clearH > 0) {
    renderer.fillRect(0, clearTop, dirty.width, clearH, false);
  }
  drawUnderPanel(renderer, band, recentBooks, selectorIndex, stats, progressPercent, globalStats);
  return dirty;
}

bool ClockfaceThemeUi::formatHeroTimeNow(char* buf, size_t bufSize) { return formatHeroTime(buf, bufSize); }

Rect ClockfaceThemeUi::redrawClockBlock(GfxRenderer& renderer, char* outTime, size_t outTimeSize) {
  const ContentBand band = layoutContentBand(renderer);
  char timeBuf[16];
  formatHeroTime(timeBuf, sizeof(timeBuf));
  if (outTime && outTimeSize > 0) {
    snprintf(outTime, outTimeSize, "%s", timeBuf);
  }

  // Full-width upper band (incl. top margin through center rule) so windowed
  // FAST white is even — never a tight digit "white box" vs greyer paper.
  const int pageW = renderer.getScreenWidth();
  const int clearTop = 0;
  const int clearH = std::max(1, band.midY + kRuleThickness - clearTop);
  renderer.fillRect(0, clearTop, pageW, clearH, false);
  drawClockHalf(renderer, band);

  return Rect{0, clearTop, pageW, clearH};
}
