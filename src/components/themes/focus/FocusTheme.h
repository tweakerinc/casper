#pragma once

#include <cstdint>

#include "components/themes/HomeCoverMetrics.h"
#include "components/themes/minimal/MinimalTheme.h"

// Stats home (and Stats-Life base): large uncropped jacket left + book-stats
// column right (top-aligned). Under the box:
//   Stats      → book title + author (Bare spacing)
//   Stats-Life → Lifetime Stats card (same cover gen as Stats)
namespace FocusMetrics {
constexpr ThemeMetrics makeValues() {
  ThemeMetrics v = MinimalMetrics::values;
  v.homeTopPadding = 50;  // room for battery / clock chrome
  v.homeCoverHeight = HomeCoverMetrics::imageHeight;
  v.homeCoverTileHeight = 780;
  v.homeRecentBooksCount = 1;
  v.homeContinueReadingInMenu = false;
  v.homeMenuTopOffset = 0;
  v.buttonHintsHeight = 48;
  return v;
}

constexpr ThemeMetrics values = makeValues();
// Same on-disk hero as Bare/Stats — do not use a different height (that forces
// a second gen + scale and shows dither grids).
constexpr int homeCoverImageWidth = HomeCoverMetrics::imageWidth;
constexpr int homeCoverImageHeight = HomeCoverMetrics::imageHeight;
constexpr int homeCoverThumbHeight = HomeCoverMetrics::thumbHeight;
}  // namespace FocusMetrics

class FocusTheme : public MinimalTheme {
 public:
  void drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                           int selectorIndex, bool& coverRendered, bool& coverBufferStored, bool& bufferRestored,
                           StoreCoverBufferFn storeCoverBuffer, const BookReadingStats* stats = nullptr,
                           float progressPercent = -1.0f, const GlobalReadingStats* globalStats = nullptr,
                           const char* currentChapterTitle = nullptr) const override;
};
