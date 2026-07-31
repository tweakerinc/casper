#pragma once

#include <cstdint>
#include <vector>

#include "components/themes/HomeCoverMetrics.h"
#include "components/themes/minimal/MinimalTheme.h"

class GfxRenderer;
struct RecentBook;
struct GlobalReadingStats;

// Stats home (X3 + X4 same layout): large uncropped jacket left + book-stats
// column right. Under the box (side Left/Right toggles on Home):
//   default → book title + author
//   toggled → Lifetime Stats card (same cover plate either way)
// Calendar-dependent stats show "-" when date/RTC data is unavailable.
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

// Session toggle for Stats under-box (title/author vs lifetime card).
// HomeActivity flips this on side Left/Right; not persisted.
namespace FocusThemeUi {
inline bool& showLifeUnderBox() {
  static bool v = false;
  return v;
}

// White-fill + redraw only the free band under the cover|stats box (title ↔
// lifetime toggle). Leaves chrome, cover, right stats, and footer untouched.
// Returns the logical dirty rect for GfxRenderer::displayWindow — do not full-
// frame displayBuffer (that blackens multipass cover greys).
Rect redrawUnderBox(GfxRenderer& renderer, const std::vector<RecentBook>& recentBooks,
                    const GlobalReadingStats* globalStats);
}  // namespace FocusThemeUi

class FocusTheme : public MinimalTheme {
 public:
  void drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                           int selectorIndex, bool& coverRendered, bool& coverBufferStored, bool& bufferRestored,
                           StoreCoverBufferFn storeCoverBuffer, const BookReadingStats* stats = nullptr,
                           float progressPercent = -1.0f, const GlobalReadingStats* globalStats = nullptr,
                           const char* currentChapterTitle = nullptr) const override;
};
