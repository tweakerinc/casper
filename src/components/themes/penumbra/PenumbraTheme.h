#pragma once

#include <HalGPIO.h>

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

// Penumbra home theme. No cover art.
// X3: large centered clock (top) + under-panel:
//   Title/Author · Recents · Stats · Lifetime — 4 page-dots when tracking is on.
// X4: Now Reading title/author (top, ALWAYS last-read book index 0) + under-panel:
//   Recents only (no stats tracking on X4 — no RTC). No page-dots.
//   Side buttons remappable (defaults: panel scroll; panel cycle is a no-op on X4).
namespace PenumbraMetrics {
constexpr ThemeMetrics makeValues() {
  ThemeMetrics v = MinimalMetrics::values;
  v.homeTopPadding = 10;
  // No cover plate — tile height is unused for layout but kept nonzero for
  // touch-band math that uses homeCoverTileHeight in classic themes.
  v.homeCoverHeight = 1;
  v.homeCoverTileHeight = 1;
  // Cap for under-panel list (X4 uses 5 without View All; X3 uses ≤4 + View All).
  v.homeRecentBooksCount = 5;
  v.homeContinueReadingInMenu = false;
  v.homeMenuTopOffset = 0;
  // Text-only footer band — see MinimalTheme (drawButtonHints is shared).
  v.buttonHintsHeight = 34;
  return v;
}

constexpr ThemeMetrics values = makeValues();
}  // namespace PenumbraMetrics

// Session-only under-panel mode (not persisted). HomeActivity cycles on side L/R.
// Numeric values match X4 page-dot order so Recents is always slot 0 (leftmost).
namespace PenumbraThemeUi {
enum class UnderMode : uint8_t {
  Recents = 0,      // X4 only under-page. Also used on X3.
  BookStats = 1,    // X3 stats page only (never used on X4 — tracking off).
  Lifetime = 2,     // X3 only.
  TitleAuthor = 3,  // X3 only (title under clock). Never a real X4 page.
  Count = 4
};

// First under-panel page for this device (X4 = Recents #1, X3 = Title/Author).
inline UnderMode defaultUnderMode() { return gpio.deviceIsX3() ? UnderMode::TitleAuthor : UnderMode::Recents; }

inline UnderMode& underMode() {
  // Lazy default: re-evaluated only once. Always force-clamp on paint / home enter.
  static UnderMode mode = UnderMode::Recents;
  return mode;
}

// Reset to the device default page (call on theme enter / theme switch / home enter).
inline void resetUnderModeToDefault() { underMode() = defaultUnderMode(); }

// --- X4: Recents only (no multi-page under-panel) ---
inline int x4PageIndex(const UnderMode m) {
  (void)m;
  return 0;
}

inline UnderMode x4ModeFromPage(const int page) {
  (void)page;
  return UnderMode::Recents;
}

// --- X3 page-dot / cycle order: Title (0) → Recents (1) → Stats (2) → Lifetime (3) ---
inline int x3PageIndex(const UnderMode m) {
  switch (m) {
    case UnderMode::TitleAuthor:
      return 0;
    case UnderMode::Recents:
      return 1;
    case UnderMode::BookStats:
      return 2;
    case UnderMode::Lifetime:
      return 3;
    default:
      return 0;
  }
}

inline UnderMode x3ModeFromPage(const int page) {
  switch (page) {
    case 1:
      return UnderMode::Recents;
    case 2:
      return UnderMode::BookStats;
    case 3:
      return UnderMode::Lifetime;
    case 0:
    default:
      return UnderMode::TitleAuthor;
  }
}

// True when under-panel is the recents list (either device).
// X4 TitleAuthor is not a real page — treat as Recents for Down / Read.
inline bool isRecentsUnderPanel() {
  const UnderMode m = underMode();
  if (m == UnderMode::Recents) return true;
  return !gpio.deviceIsX3() && m == UnderMode::TitleAuthor;
}

// Legacy name used by HomeActivity.
inline bool isX4RecentsUnderPanel() { return !gpio.deviceIsX3() && isRecentsUnderPanel(); }

// Collapse invalid / stats pages when tracking is disabled.
// X4 never has multi-page under-panel (tracking hard-off) → always Recents.
inline void clampUnderModeToTracking() {
  if (!gpio.deviceIsX3() || !SETTINGS.readingStatsTrackingEnabled()) {
    underMode() = defaultUnderMode();
    return;
  }
}

// Returns true if the mode changed (caller can window-repaint).
// X3: Title → Recents → Stats → Lifetime (4 pages) when tracking is on.
// X4: no cycle (Recents only).
inline bool cycleUnderMode(const int delta) {
  if (!gpio.deviceIsX3() || !SETTINGS.readingStatsTrackingEnabled()) {
    underMode() = defaultUnderMode();
    return false;
  }
  constexpr int n = 4;
  int page = x3PageIndex(underMode());
  page = (page + delta) % n;
  if (page < 0) page += n;
  const UnderMode next = x3ModeFromPage(page);
  if (next == underMode()) return false;
  underMode() = next;
  return true;
}

// White-fill + redraw the band below the center rule. listFocusIndex picks the
// highlighted row in the Recents under-panel only (does NOT change upper title).
Rect redrawUnderPanel(GfxRenderer& renderer, const std::vector<RecentBook>& recentBooks, int listFocusIndex,
                      const BookReadingStats* stats, float progressPercent, const GlobalReadingStats* globalStats);

// Ensure Recents progress % is in RAM. SD load only for paths not already cached
// (or when the list is empty). Path order changes reuse prior % by path match —
// does not re-read all N books from SD. Call after loadRecentBooks on resume.
void warmRecentsProgressCache(const std::vector<RecentBook>& books);

// Update one book's micro-bar % after reader exit (or mark finished). No SD I/O.
// Prefer this over force-reloading every recent book.
void updateRecentsProgressForPath(const char* bookPath, float progressPercent);

// Drop the RAM progress cache (clear cache / heavy book actions). Next warm reloads SD.
void invalidateRecentsProgressCache();

// White-fill only the clock digit band (not weekday/hairline) and redraw time.
// prevTime: last drawn string ("H:MM") so the dirty rect can be the union of old/new
// and, when only the minutes change, only the changing suffix is cleared.
// Returns the tight dirty rect for a windowed/soft panel update.
Rect redrawClockBlock(GfxRenderer& renderer, const char* prevTime = nullptr, char* outTime = nullptr,
                      size_t outTimeSize = 0);

// X3 only: 72pt clock is 2-bit AA, but BW home paints drop light fringe → jagged.
// Call after BW home is in the framebuffer (full paint or after redrawClockBlock).
// baseMode = HALF/FAST for greyscale base; window greys over the clock digit band.
// Returns false if not X3 / storeBw failed (caller should plain-display BW).
bool displayClockAntiAliased(GfxRenderer& renderer, int baseRefreshMode, const Rect* dirtyOverride = nullptr);

bool formatHeroTimeNow(char* buf, size_t bufSize);
}  // namespace PenumbraThemeUi

class PenumbraTheme : public MinimalTheme {
 public:
  // selectorIndex: list focus for Recents under-panel ONLY.
  // Upper "Now Reading" / X3 clock title / Stats always use last-read book (index 0).
  void drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                           int selectorIndex, bool& coverRendered, bool& coverBufferStored, bool& bufferRestored,
                           StoreCoverBufferFn storeCoverBuffer, const BookReadingStats* stats = nullptr,
                           float progressPercent = -1.0f, const GlobalReadingStats* globalStats = nullptr,
                           const char* currentChapterTitle = nullptr) const override;
};
