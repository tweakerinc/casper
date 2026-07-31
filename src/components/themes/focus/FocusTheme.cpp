#include "FocusTheme.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
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
#include "activities/reader/ReadingStatsUtils.h"
#include "components/UITheme.h"
#include "components/themes/HomeCoverMetrics.h"
#include "components/icons/cover.h"
#include "fontIds.h"
#include "util/StringUtils.h"

namespace {

// Outer margins: screen→cover (left) and stats→screen (right) must match.
// Mid gap (cover→stats) may grow if the jacket is narrower than the max slot.
constexpr int kEdgeGap = 8;
constexpr int kCoverCornerRadius = 6;
// Air under battery/clock before the cover+stats box (box sits a bit lower than chrome).
constexpr int kUnderChromePad = 18;
// Floor if measuring fails; real width is measured from labels/values.
constexpr int kStatsColMinW = 72;
// Source Serif 4 for Focus title + book stats (same family as Bare meta text).
constexpr int kTitleFontId = SOURCESERIF4_18_FONT_ID;
constexpr int kAuthorFontId = SOURCESERIF4_14_FONT_ID;
constexpr int kStatsValueFont = SOURCESERIF4_14_FONT_ID;  // bold values
constexpr int kStatsLabelFont = SOURCESERIF4_12_FONT_ID;  // quiet labels
// Pull label up under the value so "2h 15m" + "Reading Time" read as one unit.
// Without this, full line-box advance leaves a void that makes the label look
// attached to the *next* value instead of its own.
constexpr int kStatsValueLabelPull = 4;
// Bare-matched title↔author gap (BareTheme.cpp kTitleAuthorGap — do not drift).
constexpr int kTitleAuthorGap = 3;
// Extra nudge below contentTop so the shared cover|stats box is not glued to chrome.
constexpr int kBoxTopPad = 16;
// Min gaps when centering title/author (or lifetime card) in the free band under the box.
constexpr int kMinGapCoverToText = 10;
constexpr int kMinGapTextToFooter = 22;
constexpr int kTitleMaxLines = 3;
constexpr int kAuthorMaxLines = 2;
// Lifetime block under the cover|stats box (toggled from title/author via side buttons).
constexpr int kLifeCellPadY = 2;

// Gen aspect helpers live in HomeCoverMetrics.

float pagesPerMinute(const uint32_t totalPagesTurned, const uint32_t totalReadingSeconds) {
  if (totalReadingSeconds <= 60) {
    return 0.0f;
  }
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

bool estimatedTimeLeft(const BookReadingStats& stats, const float progressPercent, uint32_t& seconds) {
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

const char* dayCountText(const uint16_t days) { return days == 1 ? tr(STR_STATS_DAY) : tr(STR_STATS_DAYS); }

// Prefer the page-based ETA cached when the reader last saved (same as Stats).
bool estimateFinishDateFromDailyPace(const BookReadingStats& stats, const ReadingStatsDateTime& today,
                                     const uint32_t estimatedReadingSeconds, ReadingStatsDate& outDate) {
  if (!stats.startDate.isValid() || estimatedReadingSeconds == 0) {
    return false;
  }
  const uint16_t daysReading = std::max<uint16_t>(1, readingSpanDaysElapsed(stats.startDate, today.date));
  if (stats.totalReadingSeconds < 60 || daysReading == 0) {
    return false;
  }
  const uint32_t dailyAvg = stats.totalReadingSeconds / daysReading;
  if (dailyAvg == 0) {
    return false;
  }
  const uint32_t daysNeeded = (estimatedReadingSeconds + dailyAvg - 1) / dailyAvg;
  ReadingStatsDateTime estimatedFinish = today;
  addSecondsToReadingStatsDateTime(estimatedFinish, daysNeeded * 24u * 3600u);
  outDate = estimatedFinish.date;
  return outDate.isValid();
}

int contentTopY(const GfxRenderer& /*renderer*/) {
  // Compact chrome bottom + extra air so the cover is not hard under the status bar.
  return FocusMetrics::values.topPadding + BaseTheme::kTopChromeBatteryY +
         std::max(FocusMetrics::values.batteryHeight + 8, FocusMetrics::values.statusBarVerticalMargin) +
         kUnderChromePad;
}

// Stats + Stats-Life share ONE on-disk thumb (statsFamilyHeroThumbHeight).
// Prefer the shared height only — never open a different-size thumb and scale it.
std::string coverPathForBook(const RecentBook& book, const int pageW, const int pageH) {
  auto firstExisting = [](std::initializer_list<std::string> candidates) -> std::string {
    for (const std::string& path : candidates) {
      if (!path.empty() && Storage.exists(path.c_str())) {
        return path;
      }
    }
    return {};
  };

  const int sharedH = HomeCoverMetrics::statsFamilyHeroThumbHeight(pageW, pageH);

  if (FsHelpers::hasEpubExtension(book.path)) {
    Epub epub(book.path, "/.crosspoint");
    const std::string found = firstExisting({epub.getThumbBmpPath(sharedH)});
    if (!found.empty()) return found;
  }

  return firstExisting({
      UITheme::getCoverThumbPath(book.coverBmpPath, sharedH),
      book.coverBmpPath.find("[HEIGHT]") == std::string::npos ? book.coverBmpPath : std::string{},
  });
}

// Gen target plate is 3:4, but real thumbs are contain-fit into that box — most
// jackets are ~2:3, so the on-disk/art rect is full height and narrower. Loading
// wireframe must match that art plate (not the full 3:4 slot), or it bleeds into
// the stats column.
Rect typicalJacketArtRect(const Rect& slot) {
  if (slot.width <= 0 || slot.height <= 0) {
    return slot;
  }
  // Contain-fit a 2:3 jacket into the gen slot; top-left (same as real art).
  const int hIfFullW = (slot.width * 3) / 2;
  const int wIfFullH = (slot.height * 2) / 3;
  int w = slot.width;
  int h = slot.height;
  if (hIfFullW <= slot.height) {
    w = slot.width;
    h = std::max(1, hIfFullW);
  } else {
    h = slot.height;
    w = std::max(1, wIfFullH);
  }
  return Rect{slot.x, slot.y, w, h};
}

void drawMissingCover(const GfxRenderer& renderer, const Rect& artPlate, const RecentBook& book) {
  renderer.fillRoundedRect(artPlate.x, artPlate.y, artPlate.width, artPlate.height, kCoverCornerRadius, Color::White);
  renderer.drawRoundedRect(artPlate.x, artPlate.y, artPlate.width, artPlate.height, 1, kCoverCornerRadius, true);
  constexpr int iconSize = 32;
  if (artPlate.width >= iconSize + 8 && artPlate.height >= iconSize + 8) {
    renderer.drawIcon(CoverIcon, artPlate.x + (artPlate.width - iconSize) / 2,
                      artPlate.y + artPlate.height / 2 - iconSize / 2, iconSize);
  }
  (void)book;
}

// Contain-fit full jacket (no crop); prefer 1:1 native blit. Returns art rect for snapshot.
Rect drawCoverImage(const GfxRenderer& renderer, const Rect& coverRect, const RecentBook& book) {
  const std::string coverBmpPath =
      coverPathForBook(book, renderer.getScreenWidth(), renderer.getScreenHeight());
  if (coverBmpPath.empty() || !Storage.exists(coverBmpPath.c_str())) {
    const Rect plate = typicalJacketArtRect(coverRect);
    drawMissingCover(renderer, plate, book);
    return plate;
  }

  HalFile file;
  if (!Storage.openFileForRead("HOME", coverBmpPath, file)) {
    const Rect plate = typicalJacketArtRect(coverRect);
    drawMissingCover(renderer, plate, book);
    return plate;
  }

  Bitmap bitmap(file);
  if (bitmap.parseHeaders() != BmpReaderError::Ok || bitmap.getWidth() <= 0 || bitmap.getHeight() <= 0) {
    file.close();
    const Rect plate = typicalJacketArtRect(coverRect);
    drawMissingCover(renderer, plate, book);
    return plate;
  }

  const int bw = bitmap.getWidth();
  const int bh = bitmap.getHeight();
  int drawnW = bw;
  int drawnH = bh;
  if (bw > coverRect.width || bh > coverRect.height) {
    const float widthScale = static_cast<float>(coverRect.width) / static_cast<float>(bw);
    const float heightScale = static_cast<float>(coverRect.height) / static_cast<float>(bh);
    const float scale = std::min(widthScale, heightScale);
    drawnW = std::max(1, static_cast<int>(std::floor(bw * scale)));
    drawnH = std::max(1, static_cast<int>(std::floor(bh * scale)));
  }
  // Top-left align: stats top == cover top; no vertical letterbox above the jacket.
  const Rect bitmapRect{coverRect.x, coverRect.y, drawnW, drawnH};
  const int artRadius = std::min(kCoverCornerRadius, std::min(bitmapRect.width, bitmapRect.height) / 4);
  renderer.fillRoundedRect(bitmapRect.x, bitmapRect.y, bitmapRect.width, bitmapRect.height, artRadius, Color::White);
  // 1:1 native blit when possible — scaling 2-bit Atkinson is what shows gridlines.
  if (drawnW == bw && drawnH == bh) {
    renderer.drawBitmap(bitmap, bitmapRect.x, bitmapRect.y, bw, bh);
  } else {
    renderer.drawBitmap(bitmap, bitmapRect.x, bitmapRect.y, drawnW, drawnH);
  }
  renderer.maskRoundedRectOutsideCorners(bitmapRect.x, bitmapRect.y, bitmapRect.width, bitmapRect.height, artRadius,
                                         Color::White);
  renderer.drawRoundedRect(bitmapRect.x, bitmapRect.y, bitmapRect.width, bitmapRect.height, 1, artRadius, true);
  file.close();
  return bitmapRect;
}

// One stats pair: value on top, label pulled up under it (grouped unit).
// drawText(y): y is the line-box top (baseline = y + ascender).
// labelY / pair height must stay in lockstep with statsPairHeight().
void drawRightStackLabel(const GfxRenderer& renderer, const int rightX, const int y, const char* value,
                         const char* label) {
  const int valueH = renderer.getLineHeight(kStatsValueFont);
  const int valueW = renderer.getTextWidth(kStatsValueFont, value, EpdFontFamily::BOLD);
  renderer.drawText(kStatsValueFont, rightX - valueW, y, value, true, EpdFontFamily::BOLD);
  const int labelY = y + std::max(1, valueH - kStatsValueLabelPull);
  const int labelW = renderer.getTextWidth(kStatsLabelFont, label);
  renderer.drawText(kStatsLabelFont, rightX - labelW, labelY, label, true);
}

// Content height of one value+label pair (must match drawRightStackLabel).
int statsPairHeight(const GfxRenderer& renderer) {
  const int valueH = renderer.getLineHeight(kStatsValueFont);
  const int labelH = renderer.getLineHeight(kStatsLabelFont);
  return std::max(1, valueH - kStatsValueLabelPull) + labelH;
}

// Tight column width so cover can grow; right-aligned text then sits close to the jacket.
// Draw centered wrapped lines; returns Y just past the last line (Bare-style).
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

// Size the cover plate to the shared Stats-family hero (1:1 blit).
// ALWAYS exact gen plate — never shrink after the fact (that scales dither → lines).
// Stats column absorbs leftover width; under-box (title vs lifetime) never changes size.
void sizeCoverFrame(const int /*maxW*/, const int pageW, const int pageH, int& coverW, int& coverH) {
  HomeCoverMetrics::statsFamilyHeroPlate(pageW, pageH, coverW, coverH);
}

// Compact lifetime block height (title + 2×3 value/label grid). No plate chrome —
// black ink only so native e-ink white shows through (painted white plates read grey).
int minLifetimeCardHeight(const GfxRenderer& renderer) {
  const int titleH = renderer.getLineHeight(kStatsValueFont);
  const int valueH = renderer.getLineHeight(kStatsValueFont);
  const int labelH = renderer.getLineHeight(kStatsLabelFont);
  // Match right-column pair spacing (value pulled up over label).
  const int rowH = std::max(1, valueH - kStatsValueLabelPull) + labelH + kLifeCellPadY * 2;
  constexpr int kTitleBodyGap = 6;
  return std::max(64, titleH + kTitleBodyGap + rowH * 2);
}

void drawLifeStatCell(const GfxRenderer& renderer, const int x, const int w, const int y, const int h,
                      const char* value, const char* label) {
  constexpr int kPadX = 2;
  const int textW = std::max(1, w - kPadX * 2);
  const int valueLineH = renderer.getLineHeight(kStatsValueFont);
  const int labelLineH = renderer.getLineHeight(kStatsLabelFont);
  const int pairH = std::max(1, valueLineH - kStatsValueLabelPull) + labelLineH;
  const int textY = y + kLifeCellPadY + std::max(0, (h - pairH - kLifeCellPadY * 2) / 2);
  const std::string visValue = renderer.truncatedText(kStatsValueFont, value, textW, EpdFontFamily::BOLD);
  const std::string visLabel = renderer.truncatedText(kStatsLabelFont, label, textW);
  const int valueW = renderer.getTextWidth(kStatsValueFont, visValue.c_str(), EpdFontFamily::BOLD);
  const int labelW = renderer.getTextWidth(kStatsLabelFont, visLabel.c_str());
  renderer.drawText(kStatsValueFont, x + (w - valueW) / 2, textY, visValue.c_str(), true, EpdFontFamily::BOLD);
  const int labelY = textY + std::max(1, valueLineH - kStatsValueLabelPull);
  renderer.drawText(kStatsLabelFont, x + (w - labelW) / 2, labelY, visLabel.c_str(), true);
}

// Lifetime Stats — under-cover band (replaces title/author when toggled).
// Ink only: no white fill, no rounded frame. Painted white plates on e-ink read as
// a grey "card" against native panel white after multipass / HALF windows.
void drawLifetimeCard(const GfxRenderer& renderer, const Rect& cardRect, const GlobalReadingStats* globalStats) {
  if (cardRect.width < 80 || cardRect.height < 48) return;

  const GlobalReadingStats empty{};
  const GlobalReadingStats& stats = globalStats != nullptr ? *globalStats : empty;

  constexpr int kTitlePadX = 8;
  constexpr int kBodyInsetX = 4;
  constexpr int kColCount = 3;
  constexpr int kRowCount = 2;
  constexpr int kTitleBodyGap = 6;

  const int titleFontH = renderer.getLineHeight(kStatsValueFont);
  const int titleH = titleFontH;
  const int titleTextY = cardRect.y;

  const int titleMaxW = std::max(1, cardRect.width - kTitlePadX * 2);
  const std::string titleVis =
      renderer.truncatedText(kStatsValueFont, tr(STR_STATS_ALL_TIME), titleMaxW, EpdFontFamily::BOLD);
  const int titleW = renderer.getTextWidth(kStatsValueFont, titleVis.c_str(), EpdFontFamily::BOLD);
  renderer.drawText(kStatsValueFont, cardRect.x + (cardRect.width - titleW) / 2, titleTextY, titleVis.c_str(), true,
                    EpdFontFamily::BOLD);

  const int bodyY = cardRect.y + titleH + kTitleBodyGap;
  const int bodyH = std::max(1, cardRect.height - titleH - kTitleBodyGap);
  const int gridW = std::max(kColCount, cardRect.width - kBodyInsetX * 2);
  const int baseColW = gridW / kColCount;
  const int colRem = gridW % kColCount;
  int colW[kColCount];
  int colX[kColCount];
  int x = cardRect.x + kBodyInsetX;
  for (int i = 0; i < kColCount; ++i) {
    colW[i] = baseColW + (i < colRem ? 1 : 0);
    colX[i] = x;
    x += colW[i];
  }
  const int baseRowH = bodyH / kRowCount;
  const int rowRem = bodyH % kRowCount;
  const int rowH0 = baseRowH + (rowRem > 0 ? 1 : 0);
  const int rowH1 = baseRowH + (rowRem > 1 ? 1 : 0);
  const int rowY0 = bodyY;
  const int rowY1 = bodyY + rowH0;

  char buf[40];
  auto cell = [&](const int col, const int rowY, const int rowH, const char* value, const char* label) {
    drawLifeStatCell(renderer, colX[col], colW[col], rowY, rowH, value, label);
  };

  // Same 6 cells on X3 + X4 (no device-specific substitute rows).
  snprintf(buf, sizeof(buf), "%lu", static_cast<unsigned long>(stats.totalSessions));
  cell(0, rowY0, rowH0, buf, tr(STR_STATS_SESSIONS_LBL));
  BookReadingStats::formatDuration(stats.totalReadingSeconds, buf, sizeof(buf));
  cell(1, rowY0, rowH0, buf, tr(STR_STATS_TIME_LBL));
  snprintf(buf, sizeof(buf), "%.1f", pagesPerMinute(stats.totalPagesTurned, stats.totalReadingSeconds));
  cell(2, rowY0, rowH0, buf, tr(STR_STATS_PAGES_PER_MIN));

  const uint32_t avgSecs = stats.totalSessions > 0 ? stats.totalReadingSeconds / stats.totalSessions : 0;
  BookReadingStats::formatDuration(avgSecs, buf, sizeof(buf));
  cell(0, rowY1, rowH1, buf, tr(STR_STATS_AVG_SESSION_LBL));
  if (stats.completedBooks > 0) {
    snprintf(buf, sizeof(buf), "%lu", static_cast<unsigned long>(stats.completedBooks));
  } else {
    snprintf(buf, sizeof(buf), "-");
  }
  cell(1, rowY1, rowH1, buf, tr(STR_STATS_COMPLETED_LBL));
  // Longest streak needs calendar continuity (RTC on X3); show "-" when empty/unavailable.
  const uint16_t longest = stats.displayLongestReadingStreak();
  if (longest > 0) {
    snprintf(buf, sizeof(buf), "%u %s", static_cast<unsigned>(longest), dayCountText(longest));
  } else {
    snprintf(buf, sizeof(buf), "-");
  }
  cell(2, rowY1, rowH1, buf, tr(STR_STATS_LONGEST_STREAK_LBL));
}

// Full book-stats column — same order on X3 and X4.
// Time, Time Left, Progress, Daily Avg, Pages/Min, Days (Started), Finish date.
// Calendar-dependent fields show "-" when RTC/date data is unavailable (typical X4).
// rightX = pixel just past the rightmost text (text is right-aligned to rightX).
void drawRightStats(const GfxRenderer& renderer, const Rect& statsCol, const int rightX,
                    const BookReadingStats* stats, const float progressPercent) {
  const BookReadingStats empty{};
  const BookReadingStats& bookStats = stats != nullptr ? *stats : empty;
  // Vertical span = jacket box; horizontal pin is rightX.
  const int boxTop = statsCol.y;
  const int boxH = statsCol.height;
  const int pairH = statsPairHeight(renderer);

  constexpr int kRowCount = 7;

  char value[48];
  char label[48];
  char startedDate[24];
  char finishDate[24];
  uint32_t estimatedSeconds = 0;
  const bool hasEstimate = estimatedTimeLeft(bookStats, progressPercent, estimatedSeconds);
  ReadingStatsDateTime today;
  const bool hasToday = getCurrentLocalReadingStatsDateTime(today);
  const ReadingStatsDate endDate = bookStats.isCompleted && bookStats.finishedDate.isValid()
                                       ? bookStats.finishedDate
                                       : (hasToday ? today.date : ReadingStatsDate{});
  const bool hasDaySpan = bookStats.startDate.isValid() && endDate.isValid();
  const uint16_t daysReading = hasDaySpan ? readingSpanDaysElapsed(bookStats.startDate, endDate) : 0;

  // Jacket edge alignment (cover + stats line up as one block):
  //   first value line-box top   == cover top
  //   last label line-box bottom == cover bottom
  // Free height is distributed *between* pairs only; value↔label stay tight
  // via kStatsValueLabelPull so labels don't read as the next stat's value.
  int rowIdx = 0;
  auto nextY = [&]() {
    const int i = rowIdx++;
    if (kRowCount <= 1) return boxTop;
    const int travel = std::max(0, boxH - pairH);
    // i=0 → boxTop; i=last → boxTop + travel  ⇒  last pair ends at boxTop + boxH
    return boxTop + (i * travel) / (kRowCount - 1);
  };

  // 1) Reading Time
  BookReadingStats::formatDuration(bookStats.totalReadingSeconds, value, sizeof(value));
  drawRightStackLabel(renderer, rightX, nextY(), value, tr(STR_STATS_TIME_LBL));

  // 2) Time Left
  if (hasEstimate && !bookStats.isCompleted) {
    formatCompactDuration(estimatedSeconds, value, sizeof(value));
  } else {
    snprintf(value, sizeof(value), "-");
  }
  drawRightStackLabel(renderer, rightX, nextY(), value, tr(STR_TIME_LEFT));

  // 3) Progress — same value/label pair style as Stats (no pill).
  if (progressPercent >= 0.0f) {
    snprintf(value, sizeof(value), "%d%%", static_cast<int>(progressPercent + 0.5f));
  } else {
    snprintf(value, sizeof(value), "-");
  }
  drawRightStackLabel(renderer, rightX, nextY(), value, tr(STR_STATS_PROGRESS_LBL));

  // 4) Daily Avg (needs valid day span from start/finish dates)
  if (hasDaySpan) {
    const uint16_t dailyAverageDays = std::max<uint16_t>(1, daysReading);
    BookReadingStats::formatDuration(bookStats.totalReadingSeconds / dailyAverageDays, value, sizeof(value));
  } else {
    snprintf(value, sizeof(value), "-");
  }
  drawRightStackLabel(renderer, rightX, nextY(), value, tr(STR_STATS_DAILY_AVG_LBL));

  // 5) Pages/Min
  snprintf(value, sizeof(value), "%.1f", pagesPerMinute(bookStats.totalPagesTurned, bookStats.totalReadingSeconds));
  drawRightStackLabel(renderer, rightX, nextY(), value, tr(STR_STATS_PAGES_PER_MIN));

  // 6) Days reading + Started date as label
  if (hasDaySpan) {
    snprintf(value, sizeof(value), "%u %s", static_cast<unsigned>(daysReading), dayCountText(daysReading));
  } else {
    snprintf(value, sizeof(value), "-");
  }
  formatReadingStatsShortDate(bookStats.startDate, startedDate, sizeof(startedDate));
  if (bookStats.startDate.isValid()) {
    snprintf(label, sizeof(label), "%s %s", tr(STR_STATS_STARTED), startedDate);
  } else {
    snprintf(label, sizeof(label), "%s", tr(STR_STATS_STARTED));
  }
  drawRightStackLabel(renderer, rightX, nextY(), value, label);

  // 7) Finish / Est. finish date
  ReadingStatsDate finishDisplayDate;
  if (bookStats.isCompleted) {
    finishDisplayDate = bookStats.finishedDate;
  } else if (hasToday && hasEstimate) {
    if (!estimateFinishDateFromDailyPace(bookStats, today, estimatedSeconds, finishDisplayDate)) {
      ReadingStatsDateTime estimatedFinish = today;
      addSecondsToReadingStatsDateTime(estimatedFinish, estimatedSeconds);
      finishDisplayDate = estimatedFinish.date;
    }
  }
  formatReadingStatsShortDate(finishDisplayDate, finishDate, sizeof(finishDate));
  if (!finishDisplayDate.isValid()) {
    snprintf(finishDate, sizeof(finishDate), "-");
  }
  drawRightStackLabel(renderer, rightX, nextY(), finishDate,
                      bookStats.isCompleted ? tr(STR_STATS_FINISHED_DATE) : tr(STR_STATS_EST_FINISH_DATE));
}

// Shared under-box paint (full home draw + side-button partial redraw).
void paintUnderBoxContent(const GfxRenderer& renderer, const int pageW, const int boxBottom, const int bandBottom,
                          const bool lifeMode, const char* title, const char* author, const int textMaxW,
                          const int titleAuthorH, const int lifeH, const GlobalReadingStats* globalStats) {
  const int underBoxH = lifeMode ? lifeH : titleAuthorH;
  const int freeBelow = std::max(underBoxH, bandBottom - boxBottom);
  int metaTop = boxBottom + (freeBelow - underBoxH) / 2;
  const int metaTopMin = boxBottom + kMinGapCoverToText;
  const int metaTopMax = bandBottom - kMinGapTextToFooter - underBoxH;
  if (metaTop < metaTopMin) metaTop = metaTopMin;
  if (metaTopMax >= metaTopMin && metaTop > metaTopMax) metaTop = metaTopMax;

  if (lifeMode) {
    const Rect lifeRect{kEdgeGap, metaTop, pageW - 2 * kEdgeGap, underBoxH};
    drawLifetimeCard(renderer, lifeRect, globalStats);
  } else if (title && *title) {
    int textY = metaTop;
    textY = drawCenteredWrapped(renderer, kTitleFontId, pageW / 2, textY, textMaxW, title, kTitleMaxLines,
                                EpdFontFamily::BOLD);
    if (author && *author) {
      textY += kTitleAuthorGap;
      drawCenteredWrapped(renderer, kAuthorFontId, pageW / 2, textY, textMaxW, author, kAuthorMaxLines,
                          EpdFontFamily::REGULAR);
    }
  }
}

}  // namespace

void FocusTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                     const int /*selectorIndex*/, bool& coverRendered, bool& coverBufferStored,
                                     bool& bufferRestored, StoreCoverBufferFn storeCoverBuffer,
                                     const BookReadingStats* stats, float progressPercent,
                                     const GlobalReadingStats* globalStats,
                                     const char* /*currentChapterTitle*/) const {
  (void)rect;

  const int pageW = renderer.getScreenWidth();
  const int pageH = renderer.getScreenHeight();
  const int footerH = FocusMetrics::values.buttonHintsHeight;
  // Bare packing: free band under chrome → above menu; cover high, meta under.
  const int bandTop = contentTopY(renderer);
  const int bandBottom = pageH - footerH;
  const int bandH = std::max(200, bandBottom - bandTop);
  // Side Left/Right on Home toggles title/author ↔ lifetime card.
  const bool lifeMode = FocusThemeUi::showLifeUnderBox();

  if (recentBooks.empty()) {
    const char* msg = tr(STR_NO_OPEN_BOOK);
    const int lw = renderer.getTextWidth(kStatsValueFont, msg);
    renderer.drawText(kStatsValueFont, (pageW - lw) / 2, bandTop + bandH / 2, msg, true);
    if (lifeMode) {
      const int lifeH = minLifetimeCardHeight(renderer);
      const Rect lifeRect{kEdgeGap, bandBottom - kMinGapTextToFooter - lifeH, pageW - 2 * kEdgeGap, lifeH};
      drawLifetimeCard(renderer, lifeRect, globalStats);
    }
    coverRendered = true;
    coverBufferStored = false;
    return;
  }

  const RecentBook& book = recentBooks[0];
  const char* title = book.title.empty() ? book.path.c_str() : book.title.c_str();
  // Calibre often stores "Last, First"; show natural "First Last" (same as Bare).
  const std::string authorDisplay =
      book.author.empty() ? std::string() : StringUtils::formatAuthorDisplayName(book.author);
  const char* author = authorDisplay.empty() ? nullptr : authorDisplay.c_str();

  // Under-box: lifetime card or title+author. Cover plate is fixed either way.
  const int textMaxW = std::max(40, pageW - kEdgeGap * 2);
  const int lifeH = minLifetimeCardHeight(renderer);
  const int titleH =
      measureWrappedHeight(renderer, kTitleFontId, textMaxW, title, kTitleMaxLines, EpdFontFamily::BOLD);
  const int authorH = author ? measureWrappedHeight(renderer, kAuthorFontId, textMaxW, author, kAuthorMaxLines,
                                                    EpdFontFamily::REGULAR)
                             : 0;
  const int titleAuthorH = titleH + (author ? (kTitleAuthorGap + authorH) : 0);

  // Horizontal: fixed shared plate (1:1 gen), stats take remaining right column.
  //   [kEdgeGap][cover=gen plate][≥kEdgeGap][stats][kEdgeGap]
  int coverW = 0;
  int coverH = 0;
  sizeCoverFrame(/*maxW unused*/ 0, pageW, pageH, coverW, coverH);

  const int coverX = kEdgeGap;
  const int coverY = bandTop + kBoxTopPad;
  const Rect coverRect{coverX, coverY, coverW, coverH};

  const int statsRightX = pageW - kEdgeGap;
  // Remaining width for stats after fixed plate + gaps (may be < measure — labels truncate).
  const int statsW = std::max(kStatsColMinW, statsRightX - (coverX + coverW) - kEdgeGap);

  Rect artRect = coverRect;
  if (!coverRendered || !bufferRestored) {
    artRect = drawCoverImage(renderer, coverRect, book);
    coverBufferStored = storeCoverBuffer(artRect.x, artRect.y, artRect.width, artRect.height);
    coverRendered = true;
  } else {
    artRect = coverRect;
  }

  const int statsLeftX = statsRightX - statsW;
  const Rect statsCol{statsLeftX, artRect.y, statsW, artRect.height};
  drawRightStats(renderer, statsCol, statsRightX, stats, progressPercent);

  // Under the cover|stats box: lifetime card or title+author (side-button toggle).
  paintUnderBoxContent(renderer, pageW, artRect.y + artRect.height, bandBottom, lifeMode, title, author, textMaxW,
                       titleAuthorH, lifeH, globalStats);
}

// Partial home update: only the free band under the cover|stats box.
// Cover plate height is fixed, so boxBottom is stable across title ↔ lifetime.
Rect FocusThemeUi::redrawUnderBox(GfxRenderer& renderer, const std::vector<RecentBook>& recentBooks,
                                  const GlobalReadingStats* globalStats) {
  const int pageW = renderer.getScreenWidth();
  const int pageH = renderer.getScreenHeight();
  const int footerH = FocusMetrics::values.buttonHintsHeight;
  const int bandTop = contentTopY(renderer);
  const int bandBottom = pageH - footerH;
  const bool lifeMode = FocusThemeUi::showLifeUnderBox();

  int coverW = 0;
  int coverH = 0;
  sizeCoverFrame(/*maxW unused*/ 0, pageW, pageH, coverW, coverH);
  const int boxBottom = bandTop + kBoxTopPad + coverH;

  // Wipe free band through the bottom of the content area so paper is pure white
  // (content height differs between title/lifetime; cannot only erase the old rect).
  const int clearTop = boxBottom + kMinGapCoverToText;
  const int clearH = std::max(0, bandBottom - clearTop);
  const Rect dirty{0, clearTop, pageW, clearH};
  if (clearH > 0) {
    // Explicit pure white (false = white ink off) so windowed FAST matches Spectral paper.
    renderer.fillRect(0, clearTop, pageW, clearH, false);
  }

  if (recentBooks.empty()) {
    if (lifeMode) {
      const int lifeH = minLifetimeCardHeight(renderer);
      const Rect lifeRect{kEdgeGap, bandBottom - kMinGapTextToFooter - lifeH, pageW - 2 * kEdgeGap, lifeH};
      drawLifetimeCard(renderer, lifeRect, globalStats);
    }
    return dirty;
  }

  const RecentBook& book = recentBooks[0];
  const char* title = book.title.empty() ? book.path.c_str() : book.title.c_str();
  const std::string authorDisplay =
      book.author.empty() ? std::string() : StringUtils::formatAuthorDisplayName(book.author);
  const char* author = authorDisplay.empty() ? nullptr : authorDisplay.c_str();

  const int textMaxW = std::max(40, pageW - kEdgeGap * 2);
  const int lifeH = minLifetimeCardHeight(renderer);
  const int titleH =
      measureWrappedHeight(renderer, kTitleFontId, textMaxW, title, kTitleMaxLines, EpdFontFamily::BOLD);
  const int authorH = author ? measureWrappedHeight(renderer, kAuthorFontId, textMaxW, author, kAuthorMaxLines,
                                                    EpdFontFamily::REGULAR)
                             : 0;
  const int titleAuthorH = titleH + (author ? (kTitleAuthorGap + authorH) : 0);

  paintUnderBoxContent(renderer, pageW, boxBottom, bandBottom, lifeMode, title, author, textMaxW, titleAuthorH, lifeH,
                       globalStats);

  return dirty;
}

