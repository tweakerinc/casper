#include "PenumbraTheme.h"

#include <EpdFontFamily.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "CrossPointSettings.h"
#include "RecentBooksStore.h"
#include "activities/reader/BookReadingStats.h"
#include "activities/reader/GlobalReadingStats.h"
#include "activities/reader/ReadingStatsUtils.h"
#include "components/themes/BaseTheme.h"
#include "fontIds.h"
#include "util/PenumbraNowReadingPolicy.h"
#include "util/StringUtils.h"
#include "util/SystemChromeLive.h"
#include "util/UiGhostPolicy.h"

namespace {
// Real 72 pt 2-bit Source Serif Bold (digits + colon only) — smooth AA, no pixel scale.
constexpr int kClockFontId = SOURCESERIF4_72_CLOCK_FONT_ID;
constexpr int kDayFontId = SOURCESERIF4_12_FONT_ID;
// All Penumbra home text is Source Serif 4.
// X3 under-panel title stack: caption 14 / title 18 / author 14.
// X4 Now Reading: caption 12 / title 16 / author 14. Author used to be 12pt
// with a 5px pair gap (v0.1.8 hairline dodge); that looked smaller and glued
// the name to the title. Hairline air is equal-gap G, not the pair gap.
constexpr int kLabelFontId = SOURCESERIF4_14_FONT_ID;
constexpr int kLabelFontIdX4 = SOURCESERIF4_12_FONT_ID;
constexpr int kTitleFontId = SOURCESERIF4_18_FONT_ID;
constexpr int kAuthorFontId = SOURCESERIF4_14_FONT_ID;
constexpr int kTitleFontIdX4 = SOURCESERIF4_16_FONT_ID;
constexpr int kAuthorFontIdX4 = SOURCESERIF4_14_FONT_ID;
// Recents list only: 10 pt title (bold when focused), 8 pt author.
// (Must stay on SOURCESERIF4_10 / _8 — not UI_10 which aliases 12 pt.)
constexpr int kRecentsTitleFontId = SOURCESERIF4_10_FONT_ID;
constexpr int kRecentsAuthorFontId = SOURCESERIF4_8_FONT_ID;
// Stats: X3 14/12. X4 is narrower in portrait (~480 vs ~528), so values stay
// 12 and labels drop to SMALL so full "Reading Time" / "Avg. Session" fit.
constexpr int kStatValueFontId = SOURCESERIF4_14_FONT_ID;
constexpr int kStatLabelFontId = SOURCESERIF4_12_FONT_ID;
constexpr int kStatValueFontIdX4 = SOURCESERIF4_12_FONT_ID;
// X4 stats labels: Source Serif 8 (same family as Recents author).
constexpr int kStatLabelFontIdX4 = SOURCESERIF4_8_FONT_ID;
// Book title on Stats/Lifetime pages (X3 under-panel only).
constexpr int kStatsBookTitleFontId = SOURCESERIF4_14_FONT_ID;

// Title/author wrap inset — same band as Recents list so widths stay uniform.
constexpr int kSideInset = 24;
constexpr int kStatsSideInset = 8;
// Clock ink bottom → weekday. Title→author uses penumbra::kTitleToAuthorGap
// (same 18px on X3 and X4) so the pair is not glued together on X4.
constexpr int kClockToDayGap = 18;
// NOW READING → book title.
constexpr int kLabelToTitleGap = 14;
constexpr int kLabelToTitleGapX4 = 12;
constexpr int kTitleMaxLines = 3;
constexpr int kTitleMaxLinesX4 = 2;
constexpr int kAuthorMaxLines = 2;
constexpr int kRuleThickness = 2;
constexpr int kRuleHalfWidth = 90;
// Value→label pull so pairs read as one unit (same idea as Focus stats).
constexpr int kValueLabelPull = 4;
constexpr int kStatRowGap = 12;
constexpr int kStatRowGapX4 = 10;
// Title ↔ grid and grid ↔ "STATS" / "LIFETIME" caption share the same air.
constexpr int kStatsStackGap = 16;
// X3 under-panel: keep the stats chrome clear of page dots, and top-pin tightly so
// Book Stats + Lifetime share the same title/grid/footer Y when side-swiping.
constexpr int kStatsTopInsetX3 = 2;
constexpr int kStatsFooterDotsClearance = 14;  // caption bottom → dots strip
// X4: grid slightly higher, caption slightly lower (more air under the grid).
constexpr int kStatsStackGapX4Top = 8;
constexpr int kStatsStackGapX4Footer = 22;
// X4 upper half: modest pad under chrome; internal gaps fill the rest of the half.
constexpr int kX4TitleTopPad = 12;
// Extra air below mid-hairline before the RECENTS caption (non-pinned layouts).
constexpr int kRecentsTopInset = 28;
constexpr int kRecentsTopInsetX4 = 12;
// Air between "RECENTS" caption and the first book row (X3). X4 uses equal-gap G.
constexpr int kRecentsCaptionToListGap = 22;
// Gap between last book row and "View All" (X3 only; X4 has no View All).
// listFocusIndex == bookCount means View All is focused.
constexpr int kRecentsViewAllGap = 12;
// Row gap inside the recents list (title/author/bar groups).
constexpr int kRecentsRowGap = 8;
constexpr int kRecentsRowGapX4 = 6;
// Under-panel page dots (both devices). Fixed strip above menu.
// X3: 4 dots (Title · Recents · Stats · Lifetime) when tracking is on.
// X4: no page dots (Recents only; tracking off).
constexpr int kPageDotR = 5;
constexpr int kPageDotGap = 22;      // center-to-center
constexpr int kPageDotsStripH = 40;  // room so dots sit mid-way between content and menu

const StrId kWeekdayIds[7] = {
    StrId::STR_WEEKDAY_SUNDAY,   StrId::STR_WEEKDAY_MONDAY, StrId::STR_WEEKDAY_TUESDAY,  StrId::STR_WEEKDAY_WEDNESDAY,
    StrId::STR_WEEKDAY_THURSDAY, StrId::STR_WEEKDAY_FRIDAY, StrId::STR_WEEKDAY_SATURDAY,
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
  uint8_t rtcWeekday = 0;
  if (!halClock.isAvailable() || !halClock.getDateTime(year, month, day, hour, minute, &rtcWeekday)) {
    return -1;
  }
  if (month < 1 || month > 12 || day < 1) return -1;

  // Apply user timezone so "Monday" matches the local calendar day (not UTC).
  const uint8_t offsetQ = std::min<uint8_t>(SETTINGS.clockUtcOffsetQ, 104);
  const int offsetMinutes = (static_cast<int>(offsetQ) - 48) * 15;
  int localMinutes = static_cast<int>(hour) * 60 + static_cast<int>(minute) + offsetMinutes;
  int dayDelta = 0;
  while (localMinutes < 0) {
    adjustDateByDays(year, month, day, -1);
    localMinutes += 24 * 60;
    dayDelta -= 1;
  }
  while (localMinutes >= 24 * 60) {
    adjustDateByDays(year, month, day, 1);
    localMinutes -= 24 * 60;
    dayDelta += 1;
  }

  // Prefer calendar math when year is usable (after HalClock century normalize).
  if (year >= 2000) {
    return static_cast<int>((6u + dayIndexSince2000(year, month, day)) % 7u);
  }

  // Fallback: hardware weekday (0=Sun) shifted by timezone day roll.
  int wd = (static_cast<int>(rtcWeekday % 7U) + dayDelta) % 7;
  if (wd < 0) wd += 7;
  return wd;
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

int measureWrappedInkHeight(const GfxRenderer& renderer, const int fontId, const int maxWidth, const char* text,
                            const int maxLines, const EpdFontFamily::Style style) {
  if (!text || !*text) return 0;
  const auto lines = renderer.wrappedText(fontId, text, maxWidth, maxLines, style);
  return penumbra::wrappedInkHeight(static_cast<int>(lines.size()), renderer.getLineHeight(fontId),
                                    renderer.getFontAscenderSize(fontId));
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

// Same ETA model as BookStatsView / Focus / Dashboard (progress + total time).
bool estimatedTimeLeftFromProgress(const BookReadingStats& stats, const float progressPercent, uint32_t& seconds) {
  seconds = 0;
  if (stats.isCompleted) return false;
  if (stats.estimatedTimeLeftSeconds > 0) {
    seconds = stats.estimatedTimeLeftSeconds;
    return true;
  }
  if (progressPercent > 0.0f && progressPercent < 100.0f && stats.totalReadingSeconds > 0) {
    const float remaining = (100.0f - progressPercent) / progressPercent;
    seconds = static_cast<uint32_t>(static_cast<float>(stats.totalReadingSeconds) * remaining + 0.5f);
    return seconds > 0;
  }
  return false;
}

// Prefer daily-pace est. finish (Dashboard/Focus/BookStatsView).
bool estimateFinishDateFromDailyPace(const BookReadingStats& stats, const ReadingStatsDateTime& today,
                                     const uint32_t estimatedReadingSeconds, ReadingStatsDate& outDate) {
  outDate = {};
  if (!today.isValid() || !stats.startDate.isValid() || estimatedReadingSeconds == 0 ||
      stats.totalReadingSeconds == 0) {
    return false;
  }
  const uint16_t elapsedDays = readingSpanDaysElapsed(stats.startDate, today.date);
  const uint16_t readingDays = std::max<uint16_t>(1, elapsedDays);
  const uint64_t estimatedCalendarSeconds =
      (static_cast<uint64_t>(estimatedReadingSeconds) * static_cast<uint64_t>(readingDays) * 86400ULL +
       static_cast<uint64_t>(stats.totalReadingSeconds) / 2ULL) /
      static_cast<uint64_t>(stats.totalReadingSeconds);
  if (estimatedCalendarSeconds == 0) return false;
  ReadingStatsDateTime estimatedFinish = today;
  addSecondsToReadingStatsDateTime(estimatedFinish,
                                   static_cast<uint32_t>(std::min<uint64_t>(estimatedCalendarSeconds, UINT32_MAX)));
  outDate = estimatedFinish.date;
  return outDate.isValid();
}

struct ContentBand {
  int contentTop = 0;
  int contentBottom = 0;
  int midY = 0;
  int halfH = 0;
  int centerX = 0;
  int textMaxW = 0;
  // X4: pin upper/lower blocks so the hairline is centered between author and RECENTS.
  int upperTop = 0;
  int lowerTop = 0;
  int pinGap = 0;  // X4 equal-gap G (Recents caption → first book uses this too).
  bool pinBlocks = false;
};

ContentBand layoutContentBand(const GfxRenderer& renderer) {
  ContentBand b;
  const int pageW = renderer.getScreenWidth();
  const int pageH = renderer.getScreenHeight();
  const auto& metrics = PenumbraMetrics::values;
  const int footerH = BaseTheme::frontButtonFooterLayoutH(renderer);
  b.centerX = pageW / 2;
  const bool hasChrome = homeNeedsSystemChrome();
  b.contentTop =
      hasChrome
          ? (BaseTheme::kTopChromeBatteryY + std::max(metrics.batteryHeight + 8, metrics.statusBarVerticalMargin) + 8)
          : 20;
  b.contentBottom = pageH - footerH;
  const int bandH = std::max(1, b.contentBottom - b.contentTop);
  b.midY = b.contentTop + bandH / 2;
  b.halfH = bandH / 2;
  b.textMaxW = std::max(40, pageW - kSideInset * 2);
  b.upperTop = b.contentTop;
  b.lowerTop = b.midY + kRuleThickness;
  b.pinBlocks = false;
  return b;
}

bool isX4Penumbra() { return !gpio.deviceIsX3(); }

int statValueFont() { return isX4Penumbra() ? kStatValueFontIdX4 : kStatValueFontId; }
int statLabelFont() { return isX4Penumbra() ? kStatLabelFontIdX4 : kStatLabelFontId; }

int statPairHeight(const GfxRenderer& renderer) {
  const int valueH = renderer.getLineHeight(statValueFont());
  const int labelH = renderer.getLineHeight(statLabelFont());
  return std::max(1, valueH - kValueLabelPull) + labelH;
}

void drawStatCell(const GfxRenderer& renderer, const int x, const int w, const int y, const int h, const char* value,
                  const char* label) {
  // Pad only for truncate budget; glyphs are centered in the full cell width.
  constexpr int kPadX = 2;
  const int vf = statValueFont();
  const int lf = statLabelFont();
  const int textW = std::max(1, w - kPadX * 2);
  const int valueLineH = renderer.getLineHeight(vf);
  const int labelLineH = renderer.getLineHeight(lf);
  const int pairH = std::max(1, valueLineH - kValueLabelPull) + labelLineH;
  const int textY = y + std::max(0, (h - pairH) / 2);
  const std::string visValue = renderer.truncatedText(vf, value ? value : "-", textW, EpdFontFamily::BOLD);
  const std::string visLabel = renderer.truncatedText(lf, label ? label : "", textW);
  const int valueW = renderer.getTextWidth(vf, visValue.c_str(), EpdFontFamily::BOLD);
  const int labelW = renderer.getTextWidth(lf, visLabel.c_str());
  renderer.drawText(vf, x + (w - valueW) / 2, textY, visValue.c_str(), true, EpdFontFamily::BOLD);
  renderer.drawText(lf, x + (w - labelW) / 2, textY + std::max(1, valueLineH - kValueLabelPull), visLabel.c_str(),
                    true);
}

// Section label (NOW READING / RECENTS / STATS / LIFETIME).
// Always REGULAR Source Serif — never bold. Default is the X3 14pt caption.
void drawSectionLabel(const GfxRenderer& renderer, const int centerX, const int y, const char* label,
                      const int fontId = kLabelFontId) {
  if (!label || !*label) return;
  char buf[48];
  snprintf(buf, sizeof(buf), "%s", label);
  toUpperAsciiInPlace(buf);
  constexpr auto kStyle = EpdFontFamily::REGULAR;
  const int lw = renderer.getTextWidth(fontId, buf, kStyle);
  renderer.drawText(fontId, centerX - lw / 2, y, buf, true, kStyle);
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

// Always the last-read book (index 0). List focus never affects this.
const RecentBook* lastReadBook(const std::vector<RecentBook>& books) {
  if (books.empty()) return nullptr;
  return &books[0];
}

// zoneTop/zoneBottom: vertical band to center the title stack (upper or lower half).
// ALWAYS paints the last-read book (index 0) — never the Recents list focus.
// textMaxW matches the Recents list width (same side inset) so the hero title is uniform.
void drawTitleAuthorInZone(const GfxRenderer& renderer, const ContentBand& band, const int zoneTop,
                           const int zoneBottom, const std::vector<RecentBook>& books) {
  const bool x4 = isX4Penumbra();
  const int labelFont = x4 ? kLabelFontIdX4 : kLabelFontId;
  const int titleFont = x4 ? kTitleFontIdX4 : kTitleFontId;
  const int authorFont = x4 ? kAuthorFontIdX4 : kAuthorFontId;
  const int labelGap = x4 ? kLabelToTitleGapX4 : kLabelToTitleGap;
  const int titleMaxLines = x4 ? kTitleMaxLinesX4 : kTitleMaxLines;
  const int centerX = band.centerX;
  // Same max width as the Recents under-panel list.
  const int textMaxW = band.textMaxW;
  // X4 pinBlocks: start at zoneTop (caller set equal-gap upperTop). Else center.
  const int zoneTopAdj = (x4 && band.pinBlocks) ? zoneTop : (zoneTop + (x4 ? kX4TitleTopPad : 0));
  const int zoneH = std::max(1, zoneBottom - zoneTopAdj);
  const char* label = tr(STR_NOW_READING);
  const int labelH = renderer.getLineHeight(labelFont);
  const int titleLineH = renderer.getLineHeight(titleFont);

  if (books.empty()) {
    const char* msg = tr(STR_NO_OPEN_BOOK);
    const int blockH = labelH + labelGap + titleLineH;
    int y = (x4 && band.pinBlocks) ? zoneTopAdj : (zoneTopAdj + std::max(0, (zoneH - blockH) / 2));
    drawSectionLabel(renderer, centerX, y, label, labelFont);
    y += labelH + labelGap;
    const int mw = renderer.getTextWidth(titleFont, msg, EpdFontFamily::BOLD);
    renderer.drawText(titleFont, centerX - mw / 2, y, msg, true, EpdFontFamily::BOLD);
    return;
  }

  const RecentBook& book = *lastReadBook(books);
  const char* title = book.title.empty() ? book.path.c_str() : book.title.c_str();
  const std::string authorDisplay =
      book.author.empty() ? std::string() : StringUtils::formatAuthorDisplayName(book.author);
  const char* author = authorDisplay.empty() ? nullptr : authorDisplay.c_str();

  const int titleH = measureWrappedHeight(renderer, titleFont, textMaxW, title, titleMaxLines, EpdFontFamily::BOLD);
  const int authorH =
      author ? measureWrappedHeight(renderer, authorFont, textMaxW, author, kAuthorMaxLines, EpdFontFamily::REGULAR)
             : 0;
  // Plain gap after the wrapped title block (not ink-pull) so 2-line titles don't
  // crowd the author. Same air on X3 and X4 (X4 used to use 5px).
  const int authorGap = penumbra::titleToAuthorGap();
  const int textBlockH = labelH + labelGap + titleH + (author ? (authorGap + authorH) : 0);

  int y = (x4 && band.pinBlocks) ? zoneTopAdj : (zoneTopAdj + std::max(4, (zoneH - textBlockH) / 2));
  drawSectionLabel(renderer, centerX, y, label, labelFont);
  y += labelH + labelGap;
  // drawCenteredWrapped advances y past every title line; authorGap is constant
  // whether the title used 1 or 2 lines (2-line titles push the author down).
  y = drawCenteredWrapped(renderer, titleFont, centerX, y, textMaxW, title, titleMaxLines, EpdFontFamily::BOLD);
  if (author) {
    y += authorGap;
    drawCenteredWrapped(renderer, authorFont, centerX, y, textMaxW, author, kAuthorMaxLines, EpdFontFamily::REGULAR);
  }
}

// Reserved strip for page dots when multi-page under-panel is active (X3 + tracking).
// X4 never draws page dots — do not reserve this band or the 4th recent is clipped.
int penumbraPageDotsStripH() {
  if (!gpio.deviceIsX3()) return 0;
  return SETTINGS.readingStatsTrackingEnabled() ? kPageDotsStripH : 0;
}

// --- X4 hairline placement: equal air above/below the rule between author and lower panel ---

int measureNowReadingBlockH(const GfxRenderer& renderer, const ContentBand& band,
                            const std::vector<RecentBook>& books) {
  const int labelFont = kLabelFontIdX4;
  const int titleFont = kTitleFontIdX4;
  const int authorFont = kAuthorFontIdX4;
  const int labelH = renderer.getLineHeight(labelFont);
  const int titleLineH = renderer.getLineHeight(titleFont);
  if (books.empty()) {
    return labelH + kLabelToTitleGapX4 + titleLineH;
  }
  const RecentBook& book = books[0];
  const char* title = book.title.empty() ? book.path.c_str() : book.title.c_str();
  const std::string authorDisplay =
      book.author.empty() ? std::string() : StringUtils::formatAuthorDisplayName(book.author);
  const int titleH =
      measureWrappedHeight(renderer, titleFont, band.textMaxW, title, kTitleMaxLinesX4, EpdFontFamily::BOLD);
  const int authorH = authorDisplay.empty()
                          ? 0
                          : measureWrappedInkHeight(renderer, authorFont, band.textMaxW, authorDisplay.c_str(),
                                                    kAuthorMaxLines, EpdFontFamily::REGULAR);
  return labelH + kLabelToTitleGapX4 + titleH + (authorH > 0 ? (penumbra::titleToAuthorGap() + authorH) : 0);
}

// Row: title (full width) · author+% on one line · gap · bar.
// % is right-aligned on the author line so the title can use the full bar width.
int recentsRowHeight(const GfxRenderer& renderer) {
  const int titleLineH = renderer.getLineHeight(kRecentsTitleFontId);
  // Ink height for author row (SS4 advanceY is oversized and left a dead band).
  const int authorInkH = renderer.getFontAscenderSize(kRecentsAuthorFontId);
  constexpr int kMicroBarH = 5;
  constexpr int kAuthorToBarGap = 4;
  return titleLineH + authorInkH + kAuthorToBarGap + kMicroBarH;
}

// X3: up to 4 books + View All. X4: up to 5 books, no View All (mid = Recents, sides scroll).
inline int penumbraRecentsListCap() { return isX4Penumbra() ? 5 : 4; }

int measureRecentsBlockH(const GfxRenderer& renderer, const ContentBand& band, const std::vector<RecentBook>& books,
                         const bool includeViewAll = false) {
  (void)band;
  const bool x4 = isX4Penumbra();
  const int captionH = x4 ? renderer.getFontAscenderSize(kLabelFontIdX4) : renderer.getLineHeight(kLabelFontId);
  const int titleLineH = renderer.getLineHeight(kRecentsTitleFontId);
  const int rowH = recentsRowHeight(renderer);
  const int rowGap = x4 ? kRecentsRowGapX4 : kRecentsRowGap;
  // X4 caption→list air is equal-gap G (applyX4HairlineLayout), not this constant.
  const int capToList = x4 ? 0 : kRecentsCaptionToListGap;
  const int viewAllGap = kRecentsViewAllGap;
  const int maxN = penumbraRecentsListCap();
  // Empty books: still reserve full list height so clock-minute re-layout does not jump midY.
  const int n = books.empty() ? maxN : std::min(static_cast<int>(books.size()), maxN);
  if (n <= 0) return captionH + capToList + titleLineH;
  const int listH = n * rowH + (n - 1) * rowGap;
  int h = captionH + capToList + listH;
  // View All only on X3 (includeViewAll is ignored on X4).
  if (includeViewAll && !x4 && n > 0) {
    h += viewAllGap + titleLineH;
  }
  return h;
}

int measureClockBlockH(const GfxRenderer& renderer) {
  const int clockInkH = renderer.getFontAscenderSize(kClockFontId);
  const int dayH = renderer.getLineHeight(kDayFontId);
  // Weekday always present on X3 layout measure (same as drawClockHalf when RTC has a day).
  return clockInkH + kClockToDayGap + dayH;
}

// X3 under-panel Title/Author height (12/18/14 stack — not the X4 Now Reading metrics).
int measureX3TitleAuthorBlockH(const GfxRenderer& renderer, const ContentBand& band,
                               const std::vector<RecentBook>& books) {
  const int labelH = renderer.getLineHeight(kLabelFontId);
  const int titleLineH = renderer.getLineHeight(kTitleFontId);
  if (books.empty()) {
    return labelH + kLabelToTitleGap + titleLineH;
  }
  const RecentBook& book = books[0];
  const char* title = book.title.empty() ? book.path.c_str() : book.title.c_str();
  const std::string authorDisplay =
      book.author.empty() ? std::string() : StringUtils::formatAuthorDisplayName(book.author);
  const int titleH =
      measureWrappedHeight(renderer, kTitleFontId, band.textMaxW, title, kTitleMaxLines, EpdFontFamily::BOLD);
  const int authorH = authorDisplay.empty()
                          ? 0
                          : measureWrappedHeight(renderer, kAuthorFontId, band.textMaxW, authorDisplay.c_str(),
                                                 kAuthorMaxLines, EpdFontFamily::REGULAR);
  return labelH + kLabelToTitleGap + titleH + (authorH > 0 ? (penumbra::titleToAuthorGap() + authorH) : 0);
}

int measureX3StatsStyleBlockH(const GfxRenderer& renderer) {
  const int captionH = renderer.getLineHeight(kLabelFontId);
  const int pairH = statPairHeight(renderer);
  // Book stats on X3 is 3 rows when RTC tracking is on (Started + Est. Finish),
  // matching BookStatsView / Dashboard. Lifetime stays 2 rows but shares this max
  // so equal-gap layout (and footer Y) stay mode-independent.
  const int gridH = pairH * 3 + kStatRowGap * 2;
  const int titleH = renderer.getLineHeight(kStatsBookTitleFontId);
  // Compact stack: title → gap → grid → gap → caption → dots clearance.
  // Must match layoutX3StatsChrome() so G / lowerTop stay stable across pages.
  return kStatsTopInsetX3 + titleH + kStatsStackGap + gridH + kStatsStackGap + captionH + kStatsFooterDotsClearance;
}

// Stable lower-block height for X3 equal-gap layout. MUST be mode-independent:
// partial under-panel swaps (side L/R) only white-fill below the hairline. If midY
// jumps when switching Title↔Recents↔Stats, the clear band misses old ink and the
// new panel paints on top of the previous one (overlap / "Stats box over Recents").
// Only one under-panel is drawn per frame — no layered modes — but e-ink keeps
// uncleared pixels until they are filled white.
int measureX3LowerBlockH(const GfxRenderer& renderer, const ContentBand& band, const std::vector<RecentBook>& books) {
  const int titleH = measureX3TitleAuthorBlockH(renderer, band, books);
  const int recentsH = measureRecentsBlockH(renderer, band, books, /*includeViewAll=*/true);
  const int statsH = measureX3StatsStyleBlockH(renderer);
  return std::max({titleH, recentsH, statsH});
}

// X3: three equal air gaps — top→clock, date→hairline, hairline→lower panel —
// so the hairline is not glued to Recents. Leaves dots strip free below the list
// (View All sits in the Recents block). Hairline Y is fixed across under-panel modes.
void applyX3EqualSpacingLayout(const GfxRenderer& renderer, ContentBand& band, const std::vector<RecentBook>& books) {
  PenumbraThemeUi::clampUnderModeToTracking();
  const int clockH = measureClockBlockH(renderer);
  const int Lh = measureX3LowerBlockH(renderer, band, books);
  const int dots = penumbraPageDotsStripH();
  const int floorY = band.contentBottom - dots;
  int free = floorY - band.contentTop - clockH - Lh - kRuleThickness;
  if (free < 6) free = 6;
  const int G = free / 3;

  band.upperTop = band.contentTop + G;     // clock group top
  band.midY = band.upperTop + clockH + G;  // hairline (stable across modes)
  band.halfH = std::max(1, band.midY - band.contentTop);
  band.lowerTop = band.midY + kRuleThickness + G;  // lower caption / list start
  band.pinBlocks = true;
}

// X4 equal vertical rhythm (five matching air gaps G):
//   status bar → NOW READING
//   author     → hairline
//   hairline   → RECENTS
//   RECENTS    → first book
//   last book  → menu
// List is up to 5 books; no View All (sides scroll; mid button opens full Recents).
void applyX4HairlineLayout(const GfxRenderer& renderer, ContentBand& band, const std::vector<RecentBook>& books) {
  PenumbraThemeUi::clampUnderModeToTracking();  // X4 → Recents only

  const int Uh = measureNowReadingBlockH(renderer, band, books);
  const int recentsH = measureRecentsBlockH(renderer, band, books, /*includeViewAll=*/false);
  const int captionH = renderer.getFontAscenderSize(kLabelFontIdX4);
  const int listH = std::max(0, recentsH - captionH);
  const int contentH = std::max(1, band.contentBottom - band.contentTop);
  const auto g = penumbra::x4HomeGaps(band.contentTop, contentH, Uh, captionH, listH, kRuleThickness);

  band.upperTop = g.upperTop;
  band.midY = g.midY;
  band.halfH = std::max(1, band.midY - band.contentTop);
  band.lowerTop = g.recentsTop;
  band.pinGap = g.G;
  band.pinBlocks = true;
}

// X3 under-panel: title/author for the last-read book (index 0).
void drawTitleAuthorPanel(const GfxRenderer& renderer, const ContentBand& band, const std::vector<RecentBook>& books) {
  const int zoneBottom = band.contentBottom - penumbraPageDotsStripH();
  const int zoneTop = band.pinBlocks ? band.lowerTop : (band.midY + kRuleThickness);
  drawTitleAuthorInZone(renderer, band, zoneTop, zoneBottom, books);
}

// Solid filled circle (active page indicator).
void drawSolidCircle(const GfxRenderer& renderer, const int cx, const int cy, const int r) {
  if (r <= 0) return;
  const int r2 = r * r;
  for (int dy = -r; dy <= r; ++dy) {
    for (int dx = -r; dx <= r; ++dx) {
      if (dx * dx + dy * dy <= r2) {
        renderer.drawPixel(cx + dx, cy + dy, true);
      }
    }
  }
}

// Crisp ring outline (inactive page). Midpoint circle — true round, not a dither square.
void drawCircleOutline(const GfxRenderer& renderer, const int cx, const int cy, const int radius) {
  if (radius <= 0) return;
  int x = radius;
  int y = 0;
  int err = 1 - x;
  auto plot8 = [&](const int px, const int py) {
    renderer.drawPixel(cx + px, cy + py, true);
    renderer.drawPixel(cx + py, cy + px, true);
    renderer.drawPixel(cx - py, cy + px, true);
    renderer.drawPixel(cx - px, cy + py, true);
    renderer.drawPixel(cx - px, cy - py, true);
    renderer.drawPixel(cx - py, cy - px, true);
    renderer.drawPixel(cx + py, cy - px, true);
    renderer.drawPixel(cx + px, cy - py, true);
  };
  while (x >= y) {
    plot8(x, y);
    ++y;
    if (err < 0) {
      err += 2 * y + 1;
    } else {
      --x;
      err += 2 * (y - x) + 1;
    }
  }
}

// pageIndex / count: device-specific (X3=4, X4=3). No-op when tracking is off.
// Active = filled black disk; inactive = hollow circle (paper interior).
// Vertically centered in the strip between under-panel content and the menu bar.
void drawPenumbraPageDots(const GfxRenderer& renderer, const ContentBand& band, const int pageIndex, const int count) {
  const int stripH = penumbraPageDotsStripH();
  if (stripH <= 0 || count < 2) return;

  const int active = std::clamp(pageIndex, 0, count - 1);
  const int totalW = (count - 1) * kPageDotGap;
  const int startX = band.centerX - totalW / 2;
  const int cy = band.contentBottom - stripH / 2;
  const int r = kPageDotR;

  for (int i = 0; i < count; ++i) {
    const int cx = startX + i * kPageDotGap;
    if (i == active) {
      drawSolidCircle(renderer, cx, cy, r + 1);
      drawCircleOutline(renderer, cx, cy, r);
    } else {
      drawCircleOutline(renderer, cx, cy, r);
    }
  }
}

// Solid micro progress bar — pure black outline + black fill (no dither).
// Always drawn at a fixed row slot; never tied to list focus.
// Independent of Manage Reader UI → Progress Bar (reader chrome only).
void drawMicroProgressBar(const GfxRenderer& renderer, const int x, const int y, const int w, const int h,
                          const int pct /* -1 unknown, 0..100 */) {
  if (w < 4 || h < 2) return;
  renderer.drawRect(x, y, w, h, true);
  if (pct <= 0) return;
  const int innerW = w - 2;
  const int innerH = h - 2;
  if (innerW <= 0 || innerH <= 0) return;
  const int fillW = std::clamp((innerW * std::min(pct, 100) + 50) / 100, 0, innerW);
  if (fillW > 0) {
    renderer.fillRect(x + 1, y + 1, fillW, innerH, true);
  }
}

// Progress % cache — kept in RAM across Home resume. SD only for paths we have
// never seen (or after invalidate). Reader exit updates the current book via
// updateRecentsProgressForPath so we never re-read all N stats files on Back.
// Never re-scan on Down scroll / minute tick. Sized for X4's 5-row list.
constexpr int kRecentsPctCacheMax = 5;
struct RecentsPctCache {
  std::string path[kRecentsPctCacheMax];
  float pct[kRecentsPctCacheMax];
  int n = 0;
};
RecentsPctCache g_recentsPctCache;

float lookupCachedProgressByPath(const std::string& path) {
  for (int i = 0; i < g_recentsPctCache.n; ++i) {
    if (g_recentsPctCache.path[i] == path) return g_recentsPctCache.pct[i];
  }
  return -999.0f;  // sentinel: not in cache (distinct from unknown -1)
}

void ensureRecentsProgressCache(const std::vector<RecentBook>& books, const int /*nDisplay*/,
                                const bool forceReload = false) {
  // Always cache the full on-panel list capacity (not the fit count for this paint).
  const int count = std::min(static_cast<int>(books.size()), kRecentsPctCacheMax);

  // Exact path-order match and not forced → nothing to do (draw / second warm).
  if (!forceReload && g_recentsPctCache.n == count) {
    bool hit = true;
    for (int i = 0; i < count; ++i) {
      if (g_recentsPctCache.path[i] != books[static_cast<size_t>(i)].path) {
        hit = false;
        break;
      }
    }
    if (hit) return;
  }

  // Snapshot prior entries so reorder / resume can reuse % without SD.
  RecentsPctCache prior = g_recentsPctCache;
  g_recentsPctCache.n = count;
  int sdLoads = 0;
  for (int i = 0; i < count; ++i) {
    const std::string& path = books[static_cast<size_t>(i)].path;
    g_recentsPctCache.path[i] = path;
    float pct = -999.0f;
    if (!forceReload) {
      for (int j = 0; j < prior.n; ++j) {
        if (prior.path[j] == path) {
          pct = prior.pct[j];
          break;
        }
      }
    }
    if (pct < -900.0f) {
      // Prefer progress embedded in recent.json (CrossPointStats); one book-dir load only if unknown.
      pct = books[static_cast<size_t>(i)].progressPercentMilli == 0xFFFF
                ? BookReadingStats::loadForBook(path).getProgressPercent()
                : static_cast<float>(books[static_cast<size_t>(i)].progressPercentMilli) / 100.0f;
      if (books[static_cast<size_t>(i)].progressPercentMilli == 0xFFFF) ++sdLoads;
      LOG_DBG("HOME", "Recents progress cache[%d] %.1f%% %s", i, static_cast<double>(pct), path.c_str());
    } else {
      LOG_DBG("HOME", "Recents progress cache[%d] reuse %.1f%% %s", i, static_cast<double>(pct), path.c_str());
    }
    g_recentsPctCache.pct[i] = pct;
  }
  if (sdLoads == 0 && count > 0) {
    LOG_DBG("HOME", "Recents progress cache: %d rows, 0 SD loads (RAM reuse)", count);
  }
}

float recentsCachedProgress(const int i) {
  if (i < 0 || i >= g_recentsPctCache.n) return -1.0f;
  return g_recentsPctCache.pct[i];
}

// Minimalist recents list:
// - Title full width; author + % on one line; micro bar under author.
// - X3: up to 4 books + View All under the list.
// - X4: up to 5 books, no View All (sides scroll; mid = Recents).
// - listFocusIndex is list-only; upper "Now Reading" is always books[0].
void drawRecentsListPanel(const GfxRenderer& renderer, const ContentBand& band, const std::vector<RecentBook>& books,
                          const int listFocusIndex) {
  const bool x4 = isX4Penumbra();
  // View All only on X3 (full list is a front Recents button on X4).
  const bool showViewAll = !x4;
  const int zoneTop = band.midY + kRuleThickness;
  const int zoneBottom = band.contentBottom - penumbraPageDotsStripH();
  const int zoneH = std::max(1, zoneBottom - zoneTop);
  const int centerX = band.centerX;
  const int listLeft = centerX - band.textMaxW / 2;
  const int listW = band.textMaxW;

  const int captionFont = x4 ? kLabelFontIdX4 : kLabelFontId;
  const int captionH = renderer.getLineHeight(captionFont);
  const int titleFont = kRecentsTitleFontId;
  const int authorFont = kRecentsAuthorFontId;
  const int titleLineH = renderer.getLineHeight(titleFont);
  const int authorInkH = renderer.getFontAscenderSize(authorFont);
  constexpr int kMicroBarH = 5;
  constexpr int kAuthorToBarGap = 4;
  const int kRowGap = x4 ? kRecentsRowGapX4 : kRecentsRowGap;
  const int capToList = (x4 && band.pinBlocks && band.pinGap > 0) ? band.pinGap : kRecentsCaptionToListGap;
  const int viewAllGap = kRecentsViewAllGap;
  const int rowH = titleLineH + authorInkH + kAuthorToBarGap + kMicroBarH;

  const int topInset = x4 ? kRecentsTopInsetX4 : kRecentsTopInset;
  const int viewAllH = showViewAll ? (viewAllGap + titleLineH) : 0;

  const int capped = std::min(static_cast<int>(books.size()), penumbraRecentsListCap());
  int n = capped;
  if (!x4 && !band.pinBlocks) {
    const int spaceForRows = std::max(0, zoneH - topInset - captionH - capToList - viewAllH - 2);
    int maxFit = 0;
    if (rowH > 0 && spaceForRows >= rowH) {
      maxFit = 1 + std::max(0, spaceForRows - rowH) / (rowH + kRowGap);
    }
    n = maxFit > 0 ? std::min(capped, maxFit) : capped;
  }
  const int focusHi = (showViewAll && n > 0) ? n : std::max(0, n - 1);
  const int focus = n > 0 ? std::clamp(listFocusIndex, 0, focusHi) : 0;
  ensureRecentsProgressCache(books, n);

  const int listH = n > 0 ? (n * rowH + (n - 1) * kRowGap) : titleLineH;
  const int blockH = captionH + capToList + listH + (n > 0 ? viewAllH : 0);
  int y;
  if (band.pinBlocks) {
    y = band.lowerTop;
  } else {
    y = zoneTop + std::max(topInset, (zoneH - blockH) / 2);
    if (y + blockH > zoneBottom - 2) {
      y = std::max(zoneTop + topInset, zoneBottom - blockH - 2);
    }
  }

  drawSectionLabel(renderer, centerX, y, tr(STR_RECENTS), captionFont);
  const int captionAdvance = (x4 && band.pinBlocks) ? renderer.getFontAscenderSize(captionFont) : captionH;
  y += captionAdvance + capToList;

  if (n == 0) {
    const char* empty = tr(STR_NO_OPEN_BOOK);
    const int ew = renderer.getTextWidth(titleFont, empty, EpdFontFamily::REGULAR);
    renderer.drawText(titleFont, centerX - ew / 2, y, empty, true, EpdFontFamily::REGULAR);
    return;
  }

  for (int i = 0; i < n; ++i) {
    // X4 pin layout: always paint all n rows (measured into equal-gap Lh).
    if (!x4 && !band.pinBlocks && y + rowH > zoneBottom) break;
    const RecentBook& book = books[static_cast<size_t>(i)];
    const bool focused = (i == focus);
    const char* title = book.title.empty() ? book.path.c_str() : book.title.c_str();
    const std::string authorDisplay =
        book.author.empty() ? std::string() : StringUtils::formatAuthorDisplayName(book.author);

    const float progress = recentsCachedProgress(i);
    char pctBuf[8] = "";
    int pct = -1;
    if (progress >= 0.0f) {
      pct = static_cast<int>(progress + 0.5f);
      if (pct < 0) pct = 0;
      if (pct > 100) pct = 100;
      snprintf(pctBuf, sizeof(pctBuf), "%d%%", pct);
    }

    const auto titleStyle = focused ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    const std::string titleVis = renderer.truncatedText(titleFont, title, listW, titleStyle);
    renderer.drawText(titleFont, listLeft, y, titleVis.c_str(), true, titleStyle);
    y += titleLineH;

    const int authorTop = y;
    int authorMaxW = listW;
    if (pctBuf[0]) {
      const int pw = renderer.getTextWidth(authorFont, pctBuf, EpdFontFamily::REGULAR);
      authorMaxW = std::max(40, listW - pw - 8);
    }
    if (!authorDisplay.empty()) {
      const std::string authorVis =
          renderer.truncatedText(authorFont, authorDisplay.c_str(), authorMaxW, EpdFontFamily::REGULAR);
      renderer.drawText(authorFont, listLeft, authorTop, authorVis.c_str(), true, EpdFontFamily::REGULAR);
    }
    if (pctBuf[0]) {
      const int pw = renderer.getTextWidth(authorFont, pctBuf, EpdFontFamily::REGULAR);
      renderer.drawText(authorFont, listLeft + listW - pw, authorTop, pctBuf, true, EpdFontFamily::REGULAR);
    }

    const int barTop = authorTop + authorInkH + kAuthorToBarGap;
    drawMicroProgressBar(renderer, listLeft, barTop, listW, kMicroBarH, pct);
    y = barTop + kMicroBarH + kRowGap;
  }

  // X3 only: View All under the last book.
  if (showViewAll && n > 0) {
    y += viewAllGap - kRowGap;  // last row already added kRowGap
    if (y + titleLineH <= zoneBottom + 2) {
      const bool viewAllFocused = (focus == n);
      const char* label = tr(STR_VIEW_ALL);
      const auto style = viewAllFocused ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
      const int lw = renderer.getTextWidth(titleFont, label, style);
      renderer.drawText(titleFont, centerX - lw / 2, y, label, true, style);
    }
  }
}

const char* dayCountText(const uint16_t days) { return days == 1 ? tr(STR_STATS_DAY) : tr(STR_STATS_DAYS); }

// Single line so the 3×2 stats grid never shifts when titles are long.
constexpr int kStatsTitleMaxLines = 1;

// Stats / Lifetime captions always describe the last-read book (index 0).
const char* lastReadBookTitle(const std::vector<RecentBook>& books) {
  if (books.empty()) return tr(STR_NO_OPEN_BOOK);
  const RecentBook& book = books[0];
  return book.title.empty() ? book.path.c_str() : book.title.c_str();
}

// Shared X3 Book-Stats / Lifetime chrome. Both pages must use the same titleY /
// gridY / footerY so side-swiping does not jump. Footer is locked to the tall
// (3-row) stack so Lifetime's shorter grid still lands the caption in sync, and
// clearance keeps "STATS" / "LIFETIME" above the page-dot strip.
struct X3StatsChrome {
  int titleY = 0;
  int gridY = 0;
  int footerY = 0;
  int gridAvailH = 0;  // space reserved for the tallest (3-row) grid
  int zoneTop = 0;
  int zoneBottom = 0;
};

X3StatsChrome layoutX3StatsChrome(const GfxRenderer& renderer, const ContentBand& band, const bool showBookTitle,
                                  const int titleH) {
  X3StatsChrome out;
  out.zoneTop = band.pinBlocks ? band.lowerTop : (band.midY + kRuleThickness);
  out.zoneBottom = band.contentBottom - penumbraPageDotsStripH();
  const int captionH = renderer.getLineHeight(kLabelFontId);
  const int pairH = statPairHeight(renderer);
  const int maxGridH = pairH * 3 + kStatRowGap * 2;

  const int th = showBookTitle ? titleH : 0;
  const int titleGap = showBookTitle ? kStatsStackGap : 0;

  // Prefer a compact top-aligned stack (raises the whole section vs the old
  // bottom-parked caption that collided with the dots).
  int y = out.zoneTop + kStatsTopInsetX3;
  out.titleY = y;
  y += th + titleGap;
  out.gridY = y;

  const int maxFooterY = out.zoneBottom - captionH - kStatsFooterDotsClearance;
  int footerY = out.gridY + maxGridH + kStatsStackGap;
  if (footerY > maxFooterY) footerY = maxFooterY;
  // Never let the caption sit under the grid with less than a stack gap, and
  // never let it invade the dots strip.
  if (footerY < out.gridY + kStatsStackGap) footerY = out.gridY + kStatsStackGap;
  if (footerY > maxFooterY) footerY = maxFooterY;
  out.footerY = footerY;
  out.gridAvailH = std::max(1, footerY - kStatsStackGap - out.gridY);
  return out;
}

// Layout:
//   X3: book title — grid — STATS/LIFETIME footer caption (shared Ys via layoutX3StatsChrome).
//   X4: same shell as Recents — section caption on top, grid below (no book title;
//       last-read lives in the upper Now Reading panel). Unified vertical placement.
void drawStatsStylePanel(const GfxRenderer& renderer, const ContentBand& band, const char* bookTitle,
                         const char* sectionCaption, const char* const* values, const char* const* labels,
                         const bool showBookTitle = true) {
  const bool x4 = isX4Penumbra();
  const int rowGap = x4 ? kStatRowGapX4 : kStatRowGap;
  const int titleH = showBookTitle ? measureWrappedHeight(renderer, kStatsBookTitleFontId, band.textMaxW, bookTitle,
                                                          kStatsTitleMaxLines, EpdFontFamily::BOLD)
                                   : 0;
  const int captionH = renderer.getLineHeight(kLabelFontId);
  const int pairH = statPairHeight(renderer);
  const int gridH = pairH * 2 + rowGap;

  // Zone: mid rule → menu (minus page-dot strip when multi-page under-panel is on).
  const int zoneTop = band.pinBlocks ? band.lowerTop : (band.midY + kRuleThickness);
  const int zoneBottom = band.contentBottom - penumbraPageDotsStripH();

  const int pageW = renderer.getScreenWidth();
  const int gridW = std::max(40, pageW - kStatsSideInset * 2);

  if (x4) {
    // Match Recents start Y exactly. Short stats blocks used to vertical-center
    // with X3's +10 down-bias and the larger X3 top inset (28), so STATS/LIFETIME
    // sat noticeably lower than RECENTS (which top-pins on X4 with inset 16).
    // Top-align the caption with Recents, then stretch the 3×2 grid into the
    // same under-panel band so spacing feels continuous across pages.
    const int topInset = kRecentsTopInsetX4;
    int y = zoneTop + topInset;
    if (sectionCaption) {
      drawSectionLabel(renderer, band.centerX, y, sectionCaption);
      y += captionH + kRecentsCaptionToListGap;
    }
    // Fill remaining zone (above page-dot strip) so row air matches Recents weight.
    const int minGridH = gridH;
    const int avail = std::max(minGridH, zoneBottom - 2 - y);
    // Cap stretch so pairs don't float too far apart on a tall panel, but
    // allow enough extra air that the page weight matches Recents.
    const int maxGridH = minGridH + pairH * 2;
    const int drawnGridH = std::clamp(avail, minGridH, maxGridH);
    drawStatGrid(renderer, band.centerX, y, gridW, drawnGridH, /*cols=*/3, /*rows=*/2, values, labels);
    return;
  }

  // X3: shared chrome with Book Stats so Lifetime stays vertically in sync.
  const X3StatsChrome chrome = layoutX3StatsChrome(renderer, band, showBookTitle, titleH);
  int y = chrome.titleY;
  if (showBookTitle) {
    y = drawCenteredWrapped(renderer, kStatsBookTitleFontId, band.centerX, y, band.textMaxW, bookTitle,
                            kStatsTitleMaxLines, EpdFontFamily::BOLD);
    (void)y;
  }
  // Lifetime is 2 rows; keep natural pair height (do not stretch into the 3-row
  // reserve — empty air under the grid is what keeps the footer Y matched).
  drawStatGrid(renderer, band.centerX, chrome.gridY, gridW, gridH, /*cols=*/3, /*rows=*/2, values, labels);
  if (sectionCaption) {
    drawSectionLabel(renderer, band.centerX, chrome.footerY, sectionCaption);
  }
}

// Book stats — match BookStatsView / Dashboard (when it worked with RTC dates):
//   Sessions | Reading Time | Progress
//   Avg. Session | Time Left | Pages/Min
//   Days reading (Started DATE) | Est. Finish / Finished Date   [X3 RTC only]
// X4: 3×2 without date row (no RTC tracking on home).
// showBookTitle: X3 under-panel yes.
void drawBookStatsPanel(const GfxRenderer& renderer, const ContentBand& band, const char* bookTitle,
                        const BookReadingStats* stats, const float progressPercent, const bool showBookTitle) {
  const BookReadingStats empty{};
  const BookReadingStats& s = stats != nullptr ? *stats : empty;
  const bool x4 = isX4Penumbra();
  // X3 home tracks RTC; X4 under-panel is Recents-only but keep a compact 3×2 if called.
  const bool showRtcDates = !x4;

  char vSessions[24];
  char vTime[32];
  char vProgress[24];
  char vAvg[32];
  char vLeft[32];
  char vPace[24];
  char vDays[32];
  char vFinish[24];
  char startedLabel[40];
  char finishLabel[40];

  snprintf(vSessions, sizeof(vSessions), "%u", static_cast<unsigned>(s.sessionCount));
  BookReadingStats::formatDuration(s.totalReadingSeconds, vTime, sizeof(vTime));
  if (progressPercent >= 0.0f) {
    snprintf(vProgress, sizeof(vProgress), "%d%%", static_cast<int>(progressPercent + 0.5f));
  } else {
    snprintf(vProgress, sizeof(vProgress), "-");
  }
  const uint32_t avgSecs = s.sessionCount > 0 ? s.totalReadingSeconds / s.sessionCount : 0;
  BookReadingStats::formatDuration(avgSecs, vAvg, sizeof(vAvg));

  uint32_t estimatedSeconds = 0;
  const bool hasEstimate = estimatedTimeLeftFromProgress(s, progressPercent, estimatedSeconds);
  if (hasEstimate && !s.isCompleted) {
    formatCompactDuration(estimatedSeconds, vLeft, sizeof(vLeft));
  } else {
    snprintf(vLeft, sizeof(vLeft), "-");
  }
  snprintf(vPace, sizeof(vPace), "%.1f", pagesPerMinute(s.totalPagesTurned, s.totalReadingSeconds));

  if (!showRtcDates) {
    // Compact 3×2 (no dates): Progress | Time | Left / Sessions | Avg | Pace
    const char* values[6] = {vProgress, vTime, vLeft, vSessions, vAvg, vPace};
    const char* labels[6] = {tr(STR_STATS_PROGRESS_LBL), tr(STR_STATS_TIME_LBL),        tr(STR_TIME_LEFT),
                             tr(STR_STATS_SESSIONS_LBL), tr(STR_STATS_AVG_SESSION_LBL), tr(STR_STATS_PAGES_PER_MIN)};
    drawStatsStylePanel(renderer, band, bookTitle, tr(STR_STATS), values, labels, showBookTitle);
    return;
  }

  // RTC date row — same fields as BookStatsView drawPerBookStatsCard row 3.
  ReadingStatsDateTime today;
  const bool hasToday = getCurrentLocalReadingStatsDateTime(today);
  const ReadingStatsDate endDate =
      s.isCompleted && s.finishedDate.isValid() ? s.finishedDate : (hasToday ? today.date : ReadingStatsDate{});
  const bool hasDaySpan = s.startDate.isValid() && endDate.isValid();
  const uint16_t daysReading = hasDaySpan ? readingSpanDaysElapsed(s.startDate, endDate) : 0;
  if (hasDaySpan) {
    snprintf(vDays, sizeof(vDays), "%u %s", static_cast<unsigned>(daysReading), dayCountText(daysReading));
  } else {
    snprintf(vDays, sizeof(vDays), "-");
  }
  char dateBuf[24];
  formatReadingStatsShortDate(s.startDate, dateBuf, sizeof(dateBuf));
  snprintf(startedLabel, sizeof(startedLabel), "%s %s", tr(STR_STATS_STARTED), dateBuf);

  ReadingStatsDate finishDisplayDate;
  if (s.isCompleted) {
    finishDisplayDate = s.finishedDate;
  } else if (hasToday && hasEstimate) {
    if (!estimateFinishDateFromDailyPace(s, today, estimatedSeconds, finishDisplayDate)) {
      ReadingStatsDateTime estimatedFinish = today;
      addSecondsToReadingStatsDateTime(estimatedFinish, estimatedSeconds);
      finishDisplayDate = estimatedFinish.date;
    }
  }
  formatReadingStatsShortDate(finishDisplayDate, vFinish, sizeof(vFinish));
  if (!finishDisplayDate.isValid()) {
    snprintf(vFinish, sizeof(vFinish), "-");
  }
  snprintf(finishLabel, sizeof(finishLabel), "%s",
           s.isCompleted ? tr(STR_STATS_FINISHED_DATE) : tr(STR_STATS_EST_FINISH_DATE));

  // Custom 3-row panel: 3+3+2 cells (dates are half-width), same chrome as StatsStyle.
  const int rowGap = kStatRowGap;
  const int titleH = showBookTitle ? measureWrappedHeight(renderer, kStatsBookTitleFontId, band.textMaxW, bookTitle,
                                                          kStatsTitleMaxLines, EpdFontFamily::BOLD)
                                   : 0;
  const int pairH = statPairHeight(renderer);
  const int twoRowH = pairH * 2 + rowGap;
  const int thirdRowH = pairH;
  const int gridH = twoRowH + rowGap + thirdRowH;

  const int pageW = renderer.getScreenWidth();
  const int gridW = std::max(40, pageW - kStatsSideInset * 2);

  // Same title / grid / footer Y as Lifetime (drawStatsStylePanel) so side-swipe stays synced.
  const X3StatsChrome chrome = layoutX3StatsChrome(renderer, band, showBookTitle, titleH);
  int y = chrome.titleY;
  if (showBookTitle) {
    y = drawCenteredWrapped(renderer, kStatsBookTitleFontId, band.centerX, y, band.textMaxW, bookTitle,
                            kStatsTitleMaxLines, EpdFontFamily::BOLD);
    (void)y;
  }

  // Prefer natural 3-row height; if the shared chrome compressed gridAvailH, shrink
  // row air rather than letting the date row collide with the STATS caption.
  const int drawnGridH = std::min(gridH, chrome.gridAvailH);
  const int drawnTwoRowH = (drawnGridH >= gridH) ? twoRowH : std::max(pairH, (drawnGridH * twoRowH) / gridH);
  const int drawnThirdH = std::max(pairH / 2, drawnGridH - drawnTwoRowH - rowGap);

  // Row 0–1: 3 columns (BookStatsView card layout).
  const char* topValues[6] = {vSessions, vTime, vProgress, vAvg, vLeft, vPace};
  const char* topLabels[6] = {tr(STR_STATS_SESSIONS_LBL),    tr(STR_STATS_TIME_LBL), tr(STR_STATS_PROGRESS_LBL),
                              tr(STR_STATS_AVG_SESSION_LBL), tr(STR_TIME_LEFT),      tr(STR_STATS_PAGES_PER_MIN)};
  drawStatGrid(renderer, band.centerX, chrome.gridY, gridW, drawnTwoRowH, /*cols=*/3, /*rows=*/2, topValues, topLabels);
  y = chrome.gridY + drawnTwoRowH + ((drawnGridH >= gridH) ? rowGap : std::max(2, rowGap / 2));

  // Row 2: half-width Started | Est. Finish
  const int halfW = gridW / 2;
  const int usedW = halfW * 2;
  const int left = band.centerX - usedW / 2;
  drawStatCell(renderer, left, halfW, y, drawnThirdH, vDays, startedLabel);
  drawStatCell(renderer, left + halfW, halfW, y, drawnThirdH, vFinish, finishLabel);

  drawSectionLabel(renderer, band.centerX, chrome.footerY, tr(STR_STATS));
}

// Lifetime — 3×2:
//   Sessions | Reading Time | Pages/Min
//   Avg. Session | Books Read | Streak (X3) or Pages Turned (X4, no RTC)
void drawLifetimePanel(const GfxRenderer& renderer, const ContentBand& band, const char* bookTitle,
                       const GlobalReadingStats* globalStats, const bool showBookTitle, const bool useStreak) {
  const GlobalReadingStats empty{};
  const GlobalReadingStats& g = globalStats != nullptr ? *globalStats : empty;

  char vSessions[24];
  char vTime[32];
  char vPace[24];
  char vAvg[32];
  char vBooks[24];
  char vSixth[32];
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
  if (useStreak) {
    const uint16_t longest = g.displayLongestReadingStreak();
    if (longest > 0) {
      snprintf(vSixth, sizeof(vSixth), "%u %s", static_cast<unsigned>(longest), dayCountText(longest));
    } else {
      snprintf(vSixth, sizeof(vSixth), "-");
    }
  } else {
    // X4: avg pages per session (stays modest; total pages would grow forever).
    const uint32_t avgPages = g.totalSessions > 0 ? g.totalPagesTurned / g.totalSessions : 0;
    snprintf(vSixth, sizeof(vSixth), "%lu", static_cast<unsigned long>(avgPages));
  }

  const char* values[6] = {vSessions, vTime, vPace, vAvg, vBooks, vSixth};
  // Full labels; X4 label font is SMALL so "Reading Time" / "Avg. Session" fit.
  const char* labels[6] = {
      tr(STR_STATS_SESSIONS_LBL),  tr(STR_STATS_TIME_LBL),
      tr(STR_STATS_PAGES_PER_MIN), tr(STR_STATS_AVG_SESSION_LBL),
      tr(STR_STATS_COMPLETED_LBL), useStreak ? tr(STR_STATS_LONGEST_STREAK_LBL) : tr(STR_STATS_PAGES_SHORT_LBL)};
  drawStatsStylePanel(renderer, band, bookTitle, tr(STR_STATS_ALL_TIME), values, labels, showBookTitle);
}

// X3 lower half: Title/Author → Recents → Stats → Lifetime (tracking on).
// Title/Author + stats always describe the last-read book (index 0).
// listFocusIndex only highlights a row in the Recents page.
void drawUnderPanelX3(const GfxRenderer& renderer, const ContentBand& band, const std::vector<RecentBook>& books,
                      const int listFocusIndex, const BookReadingStats* stats, const float progressPercent,
                      const GlobalReadingStats* globalStats) {
  using Mode = PenumbraThemeUi::UnderMode;
  PenumbraThemeUi::clampUnderModeToTracking();
  const Mode mode = PenumbraThemeUi::underMode();
  const char* title = lastReadBookTitle(books);
  switch (mode) {
    case Mode::Recents:
      drawRecentsListPanel(renderer, band, books, listFocusIndex);
      break;
    case Mode::BookStats:
      drawBookStatsPanel(renderer, band, title, stats, progressPercent, /*showBookTitle=*/true);
      break;
    case Mode::Lifetime:
      drawLifetimePanel(renderer, band, title, globalStats, /*showBookTitle=*/true, /*useStreak=*/true);
      break;
    case Mode::TitleAuthor:
    default:
      drawTitleAuthorPanel(renderer, band, books);
      break;
  }
  drawPenumbraPageDots(renderer, band, PenumbraThemeUi::x3PageIndex(mode), /*count=*/4);
}

// X4 lower half: Recents only (no stats tracking / page dots on X4).
void drawUnderPanelX4(const GfxRenderer& renderer, const ContentBand& band, const std::vector<RecentBook>& books,
                      const int listFocusIndex, const BookReadingStats* stats, const float progressPercent,
                      const GlobalReadingStats* globalStats) {
  (void)stats;
  (void)progressPercent;
  (void)globalStats;
  PenumbraThemeUi::clampUnderModeToTracking();
  drawRecentsListPanel(renderer, band, books, listFocusIndex);
  // No page-dots: single under-page (Recents).
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
  // pinBlocks: equal-gap upperTop. Else center clock in the upper half.
  const int groupTop = band.pinBlocks ? band.upperTop : (band.contentTop + std::max(0, (band.halfH - clockBlockH) / 2));
  drawHeroClockCentered(renderer, band.centerX, groupTop, timeBuf);
  if (dayBuf[0]) {
    const int dayW = renderer.getTextWidth(kDayFontId, dayBuf, EpdFontFamily::REGULAR);
    renderer.drawText(kDayFontId, band.centerX - dayW / 2, groupTop + clockInkH + kClockToDayGap, dayBuf, true,
                      EpdFontFamily::REGULAR);
  }

  renderer.drawLine(band.centerX - kRuleHalfWidth, band.midY, band.centerX + kRuleHalfWidth, band.midY, kRuleThickness,
                    true);
}

// X4: title/author of last-read book (index 0) + hairline at band.midY.
// pinBlocks: upperTop is the equal-gap start for Now Reading (not contentTop).
void drawTitleAuthorHalf(const GfxRenderer& renderer, const ContentBand& band, const std::vector<RecentBook>& books) {
  const int upperStart = band.pinBlocks ? band.upperTop : band.contentTop;
  drawTitleAuthorInZone(renderer, band, upperStart, band.midY, books);
  renderer.drawLine(band.centerX - kRuleHalfWidth, band.midY, band.centerX + kRuleHalfWidth, band.midY, kRuleThickness,
                    true);
}

void drawPenumbraHomeX4(const GfxRenderer& renderer, const std::vector<RecentBook>& recentBooks, const int listFocus,
                        const BookReadingStats* stats, const float progressPercent,
                        const GlobalReadingStats* globalStats) {
  ContentBand band = layoutContentBand(renderer);
  applyX4HairlineLayout(renderer, band, recentBooks);
  drawTitleAuthorHalf(renderer, band, recentBooks);
  drawUnderPanelX4(renderer, band, recentBooks, listFocus, stats, progressPercent, globalStats);
}

Rect redrawUnderPanelImpl(GfxRenderer& renderer, const std::vector<RecentBook>& recentBooks, const int selectorIndex,
                          const BookReadingStats* stats, const float progressPercent,
                          const GlobalReadingStats* globalStats) {
  ContentBand band = layoutContentBand(renderer);
  if (gpio.deviceIsX3()) {
    applyX3EqualSpacingLayout(renderer, band, recentBooks);
  } else {
    applyX4HairlineLayout(renderer, band, recentBooks);
  }
  // Full-width lower half. White-fill below the hairline, then redraw the rule so a
  // mode switch never leaves ghost ink or a missing hairline on e-ink.
  const int clearTop = band.midY;  // include the rule pixels
  const int clearH = std::max(0, band.contentBottom - clearTop);
  const Rect dirty{0, clearTop, renderer.getScreenWidth(), clearH};
  if (clearH > 0) {
    renderer.fillRect(0, clearTop, dirty.width, clearH, false);
  }
  renderer.drawLine(band.centerX - kRuleHalfWidth, band.midY, band.centerX + kRuleHalfWidth, band.midY, kRuleThickness,
                    true);
  if (gpio.deviceIsX3()) {
    drawUnderPanelX3(renderer, band, recentBooks, selectorIndex, stats, progressPercent, globalStats);
  } else {
    drawUnderPanelX4(renderer, band, recentBooks, selectorIndex, stats, progressPercent, globalStats);
  }
  return dirty;
}
}  // namespace

void PenumbraTheme::drawRecentBookCover(GfxRenderer& renderer, Rect /*rect*/,
                                        const std::vector<RecentBook>& recentBooks, const int selectorIndex,
                                        bool& coverRendered, bool& coverBufferStored, bool& bufferRestored,
                                        StoreCoverBufferFn /*storeCoverBuffer*/, const BookReadingStats* stats,
                                        float progressPercent, const GlobalReadingStats* globalStats,
                                        const char* /*currentChapterTitle*/) const {
  (void)bufferRestored;

  coverBufferStored = false;
  coverRendered = true;

  // selectorIndex = list focus for Recents under-panel only (not upper title).
  const int listFocus = selectorIndex;
  if (gpio.deviceIsX3()) {
    ContentBand band = layoutContentBand(renderer);
    applyX3EqualSpacingLayout(renderer, band, recentBooks);
    drawClockHalf(renderer, band);
    drawUnderPanelX3(renderer, band, recentBooks, listFocus, stats, progressPercent, globalStats);
  } else {
    // X4: equal-gap Now Reading / hairline / Recents.
    drawPenumbraHomeX4(renderer, recentBooks, listFocus, stats, progressPercent, globalStats);
  }
}

Rect PenumbraThemeUi::redrawUnderPanel(GfxRenderer& renderer, const std::vector<RecentBook>& recentBooks,
                                       const int selectorIndex, const BookReadingStats* stats,
                                       const float progressPercent, const GlobalReadingStats* globalStats) {
  return redrawUnderPanelImpl(renderer, recentBooks, selectorIndex, stats, progressPercent, globalStats);
}

void PenumbraThemeUi::warmRecentsProgressCache(const std::vector<RecentBook>& books) {
  const int n = std::min(static_cast<int>(books.size()), penumbraRecentsListCap());
  // Merge by path: reuse RAM % for known books; SD only for new paths.
  // Reader exit must call updateRecentsProgressForPath so the current book is fresh.
  ensureRecentsProgressCache(books, n, /*forceReload=*/false);
}

void PenumbraThemeUi::updateRecentsProgressForPath(const char* bookPath, const float progressPercent) {
  if (!bookPath || !bookPath[0]) return;
  for (int i = 0; i < g_recentsPctCache.n; ++i) {
    if (g_recentsPctCache.path[i] == bookPath) {
      g_recentsPctCache.pct[i] = progressPercent;
      LOG_DBG("HOME", "Recents progress cache update %.1f%% %s", static_cast<double>(progressPercent), bookPath);
      return;
    }
  }
  // Path not in cache yet (first open / empty cache) — no-op; warm will SD-load once.
}

void PenumbraThemeUi::invalidateRecentsProgressCache() {
  g_recentsPctCache.n = 0;
  for (int i = 0; i < kRecentsPctCacheMax; ++i) {
    g_recentsPctCache.path[i].clear();
    g_recentsPctCache.pct[i] = -1.0f;
  }
  LOG_DBG("HOME", "Recents progress cache invalidated");
}

bool PenumbraThemeUi::formatHeroTimeNow(char* buf, size_t bufSize) { return formatHeroTime(buf, bufSize); }

namespace {

// Full-width digit band for greys multipass (includes pad for AA fringe).
Rect clockDigitBandRect(const GfxRenderer& renderer) {
  ContentBand band = layoutContentBand(renderer);
  applyX3EqualSpacingLayout(renderer, band, /*books=*/{});
  const int clockInkH = renderer.getFontAscenderSize(kClockFontId);
  const int dayH = renderer.getLineHeight(kDayFontId);
  const int clockBlockH = clockInkH + kClockToDayGap + dayH;
  const int groupTop = band.pinBlocks ? band.upperTop : (band.contentTop + std::max(0, (band.halfH - clockBlockH) / 2));
  constexpr int kPadY = 4;
  const int top = std::max(0, groupTop - kPadY);
  const int h = clockInkH + kPadY * 2;
  return Rect{0, top, renderer.getScreenWidth(), h};
}

void paintHeroClockOnly(const GfxRenderer& renderer) {
  ContentBand band = layoutContentBand(renderer);
  applyX3EqualSpacingLayout(renderer, band, /*books=*/{});
  char timeBuf[16];
  formatHeroTime(timeBuf, sizeof(timeBuf));
  const int clockInkH = renderer.getFontAscenderSize(kClockFontId);
  const int dayH = renderer.getLineHeight(kDayFontId);
  const int clockBlockH = clockInkH + kClockToDayGap + dayH;
  const int groupTop = band.pinBlocks ? band.upperTop : (band.contentTop + std::max(0, (band.halfH - clockBlockH) / 2));
  drawHeroClockCentered(renderer, band.centerX, groupTop, timeBuf);
}

}  // namespace

bool PenumbraThemeUi::displayClockAntiAliased(GfxRenderer& renderer, const int baseRefreshMode,
                                              const Rect* dirtyOverride) {
  if (!gpio.deviceIsX3()) return false;
  if (!renderer.storeBwBuffer()) {
    LOG_DBG("HOME", "penumbra clock AA: storeBw failed — BW only");
    return false;
  }

  const Rect band = (dirtyOverride && dirtyOverride->width > 0 && dirtyOverride->height > 0)
                        ? *dirtyOverride
                        : clockDigitBandRect(renderer);
  // Expand dirty to full width so windowed greys stay 8-aligned and prefix
  // glyphs are not cut when only minutes change.
  const Rect grayRect{0, band.y, renderer.getScreenWidth(), band.height};

  const auto baseMode = (baseRefreshMode == static_cast<int>(HalDisplay::HALF_REFRESH)) ? HalDisplay::HALF_REFRESH
                                                                                        : HalDisplay::FAST_REFRESH;
  renderer.displayGrayscaleBase(baseMode);

  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
  paintHeroClockOnly(renderer);
  renderer.copyGrayscaleLsbBuffers();

  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
  paintHeroClockOnly(renderer);
  renderer.copyGrayscaleMsbBuffers();

  renderer.displayGrayBufferWindow(grayRect.x, grayRect.y, grayRect.width, grayRect.height);
  UiGhostPolicy::noteGreyscaleOnPanel();
  renderer.setRenderMode(GfxRenderer::BW);
  renderer.restoreBwBuffer();
  renderer.cleanupGrayscaleWithFrameBuffer();
  LOG_DBG("HOME", "penumbra clock AA y=%d h=%d base=%s", grayRect.y, grayRect.height,
          baseMode == HalDisplay::HALF_REFRESH ? "HALF" : "FAST");
  return true;
}

Rect PenumbraThemeUi::redrawClockBlock(GfxRenderer& renderer, const char* prevTime, char* outTime, size_t outTimeSize) {
  ContentBand band = layoutContentBand(renderer);
  // X4 has no hero clock — upper half is title/author (redrawn with full paint).
  if (!gpio.deviceIsX3()) {
    if (outTime && outTimeSize > 0) outTime[0] = '\0';
    return Rect{0, 0, 0, 0};
  }
  // Match full-paint spacing so the minute tick does not jump the hairline.
  // Empty book list still reserves full Recents height (see measureRecentsBlockH).
  applyX3EqualSpacingLayout(renderer, band, /*books=*/{});
  char timeBuf[16];
  formatHeroTime(timeBuf, sizeof(timeBuf));
  if (outTime && outTimeSize > 0) {
    snprintf(outTime, outTimeSize, "%s", timeBuf);
  }

  const int clockInkH = renderer.getFontAscenderSize(kClockFontId);
  const int dayH = renderer.getLineHeight(kDayFontId);
  const int clockBlockH = clockInkH + kClockToDayGap + dayH;
  const int groupTop = band.pinBlocks ? band.upperTop : (band.contentTop + std::max(0, (band.halfH - clockBlockH) / 2));

  // Measure old/new clock string bounds (centered). Prefer a tight suffix clear
  // when only trailing digits change (e.g. 9:41 → 9:42) so most of the face stays put.
  const int newW = renderer.getTextWidth(kClockFontId, timeBuf, EpdFontFamily::REGULAR);
  const int newLeft = band.centerX - newW / 2;
  int clearLeft = newLeft;
  int clearRight = newLeft + newW;
  bool suffixOnly = false;

  if (prevTime && prevTime[0] != '\0') {
    const int oldW = renderer.getTextWidth(kClockFontId, prevTime, EpdFontFamily::REGULAR);
    const int oldLeft = band.centerX - oldW / 2;
    clearLeft = std::min(clearLeft, oldLeft);
    clearRight = std::max(clearRight, oldLeft + oldW);

    // Common prefix length in characters.
    size_t prefixChars = 0;
    while (prevTime[prefixChars] != '\0' && timeBuf[prefixChars] != '\0' &&
           prevTime[prefixChars] == timeBuf[prefixChars]) {
      ++prefixChars;
    }
    // Same string length and non-empty prefix → clear only the changing tail.
    // Hour width changes (9:59 → 10:00) fall through to the full union rect.
    if (prefixChars > 0 && strlen(prevTime) == strlen(timeBuf) && oldW == newW && oldLeft == newLeft) {
      char prefixBuf[16];
      const size_t n = std::min(prefixChars, sizeof(prefixBuf) - 1);
      memcpy(prefixBuf, timeBuf, n);
      prefixBuf[n] = '\0';
      const int prefixW = renderer.getTextWidth(kClockFontId, prefixBuf, EpdFontFamily::REGULAR);
      clearLeft = newLeft + prefixW;
      clearRight = newLeft + newW;
      suffixOnly = true;
    }
  }

  // Small pad so AA / glyph overhang does not leave speckles.
  constexpr int kPadX = 4;
  constexpr int kPadY = 2;
  clearLeft = std::max(0, clearLeft - kPadX);
  clearRight = std::min(renderer.getScreenWidth(), clearRight + kPadX);
  const int clearTop = std::max(0, groupTop - kPadY);
  const int clearH = clockInkH + kPadY * 2;
  const int clearW = std::max(1, clearRight - clearLeft);

  if (clearW > 0 && clearH > 0) {
    renderer.fillRect(clearLeft, clearTop, clearW, clearH, false);
  }
  // Always redraw the full time string so prefix glyphs stay sharp after a
  // suffix-only white fill (prefix is left intact; full draw is ink-on-ink there).
  (void)suffixOnly;
  drawHeroClockCentered(renderer, band.centerX, groupTop, timeBuf);

  return Rect{clearLeft, clearTop, clearW, clearH};
}
