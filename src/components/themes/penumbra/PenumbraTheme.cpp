#include "PenumbraTheme.h"

#include <EpdFontFamily.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalGPIO.h>
#include <I18n.h>

#include <algorithm>
#include <cmath>
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
// All Penumbra home text is Source Serif 4.
// X3 under-panel title stack: 12 / 18 / 14.
// X4 upper "Now Reading": caption 12 / title 14 / author 12 (list fonts only on Recents).
constexpr int kLabelFontId = SOURCESERIF4_12_FONT_ID;
constexpr int kLabelFontIdX4 = SOURCESERIF4_12_FONT_ID;
constexpr int kTitleFontId = SOURCESERIF4_18_FONT_ID;
constexpr int kAuthorFontId = SOURCESERIF4_14_FONT_ID;
// X4 Now Reading title: slightly larger than list titles (was 14).
constexpr int kTitleFontIdX4 = SOURCESERIF4_16_FONT_ID;
constexpr int kAuthorFontIdX4 = SOURCESERIF4_12_FONT_ID;
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
// Title→author gap after the *last* title line (same for 1- or 2-line titles).
constexpr int kPairGap = 10;
constexpr int kPairGapX4 = 5;
// Clock ink bottom → weekday — more air than title/author (was crowded).
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
// X4: grid slightly higher, caption slightly lower (more air under the grid).
constexpr int kStatsStackGapX4Top = 8;
constexpr int kStatsStackGapX4Footer = 22;
// X4 upper half: modest pad under chrome; internal gaps fill the rest of the half.
constexpr int kX4TitleTopPad = 12;
// Extra air below mid-hairline before the RECENTS caption (non-pinned layouts).
constexpr int kRecentsTopInset = 28;
constexpr int kRecentsTopInsetX4 = 12;
// Air between "RECENTS" caption and the first book row.
constexpr int kRecentsCaptionToListGap = 22;
constexpr int kRecentsCaptionToListGapX4 = 12;
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
  uint8_t rtcWeekday = 0;
  if (!halClock.isAvailable() ||
      !halClock.getDateTime(year, month, day, hour, minute, &rtcWeekday)) {
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
  // X4: pin upper/lower blocks so the hairline is centered between author and RECENTS.
  int upperTop = 0;
  int lowerTop = 0;
  bool pinBlocks = false;
};

ContentBand layoutContentBand(const GfxRenderer& renderer) {
  ContentBand b;
  const int pageW = renderer.getScreenWidth();
  const int pageH = renderer.getScreenHeight();
  const auto& metrics = PenumbraMetrics::values;
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
// Always REGULAR Source Serif — never bold. fontId defaults to 12pt; X4 hero uses 14pt.
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

  const int titleH =
      measureWrappedHeight(renderer, titleFont, textMaxW, title, titleMaxLines, EpdFontFamily::BOLD);
  const int authorH =
      author ? measureWrappedHeight(renderer, authorFont, textMaxW, author, kAuthorMaxLines, EpdFontFamily::REGULAR)
             : 0;
  // Plain gap after the wrapped title block (not ink-pull) so 2-line titles don't
  // crowd the author.
  const int authorGap = x4 ? kPairGapX4 : kClockToDayGap;
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
  const int authorH =
      authorDisplay.empty()
          ? 0
          : measureWrappedHeight(renderer, authorFont, band.textMaxW, authorDisplay.c_str(), kAuthorMaxLines,
                                 EpdFontFamily::REGULAR);
  return labelH + kLabelToTitleGapX4 + titleH +
         (authorH > 0 ? (kPairGapX4 + authorH) : 0);
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
inline int penumbraRecentsListCap() {
  return isX4Penumbra() ? 5 : 4;
}

int measureRecentsBlockH(const GfxRenderer& renderer, const ContentBand& band,
                         const std::vector<RecentBook>& books, const bool includeViewAll = false) {
  (void)band;
  const bool x4 = isX4Penumbra();
  const int captionH = renderer.getLineHeight(kLabelFontId);
  const int titleLineH = renderer.getLineHeight(kRecentsTitleFontId);
  const int rowH = recentsRowHeight(renderer);
  const int rowGap = x4 ? kRecentsRowGapX4 : kRecentsRowGap;
  const int capToList = x4 ? kRecentsCaptionToListGapX4 : kRecentsCaptionToListGap;
  const int viewAllGap = kRecentsViewAllGap;
  const int maxN = penumbraRecentsListCap();
  // Empty books: still reserve full list height so clock-minute re-layout does not jump midY.
  const int n =
      books.empty() ? maxN : std::min(static_cast<int>(books.size()), maxN);
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
  const int authorH =
      authorDisplay.empty()
          ? 0
          : measureWrappedHeight(renderer, kAuthorFontId, band.textMaxW, authorDisplay.c_str(), kAuthorMaxLines,
                                 EpdFontFamily::REGULAR);
  // drawTitleAuthorInZone uses kClockToDayGap for the non-X4 author gap.
  return labelH + kLabelToTitleGap + titleH + (authorH > 0 ? (kClockToDayGap + authorH) : 0);
}

int measureX3StatsStyleBlockH(const GfxRenderer& renderer) {
  const int captionH = renderer.getLineHeight(kLabelFontId);
  const int pairH = statPairHeight(renderer);
  const int gridH = pairH * 2 + kStatRowGap;
  // X3 stats: book title + grid + footer caption. Reserve generous title/footer air so
  // equal-gap layout (G ≈ hairline→title) still fits when stats is the tallest page.
  const int titleH = renderer.getLineHeight(kStatsBookTitleFontId);
  constexpr int kReserveAir = 40;
  return titleH + kReserveAir + gridH + kReserveAir + kReserveAir / 2 + captionH;
}

// Stable lower-block height for X3 equal-gap layout. MUST be mode-independent:
// partial under-panel swaps (side L/R) only white-fill below the hairline. If midY
// jumps when switching Title↔Recents↔Stats, the clear band misses old ink and the
// new panel paints on top of the previous one (overlap / "Stats box over Recents").
// Only one under-panel is drawn per frame — no layered modes — but e-ink keeps
// uncleared pixels until they are filled white.
int measureX3LowerBlockH(const GfxRenderer& renderer, const ContentBand& band,
                         const std::vector<RecentBook>& books) {
  const int titleH = measureX3TitleAuthorBlockH(renderer, band, books);
  const int recentsH = measureRecentsBlockH(renderer, band, books, /*includeViewAll=*/true);
  const int statsH = measureX3StatsStyleBlockH(renderer);
  return std::max({titleH, recentsH, statsH});
}

// X3: three equal air gaps — top→clock, date→hairline, hairline→lower panel —
// so the hairline is not glued to Recents. Leaves dots strip free below the list
// (View All sits in the Recents block). Hairline Y is fixed across under-panel modes.
void applyX3EqualSpacingLayout(const GfxRenderer& renderer, ContentBand& band,
                               const std::vector<RecentBook>& books) {
  PenumbraThemeUi::clampUnderModeToTracking();
  const int clockH = measureClockBlockH(renderer);
  const int Lh = measureX3LowerBlockH(renderer, band, books);
  const int dots = penumbraPageDotsStripH();
  const int floorY = band.contentBottom - dots;
  int free = floorY - band.contentTop - clockH - Lh - kRuleThickness;
  if (free < 6) free = 6;
  const int G = free / 3;

  band.upperTop = band.contentTop + G;           // clock group top
  band.midY = band.upperTop + clockH + G;        // hairline (stable across modes)
  band.halfH = std::max(1, band.midY - band.contentTop);
  band.lowerTop = band.midY + kRuleThickness + G;  // lower caption / list start
  band.pinBlocks = true;
}

// X4 equal vertical rhythm (four matching air gaps G):
//   status bar → NOW READING
//   author     → hairline
//   hairline   → RECENTS
//   last book  → menu
// List is up to 5 books; no View All (sides scroll; mid button opens full Recents).
void applyX4HairlineLayout(const GfxRenderer& renderer, ContentBand& band, const std::vector<RecentBook>& books) {
  PenumbraThemeUi::clampUnderModeToTracking();  // X4 → Recents only

  const int Uh = measureNowReadingBlockH(renderer, band, books);
  const int Lh = measureRecentsBlockH(renderer, band, books, /*includeViewAll=*/false);
  const int contentH = std::max(1, band.contentBottom - band.contentTop);
  int free = contentH - Uh - Lh - kRuleThickness;
  if (free < 0) free = 0;
  const int G = free / 4;

  band.upperTop = band.contentTop + G;
  band.midY = band.upperTop + Uh + G;
  band.halfH = std::max(1, band.midY - band.contentTop);
  band.lowerTop = band.midY + kRuleThickness + G;
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
void drawPenumbraPageDots(const GfxRenderer& renderer, const ContentBand& band, const int pageIndex,
                          const int count) {
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

// Progress % cache — avoid loadForBook (SD) on every Down scroll (X3 SPI is slow).
// Sized for X4's 5-row list (X3 uses ≤4 + View All).
constexpr int kRecentsPctCacheMax = 5;
struct RecentsPctCache {
  std::string path[kRecentsPctCacheMax];
  float pct[kRecentsPctCacheMax];
  int n = 0;
};
RecentsPctCache g_recentsPctCache;

void ensureRecentsProgressCache(const std::vector<RecentBook>& books, const int n) {
  const int count = std::clamp(n, 0, kRecentsPctCacheMax);
  bool hit = (g_recentsPctCache.n == count);
  for (int i = 0; hit && i < count; ++i) {
    if (g_recentsPctCache.path[i] != books[static_cast<size_t>(i)].path) hit = false;
  }
  if (hit) return;
  g_recentsPctCache.n = count;
  for (int i = 0; i < count; ++i) {
    g_recentsPctCache.path[i] = books[static_cast<size_t>(i)].path;
    g_recentsPctCache.pct[i] = BookReadingStats::loadForBook(g_recentsPctCache.path[i]).getProgressPercent();
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
void drawRecentsListPanel(const GfxRenderer& renderer, const ContentBand& band,
                          const std::vector<RecentBook>& books, const int listFocusIndex) {
  const bool x4 = isX4Penumbra();
  // View All only on X3 (full list is a front Recents button on X4).
  const bool showViewAll = !x4;
  const int zoneTop = band.midY + kRuleThickness;
  const int zoneBottom = band.contentBottom - penumbraPageDotsStripH();
  const int zoneH = std::max(1, zoneBottom - zoneTop);
  const int centerX = band.centerX;
  const int listLeft = centerX - band.textMaxW / 2;
  const int listW = band.textMaxW;

  const int captionH = renderer.getLineHeight(kLabelFontId);
  const int titleFont = kRecentsTitleFontId;
  const int authorFont = kRecentsAuthorFontId;
  const int titleLineH = renderer.getLineHeight(titleFont);
  const int authorInkH = renderer.getFontAscenderSize(authorFont);
  constexpr int kMicroBarH = 5;
  constexpr int kAuthorToBarGap = 4;
  const int kRowGap = x4 ? kRecentsRowGapX4 : kRecentsRowGap;
  const int capToList = x4 ? kRecentsCaptionToListGapX4 : kRecentsCaptionToListGap;
  const int viewAllGap = kRecentsViewAllGap;
  const int rowH = titleLineH + authorInkH + kAuthorToBarGap + kMicroBarH;

  const int topInset = x4 ? kRecentsTopInsetX4 : kRecentsTopInset;
  const int viewAllH = showViewAll ? (viewAllGap + titleLineH) : 0;

  const int capped = std::min(static_cast<int>(books.size()), penumbraRecentsListCap());
  int n = capped;
  if (!x4 && !band.pinBlocks) {
    const int spaceForRows =
        std::max(0, zoneH - topInset - captionH - capToList - viewAllH - 2);
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

  drawSectionLabel(renderer, centerX, y, tr(STR_RECENTS));
  y += captionH + capToList;

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

// Layout:
//   X3: book title — grid — STATS/LIFETIME footer caption.
//       Title→grid air matches hairline→title (equal-gap G). Footer sits lower to fill residual space.
//   X4: same shell as Recents — section caption on top, grid below (no book title;
//       last-read lives in the upper Now Reading panel). Unified vertical placement.
void drawStatsStylePanel(const GfxRenderer& renderer, const ContentBand& band, const char* bookTitle,
                         const char* sectionCaption, const char* const* values, const char* const* labels,
                         const bool showBookTitle = true) {
  const bool x4 = isX4Penumbra();
  const int rowGap = x4 ? kStatRowGapX4 : kStatRowGap;
  const int titleH =
      showBookTitle ? measureWrappedHeight(renderer, kStatsBookTitleFontId, band.textMaxW, bookTitle,
                                           kStatsTitleMaxLines, EpdFontFamily::BOLD)
                    : 0;
  const int captionH = renderer.getLineHeight(kLabelFontId);
  const int pairH = statPairHeight(renderer);
  const int gridH = pairH * 2 + rowGap;

  // Zone: mid rule → menu (minus page-dot strip when multi-page under-panel is on).
  const int zoneTop = band.pinBlocks ? band.lowerTop : (band.midY + kRuleThickness);
  const int zoneBottom = band.contentBottom - penumbraPageDotsStripH();
  const int zoneH = std::max(1, zoneBottom - zoneTop);

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

  // X3: hairline→title air is G (lowerTop − midY − rule). Match that for title→grid,
  // then drop the STATS/LIFETIME caption further to use leftover under-panel space.
  const int hairlineAir =
      band.pinBlocks ? std::max(kStatsStackGap, band.lowerTop - band.midY - kRuleThickness) : kStatsStackGap;
  const int titleGap = showBookTitle ? hairlineAir : 0;
  // Footer sits a bit lower than the title gap so the page doesn't look top-heavy.
  const int footerMinGap = hairlineAir + hairlineAir / 2;

  const int footerH = sectionCaption ? captionH : 0;
  const int blockH = titleH + titleGap + gridH + (footerH > 0 ? footerMinGap + footerH : 0);
  int y = band.pinBlocks ? zoneTop : (zoneTop + std::max(0, (zoneH - blockH) / 2));
  if (y < zoneTop + 2) y = zoneTop + 2;

  if (showBookTitle) {
    y = drawCenteredWrapped(renderer, kStatsBookTitleFontId, band.centerX, y, band.textMaxW, bookTitle,
                            kStatsTitleMaxLines, EpdFontFamily::BOLD);
    y += titleGap;
  }

  drawStatGrid(renderer, band.centerX, y, gridW, gridH, /*cols=*/3, /*rows=*/2, values, labels);
  y += gridH;

  if (sectionCaption) {
    // Prefer near the page-dot strip; never closer than footerMinGap under the grid.
    int footerY = zoneBottom - captionH - 2;
    if (footerY < y + footerMinGap) {
      footerY = y + footerMinGap;
    }
    if (footerY + captionH > zoneBottom) {
      footerY = std::max(y + kStatsStackGap, zoneBottom - captionH);
    }
    drawSectionLabel(renderer, band.centerX, footerY, sectionCaption);
  }
}

// Book stats — 3×2:
//   Progress | Reading Time | Time Left
//   Sessions | Avg. Session | Pages/Min
// showBookTitle: X3 under-panel yes.
void drawBookStatsPanel(const GfxRenderer& renderer, const ContentBand& band, const char* bookTitle,
                        const BookReadingStats* stats, const float progressPercent, const bool showBookTitle) {
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
  // Full labels on both devices; X4 uses a smaller label font (see kStatLabelFontIdX4).
  const char* labels[6] = {
      tr(STR_STATS_PROGRESS_LBL), tr(STR_STATS_TIME_LBL),        tr(STR_TIME_LEFT),
      tr(STR_STATS_SESSIONS_LBL), tr(STR_STATS_AVG_SESSION_LBL), tr(STR_STATS_PAGES_PER_MIN)};
  drawStatsStylePanel(renderer, band, bookTitle, tr(STR_STATS), values, labels, showBookTitle);
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
  const int groupTop =
      band.pinBlocks ? band.upperTop
                     : (band.contentTop + std::max(0, (band.halfH - clockBlockH) / 2));
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
  ensureRecentsProgressCache(books, n);
}

bool PenumbraThemeUi::formatHeroTimeNow(char* buf, size_t bufSize) { return formatHeroTime(buf, bufSize); }

Rect PenumbraThemeUi::redrawClockBlock(GfxRenderer& renderer, char* outTime, size_t outTimeSize) {
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

  // Full-width upper band through the hairline so windowed FAST white is even.
  const int pageW = renderer.getScreenWidth();
  const int clearTop = 0;
  const int clearH = std::max(1, band.midY + kRuleThickness - clearTop);
  renderer.fillRect(0, clearTop, pageW, clearH, false);
  drawClockHalf(renderer, band);

  return Rect{0, clearTop, pageW, clearH};
}
