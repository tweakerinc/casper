#pragma once

#include <cstdint>

#include "components/themes/HomeCoverMetrics.h"
#include "components/themes/minimal/MinimalTheme.h"

namespace DashboardMetrics {
constexpr ThemeMetrics makeValues() {
  ThemeMetrics v = MinimalMetrics::values;
  v.homeTopPadding = 50;
  v.homeCoverHeight = HomeCoverMetrics::imageHeight;  // shared hero height with Bare/Focus
  v.homeCoverTileHeight = 720;
  v.homeRecentBooksCount = 1;
  v.homeContinueReadingInMenu = false;
  v.homeMenuTopOffset = 0;
  // Inherit MinimalMetrics::buttonHintsHeight (text-only footer, shared with Bare).
  return v;
}

constexpr ThemeMetrics values = makeValues();
// Same on-disk hero recipe as Bare/Focus (c28). Bare-native size for 1:1 blit.
constexpr int homeCoverImageWidth = HomeCoverMetrics::imageWidth;
constexpr int homeCoverImageHeight = HomeCoverMetrics::imageHeight;
constexpr int homeCoverThumbHeight = HomeCoverMetrics::thumbHeight;
// Compact shelf thumbs (draw path can fall back to hero if missing).
constexpr int homeShelfThumbHeight = 168;
}  // namespace DashboardMetrics

// Parked themes (not in picker; load remaps → STATS_LIFE). Kept for easy restore:
// dist/theme-backup-shelf-scroll/README.md
namespace DashboardRecentsMetrics {
constexpr ThemeMetrics makeValues() {
  ThemeMetrics v = DashboardMetrics::values;
  // Hard cap: 4 recents = 4 shelf slots (selection underline, no sliding window).
  v.homeRecentBooksCount = 4;
  return v;
}
constexpr ThemeMetrics values = makeValues();
}  // namespace DashboardRecentsMetrics

namespace DashboardScrollMetrics {
constexpr ThemeMetrics makeValues() {
  ThemeMetrics v = DashboardMetrics::values;
  v.homeRecentBooksCount = 4;  // L/R among up to 4 recents; only hero cover is drawn
  return v;
}
constexpr ThemeMetrics values = makeValues();
}  // namespace DashboardScrollMetrics

class DashboardTheme : public MinimalTheme {
 public:
  void drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                           int selectorIndex, bool& coverRendered, bool& coverBufferStored, bool& bufferRestored,
                           StoreCoverBufferFn storeCoverBuffer, const BookReadingStats* stats = nullptr,
                           float progressPercent = -1.0f, const GlobalReadingStats* globalStats = nullptr,
                           const char* currentChapterTitle = nullptr) const override;
  void drawSleepScreen(const GfxRenderer& renderer, const RecentBook& book, const BookReadingStats* stats,
                       const GlobalReadingStats* globalStats, float progressPercent = -1.0f,
                       const char* currentChapterTitle = nullptr, bool inverted = false) const;
};
