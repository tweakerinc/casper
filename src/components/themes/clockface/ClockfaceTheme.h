#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "CrossPointSettings.h"
#include "components/themes/minimal/MinimalTheme.h"

class GfxRenderer;
struct RecentBook;
struct BookReadingStats;
struct GlobalReadingStats;
struct Rect;

// Spectral home (X3 only; internal class still ClockfaceTheme): large centered
// clock (top half) + lower panel. Side buttons remappable (defaults: both panel
// scroll). Same action on both sides is bidirectional; a single side one-way
// cycles. No cover art. Not offered on X4.
namespace ClockfaceMetrics {
constexpr ThemeMetrics makeValues() {
  ThemeMetrics v = MinimalMetrics::values;
  v.homeTopPadding = 10;
  // No cover plate — tile height is unused for layout but kept nonzero for
  // touch-band math that uses homeCoverTileHeight in classic themes.
  v.homeCoverHeight = 1;
  v.homeCoverTileHeight = 1;
  // Current book + 3 more recents (side Left steps this list).
  v.homeRecentBooksCount = 4;
  v.homeContinueReadingInMenu = false;
  v.homeMenuTopOffset = 0;
  v.buttonHintsHeight = 48;
  return v;
}

constexpr ThemeMetrics values = makeValues();
}  // namespace ClockfaceMetrics

// Session-only under-panel mode (not persisted). HomeActivity cycles on side L/R.
// When reading-stats tracking is off, only TitleAuthor is valid (no stats pages).
namespace ClockfaceThemeUi {
enum class UnderMode : uint8_t { TitleAuthor = 0, BookStats = 1, Lifetime = 2, Count = 3 };

inline UnderMode& underMode() {
  static UnderMode mode = UnderMode::TitleAuthor;
  return mode;
}

// Collapse BookStats/Lifetime back to title when tracking is disabled mid-session.
inline void clampUnderModeToTracking() {
  if (!SETTINGS.readingStatsTrackingEnabled() && underMode() != UnderMode::TitleAuthor) {
    underMode() = UnderMode::TitleAuthor;
  }
}

// Returns true if the mode changed (caller can window-repaint). No-op when
// tracking is off — stays on title/author only.
inline bool cycleUnderMode(const int delta) {
  if (!SETTINGS.readingStatsTrackingEnabled()) {
    underMode() = UnderMode::TitleAuthor;
    return false;
  }
  const int n = static_cast<int>(UnderMode::Count);
  int v = (static_cast<int>(underMode()) + delta) % n;
  if (v < 0) v += n;
  const UnderMode next = static_cast<UnderMode>(v);
  if (next == underMode()) return false;
  underMode() = next;
  return true;
}

// White-fill + redraw the band below the center rule. Prefer HomeActivity's
// full-panel displayWindow path so paper white stays even. selectorIndex picks
// which recent book is shown (Spectral keeps up to 4).
Rect redrawUnderPanel(GfxRenderer& renderer, const std::vector<RecentBook>& recentBooks, int selectorIndex,
                      const BookReadingStats* stats, float progressPercent,
                      const GlobalReadingStats* globalStats);

// White-fill full-width upper half + redraw clock/weekday/rule. Prefer
// full-panel displayWindow from HomeActivity (tight digit windows leave a
// pure-white box vs greyer residual paper).
// outTime: optional "H:MM" / "HH:MM" buffer (>=8) of the string just drawn.
Rect redrawClockBlock(GfxRenderer& renderer, char* outTime = nullptr, size_t outTimeSize = 0);

// Fresh local wall time for the hero clock (always polls RTC; ignores 10s cache).
// Writes "H:MM" or "HH:MM" (no AM/PM). Returns false if RTC unavailable.
bool formatHeroTimeNow(char* buf, size_t bufSize);
}  // namespace ClockfaceThemeUi

class ClockfaceTheme : public MinimalTheme {
 public:
  void drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                           int selectorIndex, bool& coverRendered, bool& coverBufferStored, bool& bufferRestored,
                           StoreCoverBufferFn storeCoverBuffer, const BookReadingStats* stats = nullptr,
                           float progressPercent = -1.0f, const GlobalReadingStats* globalStats = nullptr,
                           const char* currentChapterTitle = nullptr) const override;
};
