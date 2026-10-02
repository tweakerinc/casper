#pragma once

#include <cstdint>

#include "components/themes/HomeCoverMetrics.h"
#include "components/themes/minimal/MinimalTheme.h"

// Bare home: large centered cover nearly to the top, title, author, text-only
// front labels (Menu · Library · Synopsis · Read). Menu opens the Stats popup
// list with Settings at the bottom. No battery/clock chrome by default.
namespace BareMetrics {
constexpr ThemeMetrics makeValues() {
  ThemeMetrics v = MinimalMetrics::values;
  v.homeTopPadding = 10;
  // Prefer a tall cover; layout centers it between top chrome and title.
  v.homeCoverHeight = HomeCoverMetrics::imageHeight;
  v.homeCoverTileHeight = 780;
  v.homeRecentBooksCount = 1;
  v.homeContinueReadingInMenu = false;
  v.homeMenuTopOffset = 0;
  v.buttonHintsHeight = 34;  // text-only labels, no outline chrome (see MinimalTheme)
  return v;
}

constexpr ThemeMetrics values = makeValues();
// Shared hero gen with Stats/Focus (HomeCoverMetrics). Contain-fit, centered frame.
constexpr int homeCoverImageWidth = HomeCoverMetrics::imageWidth;
constexpr int homeCoverImageHeight = HomeCoverMetrics::imageHeight;
constexpr int homeCoverThumbHeight = HomeCoverMetrics::thumbHeight;
}  // namespace BareMetrics

class BareTheme : public MinimalTheme {
 public:
  void drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                           int selectorIndex, bool& coverRendered, bool& coverBufferStored, bool& bufferRestored,
                           StoreCoverBufferFn storeCoverBuffer, const BookReadingStats* stats = nullptr,
                           float progressPercent = -1.0f, const GlobalReadingStats* globalStats = nullptr,
                           const char* currentChapterTitle = nullptr) const override;

  // Footer chrome: MinimalTheme::drawButtonHints (shared with Stats).
};
