#include "DashboardTheme.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <numeric>
#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "activities/reader/BookReadingStats.h"
#include "activities/reader/GlobalReadingStats.h"
#include "activities/reader/ReadingStatsUtils.h"
#include "components/UITheme.h"
#include "components/icons/afternoon.h"
#include "components/icons/book24.h"
#include "components/icons/cover.h"
#include "components/icons/evening.h"
#include "components/icons/morning.h"
#include "components/icons/night.h"
#include "components/icons/streak.h"
#include "fontIds.h"

namespace {
constexpr int kContentInsetX4 = 20;
constexpr int kContentInsetX3 = 75;
constexpr int kTopInset = 8;
constexpr int kCoverCornerRadius = 8;
constexpr int kStatsColumnWidth = 105;
constexpr int kStatsColumnWidthWide = 120;
constexpr int kCoverStatsGap = 12;
// Title sits above cover; cover/stats shift down by this band.
constexpr int kTitleTopPad = 2;
constexpr int kTitleCoverGap = 8;
constexpr int kCoverMetaGap = 12;  // streak / reader-type row under cover
constexpr int kFooterIconSize = 24;
constexpr int kFooterIconTextGap = 8;
// X3: Time, Time Left, Progress, Daily Avg, Pages/Min, Pages Turned, Started, Finish
// X4: Time, Time Left, Progress, Pages/Min, Pages Turned, Sessions, Avg Session
// Pages Turned stays in the book column (not the lifetime card — no room there).
constexpr int kStatsRowCount = 8;
constexpr int kStatsRowCountX4 = 7;
constexpr int kStatsValueLabelGap = 1;
// Spacing under meta row for lifetime stats card (keep clear of achievements).
constexpr int kLifetimeStatsMetaGap = 10;
constexpr int kLifetimeStatsBottomPad = 4;
// Meta band (day streak + reader type) under the cover.
constexpr int kMetaBandH = kFooterIconSize + 6;
// Lifetime card: roomy title band so "Lifetime Stats" is not tight, then two body rows.
constexpr int kLifetimeTitleH = 30;
constexpr int kLifetimeCellPadY = 4;
// Floor height for the lifetime card when reserving space under the cover.
constexpr int kLifetimeMinCardH = 110;

bool isWideScreen(const GfxRenderer& renderer) { return renderer.getScreenWidth() >= 560; }

int contentInset(const GfxRenderer& renderer) { return isWideScreen(renderer) ? kContentInsetX3 : kContentInsetX4; }

// Minimum lifetime card height for cover layout reserve (header + two value/label rows).
int minLifetimeCardHeight(const GfxRenderer& renderer) {
  const int valueH = renderer.getLineHeight(UI_12_FONT_ID);
  const int labelH = renderer.getLineHeight(SMALL_FONT_ID);
  const int rowH = valueH + 2 + labelH + kLifetimeCellPadY * 2;
  return std::max(kLifetimeMinCardH, kLifetimeTitleH + rowH * 2);
}

// Space reserved under the cover for meta row + lifetime card (no overlap).
int lowerDashboardReserve(const GfxRenderer& renderer) {
  return kCoverMetaGap + kMetaBandH + kLifetimeStatsMetaGap + minLifetimeCardHeight(renderer) +
         kLifetimeStatsBottomPad;
}

// Shared horizontal frame for title, cover, right stats, achievements, lifetime card.
// One left edge + one right edge across the whole dashboard.
struct ContentFrame {
  int left = 0;
  int width = 0;
  int right() const { return left + width; }  // exclusive right (x of first pixel past content)
};

ContentFrame contentFrame(const GfxRenderer& renderer) {
  const int inset = contentInset(renderer);
  ContentFrame f;
  f.left = inset;
  f.width = std::max(1, renderer.getScreenWidth() - inset * 2);
  return f;
}

// Largest bold face that still fits one line. Candidates respect OMIT_* font flags
// (e.g. tiny omits 18/20). Missing faces report width 0 and must be skipped.
// Prefer faces that have a real Bold cut; SMALL_FONT has only Regular.
int pickSingleLineTitleFont(const GfxRenderer& renderer, const char* title, const int maxWidth) {
  static constexpr int kCandidates[] = {
#ifndef OMIT_HUGE_FONT
      LEXENDDECA_20_FONT_ID,
#endif
#ifndef OMIT_XLARGE_FONT
      LEXENDDECA_18_FONT_ID,
#endif
#ifndef OMIT_LARGE_FONT
      LEXENDDECA_16_FONT_ID,
#endif
#ifndef OMIT_MEDIUM_FONT
      LEXENDDECA_14_FONT_ID,
#endif
#ifndef OMIT_SMALL_FONT
      LEXENDDECA_12_FONT_ID,
#endif
#ifndef OMIT_TINY_FONT
      LEXENDDECA_10_FONT_ID,
#endif
      UI_12_FONT_ID,
      UI_10_FONT_ID,
  };
  for (const int fontId : kCandidates) {
    const int w = renderer.getTextWidth(fontId, title, EpdFontFamily::BOLD);
    // Width 0 means empty text or unregistered font — never treat as "fits".
    if (w > 0 && w <= maxWidth) {
      return fontId;
    }
  }
  return UI_10_FONT_ID;
}

// Centered bold title within a content frame. Reading faces are 2-bit AA and look
// thin on pure-BW home; a 1px horizontal second pass adds stroke weight.
void drawCenteredBoldTitleInFrame(const GfxRenderer& renderer, const ContentFrame& frame, const int fontId,
                                  const int y, const char* text, const bool black) {
  constexpr auto kStyle = EpdFontFamily::BOLD;
  const int textW = renderer.getTextWidth(fontId, text, kStyle);
  // Leave 1px for the embolden pass so long titles do not clip the right edge.
  const int x = frame.left + std::max(0, (frame.width - textW - 1) / 2);
  renderer.drawText(fontId, x, y, text, black, kStyle);
  renderer.drawText(fontId, x + 1, y, text, black, kStyle);
}

// Draws a single-line, centered, bold book title at the top. Returns Y just below the title band.
// Title is constrained to the same horizontal frame as cover + lifetime card.
int drawTopBookTitle(const GfxRenderer& renderer, const Rect& contentRect, const RecentBook& book,
                     const bool black = true) {
  const ContentFrame frame = contentFrame(renderer);
  const int maxTextW = std::max(1, frame.width - 1);
  const char* rawTitle = book.title.empty() ? book.path.c_str() : book.title.c_str();
  const int fontId = pickSingleLineTitleFont(renderer, rawTitle, maxTextW);
  const std::string line = renderer.truncatedText(fontId, rawTitle, maxTextW, EpdFontFamily::BOLD);
  const int titleY = contentRect.y + kTitleTopPad;
  drawCenteredBoldTitleInFrame(renderer, frame, fontId, titleY, line.c_str(), black);
  return titleY + renderer.getLineHeight(fontId) + kTitleCoverGap;
}

// Cover sits on the shared left edge; width leaves room for the right stats column
// so the stats' right edge matches the lifetime card / title frame right edge.
// Height stops above meta + lifetime so those rows never overlap the cover stats.
//
// Aspect is locked to the generated home thumb ratio (homeCoverImage W:H, ~2:3).
// If layout only shrinks height, a non-matching frame makes 1-bit contain-scale
// shrink width and left-align the art — a large empty band on the right.
Rect coverRectForScreen(const GfxRenderer& renderer, const Rect& rect, const int coverTopY) {
  const ContentFrame frame = contentFrame(renderer);
  const int statsW = isWideScreen(renderer) ? kStatsColumnWidthWide : kStatsColumnWidth;
  const int maxCoverW = std::max(80, frame.width - statsW - kCoverStatsGap);
  const int maxCoverH = std::max(120, rect.y + rect.height - coverTopY - lowerDashboardReserve(renderer));

  constexpr int kThumbW = DashboardMetrics::homeCoverImageWidth;
  constexpr int kThumbH = DashboardMetrics::homeCoverImageHeight;

  int coverW = std::min(kThumbW, maxCoverW);
  int coverH = (coverW * kThumbH + kThumbW / 2) / kThumbW;
  if (coverH > maxCoverH) {
    coverH = maxCoverH;
    coverW = std::max(1, (coverH * kThumbW + kThumbH / 2) / kThumbH);
    if (coverW > maxCoverW) {
      coverW = maxCoverW;
      coverH = std::max(1, (coverW * kThumbH + kThumbW / 2) / kThumbW);
    }
  }
  return Rect{frame.left, coverTopY, coverW, coverH};
}

// Prefer full-bleed crop thumbs (no _fit letterboxing). Adaptive _fit files are a
// fallback only — they are often narrower than the frame and look like side bars.
std::string coverPathForRect(const RecentBook& book, const Rect& imageRect) {
  auto firstExisting = [](std::initializer_list<std::string> candidates) -> std::string {
    for (const std::string& path : candidates) {
      if (!path.empty() && Storage.exists(path.c_str())) {
        return path;
      }
    }
    return {};
  };

  if (FsHelpers::hasEpubExtension(book.path)) {
    Epub epub(book.path, "/.crosspoint");
    const std::string found = firstExisting({
        epub.getThumbBmpPath(imageRect.width, imageRect.height),
        epub.getThumbBmpPath(DashboardMetrics::homeCoverImageWidth, DashboardMetrics::homeCoverImageHeight),
        epub.getAdaptiveThumbBmpPath(imageRect.width, imageRect.height),
        epub.getAdaptiveThumbBmpPath(DashboardMetrics::homeCoverImageWidth, DashboardMetrics::homeCoverImageHeight),
    });
    if (!found.empty()) {
      return found;
    }
  }

  if (book.coverBmpPath.empty()) {
    return {};
  }

  return firstExisting({
      UITheme::getCoverThumbPath(book.coverBmpPath, imageRect.width, imageRect.height),
      UITheme::getCoverThumbPath(book.coverBmpPath, DashboardMetrics::homeCoverImageWidth,
                                 DashboardMetrics::homeCoverImageHeight),
      UITheme::getCoverThumbPath(book.coverBmpPath, imageRect.height),
  });
}

void drawMissingBookCover(const GfxRenderer& renderer, const Rect& coverRect, const RecentBook& book) {
  renderer.fillRoundedRect(coverRect.x, coverRect.y, coverRect.width, coverRect.height, kCoverCornerRadius,
                           Color::White);
  renderer.drawRoundedRect(coverRect.x, coverRect.y, coverRect.width, coverRect.height, 1, kCoverCornerRadius, true);

  constexpr int iconSize = 32;
  renderer.drawIcon(CoverIcon, coverRect.x + (coverRect.width - iconSize) / 2, coverRect.y + 36, iconSize, iconSize);

  constexpr int textPadding = 14;
  const int textW = coverRect.width - textPadding * 2;
  const char* title = book.title.empty() ? book.path.c_str() : book.title.c_str();
  auto titleLines = renderer.wrappedText(UI_12_FONT_ID, title, textW, 4, EpdFontFamily::BOLD);
  const int lineH = renderer.getLineHeight(UI_12_FONT_ID);
  int textY = coverRect.y + (coverRect.height - static_cast<int>(titleLines.size()) * lineH) / 2;
  for (const auto& line : titleLines) {
    const int lineW = renderer.getTextWidth(UI_12_FONT_ID, line.c_str(), EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, coverRect.x + (coverRect.width - lineW) / 2, textY, line.c_str(), true,
                      EpdFontFamily::BOLD);
    textY += lineH;
  }
}

void drawBookCover(const GfxRenderer& renderer, const Rect& coverRect, const RecentBook& book,
                   const Color backgroundColor) {
  bool hasCover = false;
  const std::string coverBmpPath = coverPathForRect(book, coverRect);
  if (!coverBmpPath.empty() && Storage.exists(coverBmpPath.c_str())) {
    FsFile file;
    if (Storage.openFileForRead("HOME", coverBmpPath, file)) {
      Bitmap bitmap(file);
      if (bitmap.parseHeaders() == BmpReaderError::Ok) {
        const int bw = bitmap.getWidth();
        const int bh = bitmap.getHeight();
        // Page underlay (shows through rounded corners after mask).
        renderer.fillRoundedRect(coverRect.x, coverRect.y, coverRect.width, coverRect.height, kCoverCornerRadius,
                                 backgroundColor);
        renderer.fillRoundedRect(coverRect.x, coverRect.y, coverRect.width, coverRect.height, kCoverCornerRadius,
                                 Color::White);

        // 1-bit path only downscales and draws top-left aligned. Center a
        // contain-fit so leftover band is equal L/R (or none when aspect matches).
        int drawX = coverRect.x;
        int drawY = coverRect.y;
        int drawW = coverRect.width;
        int drawH = coverRect.height;
        if (bw > 0 && bh > 0) {
          const float scale =
              std::min(static_cast<float>(coverRect.width) / static_cast<float>(bw),
                       static_cast<float>(coverRect.height) / static_cast<float>(bh));
          // drawBitmap1Bit will not upscale; when thumb is smaller, use native size centered.
          if (scale < 1.0f) {
            drawW = std::max(1, static_cast<int>(std::floor(static_cast<float>(bw) * scale)));
            drawH = std::max(1, static_cast<int>(std::floor(static_cast<float>(bh) * scale)));
          } else {
            drawW = bw;
            drawH = bh;
          }
          drawX = coverRect.x + (coverRect.width - drawW) / 2;
          drawY = coverRect.y + (coverRect.height - drawH) / 2;
        }
        renderer.drawBitmap(bitmap, drawX, drawY, drawW, drawH);

        renderer.maskRoundedRectOutsideCorners(coverRect.x, coverRect.y, coverRect.width, coverRect.height,
                                               kCoverCornerRadius, backgroundColor);
        renderer.drawRoundedRect(coverRect.x, coverRect.y, coverRect.width, coverRect.height, 1, kCoverCornerRadius,
                                 true);
        hasCover = true;
      }
      file.close();
    }
  }

  if (!hasCover) {
    drawMissingBookCover(renderer, coverRect, book);
  }
}

void drawRightAlignedText(const GfxRenderer& renderer, const int fontId, const int rightX, const int y,
                          const char* text, const bool bold = false, const bool black = true) {
  const EpdFontFamily::Style style = bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
  const int width = renderer.getTextWidth(fontId, text, style);
  renderer.drawText(fontId, rightX - width, y, text, black, style);
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

bool fallbackEstimatedTimeLeft(const BookReadingStats& stats, const float progressPercent, uint32_t& seconds) {
  seconds = 0;
  if (progressPercent <= 0.0f || progressPercent >= 100.0f || stats.totalReadingSeconds < 120) {
    return false;
  }
  const float progress = progressPercent / 100.0f;
  const float estimate = (static_cast<float>(stats.totalReadingSeconds) * (1.0f - progress)) / progress;
  if (estimate <= 0.0f) {
    return false;
  }
  seconds = static_cast<uint32_t>(estimate + 0.5f);
  return seconds > 0;
}

bool estimatedTimeLeft(const BookReadingStats& stats, const float progressPercent, uint32_t& seconds) {
  if (stats.estimatedTimeLeftSeconds > 0) {
    seconds = stats.estimatedTimeLeftSeconds;
    return true;
  }
  return fallbackEstimatedTimeLeft(stats, progressPercent, seconds);
}

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
  if (estimatedCalendarSeconds == 0) {
    return false;
  }

  ReadingStatsDateTime estimatedFinish = today;
  addSecondsToReadingStatsDateTime(estimatedFinish,
                                   static_cast<uint32_t>(std::min<uint64_t>(estimatedCalendarSeconds, UINT32_MAX)));
  outDate = estimatedFinish.date;
  return outDate.isValid();
}

float pagesPerMinute(const uint32_t totalPagesTurned, const uint32_t totalReadingSeconds) {
  if (totalReadingSeconds <= 60) {
    return 0.0f;
  }
  return static_cast<float>(totalPagesTurned) * 60.0f / static_cast<float>(totalReadingSeconds);
}

const char* dayCountText(const uint16_t days) { return days == 1 ? tr(STR_STATS_DAY) : tr(STR_STATS_DAYS); }

int statsBlockHeight(const GfxRenderer& renderer) {
  const int valueLineH = renderer.getLineHeight(UI_12_FONT_ID);
  const int labelLineH = renderer.getLineHeight(SMALL_FONT_ID);
  return valueLineH + kStatsValueLabelGap + labelLineH;
}

// Place book-column stat rows so they span the cover box exactly:
// - first row top == cover top pixel
// - last row bottom == cover bottom pixel (exclusive end = topY + spanH)
// - intermediate row tops are evenly spaced
int statsBlockTop(const int topY, const int spanH, const int index, const int blockH, const int rowCount) {
  if (rowCount <= 1) {
    return topY;
  }
  // Last row's top so its value+label block ends on the cover bottom edge.
  const int lastTop = topY + std::max(0, spanH - blockH);
  if (index <= 0) {
    return topY;
  }
  if (index >= rowCount - 1) {
    return lastTop;
  }
  // Even steps between first and last tops (at most 1px variance from integer math).
  return topY + (index * (lastTop - topY)) / (rowCount - 1);
}

void drawStatsRow(const GfxRenderer& renderer, const int rightX, const int y, const char* value, const char* label,
                  const bool black = true) {
  const int valueLineH = renderer.getLineHeight(UI_12_FONT_ID);
  drawRightAlignedText(renderer, UI_12_FONT_ID, rightX, y, value, true, black);
  drawRightAlignedText(renderer, SMALL_FONT_ID, rightX, y + valueLineH + kStatsValueLabelGap, label, false, black);
}

void drawDashboardStats(const GfxRenderer& renderer, const Rect& coverRect, const BookReadingStats* stats,
                        const float progressPercent, const bool black = true) {
  // Right-align to the content frame. Vertical span matches the cover layout rect
  // exactly (same box as drawBookCover's rounded frame).
  const int rightX = contentFrame(renderer).right() - 1;
  const int blockH = statsBlockHeight(renderer);
  // Use the cover box edges as the hard top/bottom of the stats column.
  const int spanH = std::max(blockH, coverRect.height);
  const bool showRtcStats = gpio.deviceIsX3();
  const int rowCount = showRtcStats ? kStatsRowCount : kStatsRowCountX4;
  const BookReadingStats emptyStats{};
  const BookReadingStats& bookStats = stats != nullptr ? *stats : emptyStats;
  char value[40];
  char label[40];
  char startedDate[24];
  char finishDate[24];
  uint32_t estimatedSeconds = 0;
  const bool hasEstimate = estimatedTimeLeft(bookStats, progressPercent, estimatedSeconds);
  ReadingStatsDateTime today;
  const bool hasToday = showRtcStats && getCurrentLocalReadingStatsDateTime(today);
  const ReadingStatsDate endDate = bookStats.isCompleted && bookStats.finishedDate.isValid()
                                       ? bookStats.finishedDate
                                       : (hasToday ? today.date : ReadingStatsDate{});
  const bool hasDaySpan = bookStats.startDate.isValid() && endDate.isValid();
  const uint16_t daysReading = hasDaySpan ? readingSpanDaysElapsed(bookStats.startDate, endDate) : 0;

  int rowIndex = 0;
  int rowY = statsBlockTop(coverRect.y, spanH, rowIndex, blockH, rowCount);
  BookReadingStats::formatDuration(bookStats.totalReadingSeconds, value, sizeof(value));
  drawStatsRow(renderer, rightX, rowY, value, tr(STR_STATS_TIME_LBL), black);

  rowY = statsBlockTop(coverRect.y, spanH, ++rowIndex, blockH, rowCount);
  if (hasEstimate && !bookStats.isCompleted) {
    formatCompactDuration(estimatedSeconds, value, sizeof(value));
  } else {
    snprintf(value, sizeof(value), "-");
  }
  drawStatsRow(renderer, rightX, rowY, value, tr(STR_TIME_LEFT), black);

  rowY = statsBlockTop(coverRect.y, spanH, ++rowIndex, blockH, rowCount);
  if (progressPercent >= 0.0f) {
    snprintf(value, sizeof(value), "%d%%", static_cast<int>(progressPercent + 0.5f));
  } else {
    snprintf(value, sizeof(value), "-");
  }
  drawStatsRow(renderer, rightX, rowY, value, tr(STR_STATS_PROGRESS_LBL), black);

  if (showRtcStats) {
    rowY = statsBlockTop(coverRect.y, spanH, ++rowIndex, blockH, rowCount);
    if (hasDaySpan) {
      const uint16_t dailyAverageDays = std::max<uint16_t>(1, daysReading);
      BookReadingStats::formatDuration(bookStats.totalReadingSeconds / dailyAverageDays, value, sizeof(value));
    } else {
      snprintf(value, sizeof(value), "-");
    }
    drawStatsRow(renderer, rightX, rowY, value, tr(STR_STATS_DAILY_AVG_LBL), black);
  }

  rowY = statsBlockTop(coverRect.y, spanH, ++rowIndex, blockH, rowCount);
  snprintf(value, sizeof(value), "%.1f", pagesPerMinute(bookStats.totalPagesTurned, bookStats.totalReadingSeconds));
  drawStatsRow(renderer, rightX, rowY, value, tr(STR_STATS_PAGES_PER_MIN), black);

  // Pages Turned sits under Pages/Min (book column, not the lifetime card).
  rowY = statsBlockTop(coverRect.y, spanH, ++rowIndex, blockH, rowCount);
  snprintf(value, sizeof(value), "%lu", static_cast<unsigned long>(bookStats.totalPagesTurned));
  drawStatsRow(renderer, rightX, rowY, value, tr(STR_STATS_PAGES_LBL), black);

  if (!showRtcStats) {
    rowY = statsBlockTop(coverRect.y, spanH, ++rowIndex, blockH, rowCount);
    snprintf(value, sizeof(value), "%u", static_cast<unsigned>(bookStats.sessionCount));
    drawStatsRow(renderer, rightX, rowY, value, tr(STR_STATS_SESSIONS_LBL), black);

    rowY = statsBlockTop(coverRect.y, spanH, ++rowIndex, blockH, rowCount);
    const uint32_t avgSeconds = bookStats.sessionCount > 0 ? bookStats.totalReadingSeconds / bookStats.sessionCount : 0;
    BookReadingStats::formatDuration(avgSeconds, value, sizeof(value));
    drawStatsRow(renderer, rightX, rowY, value, tr(STR_STATS_AVG_SESSION_LBL), black);
    return;
  }

  // Started / days reading — below Pages Turned.
  rowY = statsBlockTop(coverRect.y, spanH, ++rowIndex, blockH, rowCount);
  if (hasDaySpan) {
    snprintf(value, sizeof(value), "%u %s", static_cast<unsigned>(daysReading), dayCountText(daysReading));
  } else {
    snprintf(value, sizeof(value), "-");
  }
  formatReadingStatsShortDate(bookStats.startDate, startedDate, sizeof(startedDate));
  snprintf(label, sizeof(label), "%s %s", tr(STR_STATS_STARTED), startedDate);
  drawStatsRow(renderer, rightX, rowY, value, label, black);

  rowY = statsBlockTop(coverRect.y, spanH, ++rowIndex, blockH, rowCount);
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
  drawStatsRow(renderer, rightX, rowY, finishDate,
               bookStats.isCompleted ? tr(STR_STATS_FINISHED_DATE) : tr(STR_STATS_EST_FINISH_DATE), black);
}

bool dominantReaderTypeBucket(const GlobalReadingStats& globalStats, ReadingTimeBucket& bucketOut) {
  const auto& values = globalStats.timeOfDaySeconds;
  const uint32_t totalSeconds = std::accumulate(values.begin(), values.end(), 0u);
  if (totalSeconds == 0) {
    return false;
  }

  const size_t dominantIndex =
      static_cast<size_t>(std::distance(values.begin(), std::max_element(values.begin(), values.end())));
  bucketOut = static_cast<ReadingTimeBucket>(dominantIndex);
  return true;
}

const char* readerTypeLabel(const GlobalReadingStats* globalStats) {
  if (globalStats == nullptr) {
    return tr(STR_STATS_NEW_READER);
  }

  ReadingTimeBucket bucket = ReadingTimeBucket::Night;
  if (!dominantReaderTypeBucket(*globalStats, bucket)) {
    return tr(STR_STATS_NEW_READER);
  }

  switch (bucket) {
    case ReadingTimeBucket::Morning:
      return tr(STR_STATS_MORNING_READER);
    case ReadingTimeBucket::Afternoon:
      return tr(STR_STATS_AFTERNOON_READER);
    case ReadingTimeBucket::Evening:
      return tr(STR_STATS_EVENING_READER);
    case ReadingTimeBucket::Night:
    default:
      return tr(STR_STATS_NIGHT_READER);
  }
}

const uint8_t* readerTypeIcon(const GlobalReadingStats* globalStats) {
  if (globalStats == nullptr) {
    return Book24Icon;
  }

  ReadingTimeBucket bucket = ReadingTimeBucket::Night;
  if (!dominantReaderTypeBucket(*globalStats, bucket)) {
    return Book24Icon;
  }

  switch (bucket) {
    case ReadingTimeBucket::Morning:
      return MorningReaderIcon;
    case ReadingTimeBucket::Afternoon:
      return AfternoonReaderIcon;
    case ReadingTimeBucket::Evening:
      return EveningReaderIcon;
    case ReadingTimeBucket::Night:
    default:
      return NightReaderIcon;
  }
}

void formatStreakStat(const GlobalReadingStats* globalStats, char* buf, const size_t len) {
  if (len == 0) {
    return;
  }
  if (globalStats == nullptr) {
    snprintf(buf, len, "%s", tr(STR_STATS_NO_STREAK));
    return;
  }

  ReadingStatsDateTime today;
  const uint16_t streak =
      getCurrentLocalReadingStatsDateTime(today) ? globalStats->currentReadingStreak(&today.date) : 0;
  if (streak == 0) {
    snprintf(buf, len, "%s", tr(STR_STATS_NO_STREAK));
    return;
  }
  snprintf(buf, len, tr(STR_STATS_DAY_STREAK_FORMAT), static_cast<unsigned>(streak));
}

void drawIconLabel(const GfxRenderer& renderer, const uint8_t* icon, const int iconX, const int centerY,
                   const char* label, const int maxTextW, const bool inverted = false) {
  const std::string visibleLabel = renderer.truncatedText(UI_10_FONT_ID, label, maxTextW);
  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
  if (inverted) {
    renderer.drawIconInverted(icon, iconX, centerY - kFooterIconSize / 2, kFooterIconSize, kFooterIconSize);
  } else {
    renderer.drawIcon(icon, iconX, centerY - kFooterIconSize / 2, kFooterIconSize, kFooterIconSize);
  }
  renderer.drawText(UI_10_FONT_ID, iconX + kFooterIconSize + kFooterIconTextGap, centerY - lineH / 2,
                    visibleLabel.c_str(), !inverted);
}

void drawRightAlignedIconLabel(const GfxRenderer& renderer, const uint8_t* icon, const int rightX, const int centerY,
                               const char* label, const int maxTextW, const bool inverted = false) {
  const std::string visibleLabel = renderer.truncatedText(UI_10_FONT_ID, label, maxTextW);
  const int labelW = renderer.getTextWidth(UI_10_FONT_ID, visibleLabel.c_str());
  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
  const int textX = rightX - labelW;
  const int iconX = textX - kFooterIconTextGap - kFooterIconSize;
  if (inverted) {
    renderer.drawIconInverted(icon, iconX, centerY - kFooterIconSize / 2, kFooterIconSize, kFooterIconSize);
  } else {
    renderer.drawIcon(icon, iconX, centerY - kFooterIconSize / 2, kFooterIconSize, kFooterIconSize);
  }
  renderer.drawText(UI_10_FONT_ID, textX, centerY - lineH / 2, visibleLabel.c_str(), !inverted);
}

void drawLeftAnchoredFooterStat(const GfxRenderer& renderer, const int labelX, const int centerY, const int maxTextW,
                                const char* value, const char* label, const bool inverted = false) {
  const int valueLineH = renderer.getLineHeight(UI_12_FONT_ID);
  const int labelLineH = renderer.getLineHeight(UI_10_FONT_ID);
  const int totalH = valueLineH + kStatsValueLabelGap + labelLineH;
  const int valueW = renderer.getTextWidth(UI_12_FONT_ID, value, EpdFontFamily::BOLD);
  const std::string visibleLabel = renderer.truncatedText(UI_10_FONT_ID, label, maxTextW);
  const int labelW = renderer.getTextWidth(UI_10_FONT_ID, visibleLabel.c_str());
  const int topY = centerY - totalH / 2;
  renderer.drawText(UI_12_FONT_ID, labelX + (labelW - valueW) / 2, topY, value, !inverted, EpdFontFamily::BOLD);
  renderer.drawText(UI_10_FONT_ID, labelX, topY + valueLineH + kStatsValueLabelGap, visibleLabel.c_str(), !inverted);
}

void drawRightAnchoredFooterStat(const GfxRenderer& renderer, const int labelRightX, const int centerY,
                                 const int maxTextW, const char* value, const char* label,
                                 const bool inverted = false) {
  const int valueLineH = renderer.getLineHeight(UI_12_FONT_ID);
  const int labelLineH = renderer.getLineHeight(UI_10_FONT_ID);
  const int totalH = valueLineH + kStatsValueLabelGap + labelLineH;
  const int valueW = renderer.getTextWidth(UI_12_FONT_ID, value, EpdFontFamily::BOLD);
  const std::string visibleLabel = renderer.truncatedText(UI_10_FONT_ID, label, maxTextW);
  const int labelW = renderer.getTextWidth(UI_10_FONT_ID, visibleLabel.c_str());
  const int labelX = labelRightX - labelW;
  const int topY = centerY - totalH / 2;
  renderer.drawText(UI_12_FONT_ID, labelX + (labelW - valueW) / 2, topY, value, !inverted, EpdFontFamily::BOLD);
  renderer.drawText(UI_10_FONT_ID, labelX, topY + valueLineH + kStatsValueLabelGap, visibleLabel.c_str(), !inverted);
}

// Achievements under the cover: day streak + reader type evenly across the full
// content width (two equal halves; each group centered in its half).
// Returns Y just below this band for the lifetime card.
int drawMetaStatsUnderCover(const GfxRenderer& renderer, const Rect& coverRect, const GlobalReadingStats* globalStats,
                            const bool inverted = false) {
  const ContentFrame frame = contentFrame(renderer);
  const int bandH = kMetaBandH;
  const int topY = coverRect.y + coverRect.height + kCoverMetaGap;
  const int centerY = topY + bandH / 2;
  constexpr int kAchGap = 6;  // space between icon and text

  char streakBuf[48];
  formatStreakStat(globalStats, streakBuf, sizeof(streakBuf));
  const char* readerLabel = readerTypeLabel(globalStats);
  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
  const int textY = centerY - lineH / 2;
  const int iconY = centerY - kFooterIconSize / 2;

  // Full content frame, split into two equal columns so items are centered
  // left/right of screen center and neither is cramped into a thin third.
  const int half0 = frame.width / 2;
  const int half1 = frame.width - half0;
  const int col0X = frame.left;
  const int col1X = frame.left + half0;

  auto drawCenteredAchievement = [&](const int colX, const int colW, const uint8_t* icon, const char* label) {
    const int maxTextW = std::max(1, colW - kFooterIconSize - kAchGap - 8);
    const std::string visible = renderer.truncatedText(UI_10_FONT_ID, label, maxTextW);
    const int textW = renderer.getTextWidth(UI_10_FONT_ID, visible.c_str());
    const int groupW = kFooterIconSize + kAchGap + textW;
    const int groupX = colX + std::max(0, (colW - groupW) / 2);
    if (inverted) {
      renderer.drawIconInverted(icon, groupX, iconY, kFooterIconSize, kFooterIconSize);
    } else {
      renderer.drawIcon(icon, groupX, iconY, kFooterIconSize, kFooterIconSize);
    }
    renderer.drawText(UI_10_FONT_ID, groupX + kFooterIconSize + kAchGap, textY, visible.c_str(), !inverted);
  };

  drawCenteredAchievement(col0X, half0, StreakIcon, streakBuf);
  drawCenteredAchievement(col1X, half1, readerTypeIcon(globalStats), readerLabel);

  return topY + bandH + 1;
}

// Lifetime value+label cell: value and label each centered in the cell.
// Values use UI_10 (one step under the book-column UI_12). Long labels are
// truncated by draw path if a column is too narrow.
void drawDashboardLifetimeStatCell(const GfxRenderer& renderer, const int x, const int w, const int y, const int h,
                                 const char* value, const char* label, const bool black) {
  constexpr int kValueFont = UI_10_FONT_ID;
  constexpr int kLabelFont = SMALL_FONT_ID;
  constexpr int kPadX = 2;
  constexpr int kValueLabelGap = 1;
  const int textW = std::max(1, w - kPadX * 2);
  const int valueLineH = renderer.getLineHeight(kValueFont);
  const int labelLineH = renderer.getLineHeight(kLabelFont);
  const int totalTextH = valueLineH + kValueLabelGap + labelLineH;
  const int textY = y + std::max(0, (h - totalTextH) / 2);
  const std::string visValue = renderer.truncatedText(kValueFont, value, textW, EpdFontFamily::BOLD);
  const std::string visLabel = renderer.truncatedText(kLabelFont, label, textW);
  const int valueW = renderer.getTextWidth(kValueFont, visValue.c_str(), EpdFontFamily::BOLD);
  const int labelW = renderer.getTextWidth(kLabelFont, visLabel.c_str());
  renderer.drawText(kValueFont, x + (w - valueW) / 2, textY, visValue.c_str(), black, EpdFontFamily::BOLD);
  renderer.drawText(kLabelFont, x + (w - labelW) / 2, textY + valueLineH + kValueLabelGap, visLabel.c_str(), black);
}

// Lifetime stats card.
// Header: centered "Lifetime Stats".
// Body 2x4, four equal-width columns centered in the card.
// Labels: Reading Time / Pages Turned / Streak / Avg. Session.
// Streak and Days Read values include the unit ("11 Days") in the value font.
// Days Read = distinct calendar days with reading activity (not hours/24).
void drawLifetimeStatsCard(const GfxRenderer& renderer, const Rect& cardRect, const GlobalReadingStats* globalStats,
                         const bool black = true) {
  if (cardRect.width < 80 || cardRect.height < 60) return;

  const GlobalReadingStats empty{};
  const GlobalReadingStats& stats = globalStats != nullptr ? *globalStats : empty;

  constexpr int kTitlePadX = 8;
  constexpr int kBodyInsetX = 2;  // equal left/right inset so the grid sits centered
  constexpr int kColCount = 4;
  constexpr int kRowCount = 2;
  // Prefer a taller header; never steal more than half the card from the 2-row body.
  const int titleH = std::min(kLifetimeTitleH, std::max(24, cardRect.height / 3));
  const int titleTextY = cardRect.y + (titleH - renderer.getLineHeight(UI_10_FONT_ID)) / 2;

  // Card chrome
  renderer.drawRect(cardRect.x, cardRect.y, cardRect.width, cardRect.height, black);
  renderer.drawLine(cardRect.x, cardRect.y + titleH, cardRect.x + cardRect.width - 1, cardRect.y + titleH, black);

  // Header: single centered title.
  const int titleMaxW = std::max(1, cardRect.width - kTitlePadX * 2);
  const std::string titleVis =
      renderer.truncatedText(UI_10_FONT_ID, tr(STR_STATS_ALL_TIME), titleMaxW, EpdFontFamily::BOLD);
  const int titleW = renderer.getTextWidth(UI_10_FONT_ID, titleVis.c_str(), EpdFontFamily::BOLD);
  renderer.drawText(UI_10_FONT_ID, cardRect.x + (cardRect.width - titleW) / 2, titleTextY, titleVis.c_str(), black,
                    EpdFontFamily::BOLD);

  const int bodyY = cardRect.y + titleH;
  const int bodyH = cardRect.height - titleH;
  // Centered grid: equal side insets, then remainder pixels spread across columns
  // so no column is wider than the others by more than 1px.
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
  // Equal row heights; leftover body pixels split top/bottom for vertical centering.
  const int baseRowH = bodyH / kRowCount;
  const int rowRem = bodyH % kRowCount;
  const int rowH0 = baseRowH + (rowRem > 0 ? 1 : 0);
  const int rowH1 = baseRowH + (rowRem > 1 ? 1 : 0);
  // If bodyH % 2 == 1, first row got the extra pixel; grid still fills the body.
  const int rowY0 = bodyY;
  const int rowY1 = bodyY + rowH0;

  char buf[40];

  auto cell = [&](const int col, const int rowY, const int rowH, const char* value, const char* label) {
    drawDashboardLifetimeStatCell(renderer, colX[col], colW[col], rowY, rowH, value, label, black);
  };

  // Row 1
  snprintf(buf, sizeof(buf), "%lu", static_cast<unsigned long>(stats.totalSessions));
  cell(0, rowY0, rowH0, buf, tr(STR_STATS_SESSIONS_LBL));

  BookReadingStats::formatDuration(stats.totalReadingSeconds, buf, sizeof(buf));
  cell(1, rowY0, rowH0, buf, tr(STR_STATS_TIME_LBL));

  snprintf(buf, sizeof(buf), "%.1f", pagesPerMinute(stats.totalPagesTurned, stats.totalReadingSeconds));
  cell(2, rowY0, rowH0, buf, tr(STR_STATS_PAGES_PER_MIN));

  snprintf(buf, sizeof(buf), "%lu", static_cast<unsigned long>(stats.totalPagesTurned));
  cell(3, rowY0, rowH0, buf, tr(STR_STATS_PAGES_LBL));

  // Row 2
  const uint32_t avgSecs = stats.totalSessions > 0 ? stats.totalReadingSeconds / stats.totalSessions : 0;
  BookReadingStats::formatDuration(avgSecs, buf, sizeof(buf));
  cell(0, rowY1, rowH1, buf, tr(STR_STATS_AVG_SESSION_LBL));

  if (stats.completedBooks > 0) {
    snprintf(buf, sizeof(buf), "%lu", static_cast<unsigned long>(stats.completedBooks));
  } else {
    snprintf(buf, sizeof(buf), "-");
  }
  cell(1, rowY1, rowH1, buf, tr(STR_STATS_COMPLETED_LBL));

  const uint16_t longest = stats.displayLongestReadingStreak();
  if (longest > 0) {
    // Unit in the same bold value font as the number: "11 Days".
    snprintf(buf, sizeof(buf), "%u %s", static_cast<unsigned>(longest), dayCountText(longest));
  } else {
    snprintf(buf, sizeof(buf), "-");
  }
  cell(2, rowY1, rowH1, buf, tr(STR_STATS_LONGEST_STREAK_LBL));

  const uint16_t daysRead = computeReadingHistoryDaysRead(stats.readingHistoryBits);
  if (daysRead > 0) {
    snprintf(buf, sizeof(buf), "%u %s", static_cast<unsigned>(daysRead), dayCountText(daysRead));
  } else {
    snprintf(buf, sizeof(buf), "-");
  }
  cell(3, rowY1, rowH1, buf, tr(STR_STATS_DAYS_READ_LBL));
}
void drawLifetimeStatsUnderMeta(const GfxRenderer& renderer, const Rect& contentRect, const int metaBottomY,
                              const GlobalReadingStats* globalStats, const bool black = true) {
  const ContentFrame frame = contentFrame(renderer);
  const int availableBottom = contentRect.y + contentRect.height - kLifetimeStatsBottomPad;
  // Sit clearly below the achievements row and use the remaining tile height
  // so the 2x4 grid is readable (not crushed against meta).
  const int cardTop = metaBottomY + kLifetimeStatsMetaGap;
  if (availableBottom - cardTop < 60) return;

  const Rect cardRect{frame.left, cardTop, frame.width, availableBottom - cardTop};
  drawLifetimeStatsCard(renderer, cardRect, globalStats, black);
}
}  // namespace

void DashboardTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                         int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                         bool& bufferRestored, const std::function<bool()>& storeCoverBuffer,
                                         const BookReadingStats* stats, const float progressPercent,
                                         const GlobalReadingStats* globalStats, const char* currentChapterTitle) const {
  (void)selectorIndex;
  (void)bufferRestored;
  (void)currentChapterTitle;  // Progress % is shown in the stats column; chapter title omitted.

  if (recentBooks.empty()) {
    const Rect coverRect = coverRectForScreen(renderer, rect, rect.y + kTopInset);
    renderer.drawRoundedRect(coverRect.x, coverRect.y, coverRect.width, coverRect.height, 1, kCoverCornerRadius, true);
    coverRendered = false;
    coverBufferStored = false;
    return;
  }

  const int coverTopY = drawTopBookTitle(renderer, rect, recentBooks[0], /*black=*/true);
  const Rect coverRect = coverRectForScreen(renderer, rect, coverTopY);

  if (!coverRendered) {
    drawBookCover(renderer, coverRect, recentBooks[0], Color::White);
    coverBufferStored = storeCoverBuffer();
    coverRendered = coverBufferStored;
  }

  drawDashboardStats(renderer, coverRect, stats, progressPercent);
  const int metaBottomY = drawMetaStatsUnderCover(renderer, coverRect, globalStats);
  drawLifetimeStatsUnderMeta(renderer, rect, metaBottomY, globalStats, /*black=*/true);
}

void DashboardTheme::drawSleepScreen(const GfxRenderer& renderer, const RecentBook& book, const BookReadingStats* stats,
                                     const GlobalReadingStats* globalStats, const float progressPercent,
                                     const char* currentChapterTitle, const bool inverted) const {
  (void)currentChapterTitle;
  renderer.clearScreen(inverted ? 0xFF : 0x00);

  const Rect contentRect{0, DashboardMetrics::values.homeTopPadding, renderer.getScreenWidth(),
                         DashboardMetrics::values.homeCoverTileHeight};
  const int coverTopY = drawTopBookTitle(renderer, contentRect, book, /*black=*/!inverted);
  const Rect coverRect = coverRectForScreen(renderer, contentRect, coverTopY);
  drawBookCover(renderer, coverRect, book, inverted ? Color::White : Color::Black);
  drawDashboardStats(renderer, coverRect, stats, progressPercent, inverted);
  const int metaBottomY = drawMetaStatsUnderCover(renderer, coverRect, globalStats, !inverted);
  drawLifetimeStatsUnderMeta(renderer, contentRect, metaBottomY, globalStats, /*black=*/!inverted);
}
