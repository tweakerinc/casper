#include "HomeActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Utf8.h>
#include <Xtc.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <vector>

#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalStorage.h>

#include "BookActions.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "FileBrowserActionActivity.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "RecentBooksStore.h"
#include "activities/reader/BookStatsActivity.h"
#include "activities/reader/GlobalReadingStats.h"
#include "activities/reader/ReaderActivity.h"
#include "components/UITheme.h"
#include "components/themes/BaseTheme.h"
#include "components/themes/HomeCoverMetrics.h"
#include "components/themes/bare/BareTheme.h"
#include "components/themes/dashboard/DashboardTheme.h"
#include "components/themes/clockface/ClockfaceTheme.h"
#include "components/themes/focus/FocusTheme.h"
#include "fontIds.h"

namespace {

// Home / Bare long-press (Read menu, Library→Recents, Menu→Settings).
// Was 1000 → 500; 300ms still separates tap from hold without feeling sticky.
constexpr unsigned long READ_LONG_PRESS_MS = 300;
// Start aborting multipass greys before the threshold so waitForRenderIdle is
// short when the menu push runs (otherwise hold + grey stage feels like 1s+).
constexpr unsigned long LONG_PRESS_PRECANCEL_MS = 120;

// Shelf / Stats Scroll parked (remapped → STATS). Dead until picker restore.
bool isDashboardRecentsTheme() { return false; }

bool isDashboardScrollTheme() { return false; }

bool usesRecentBookSideNav() { return isDashboardRecentsTheme() || isDashboardScrollTheme(); }

bool isBareTheme() {
  return static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme) == CrossPointSettings::UI_THEME::BARE;
}

bool isSpectralTheme() {
  return static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme) == CrossPointSettings::UI_THEME::SPECTRAL;
}

// Stats home (cover + book stats; side L/R toggles title ↔ lifetime under-box).
// Legacy STATS_LIFE / parked ids still match until load remaps them to STATS.
bool isStatsTheme() {
  using T = CrossPointSettings::UI_THEME;
  const auto theme = static_cast<T>(SETTINGS.uiTheme);
  return theme == T::STATS || theme == T::STATS_LIFE || theme == T::DASHBOARD_RECENTS ||
         theme == T::DASHBOARD_SCROLL || theme == T::DASHBOARD_MAGAZINE || theme == T::DASHBOARD_CARD ||
         theme == T::MINIMAL || theme == T::LYRA_CAROUSEL;
}

// Front-button home chrome (no classic bottom list).
bool usesMinimalHomeInteraction() {
  return isBareTheme() || isSpectralTheme() || isStatsTheme();
}

// All minimal homes: Menu · Library · Recents · Read (Settings under Menu /
// long-press Menu). Unified Bare / Spectral / Stats.
bool usesBareStyleHomeNav() { return usesMinimalHomeInteraction(); }

// Stats family: FocusTheme layout + shared cover gen size.
bool usesStatsFamilyCover() { return isStatsTheme(); }

// Themes that paint a cover and need multipass greys. SPECTRAL is text-only.
bool usesHomeCoverMultipass() {
  return usesMinimalHomeInteraction() && !isSpectralTheme();
}

// Hero thumb height for the *current* theme so gen size matches on-screen blit
// (1:1). Scaling 2-bit Atkinson is what creates gridlines.
int homeHeroThumbHeight(const GfxRenderer& renderer, const int fallbackCoverHeight) {
  if (isBareTheme()) {
    return HomeCoverMetrics::thumbHeight;  // 560 → 420×560, Bare 1:1
  }
  // Stats: shared height key (same thumb file, same plate).
  if (usesStatsFamilyCover()) {
    return HomeCoverMetrics::statsFamilyHeroThumbHeight(renderer.getScreenWidth(),
                                                        renderer.getScreenHeight());
  }
  return fallbackCoverHeight;
}

bool isAnyFrontButtonPressed(const MappedInputManager& mappedInput) {
  return mappedInput.isFrontButtonPressed(HalGPIO::BTN_BACK) ||
         mappedInput.isFrontButtonPressed(HalGPIO::BTN_CONFIRM) ||
         mappedInput.isFrontButtonPressed(HalGPIO::BTN_LEFT) || mappedInput.isFrontButtonPressed(HalGPIO::BTN_RIGHT);
}

// Popup menu: Dashboard BACK (Menu) / Bare CONFIRM (Menu).
// Settings is always in the Menu list (front bar is Menu · Library · Recents · Read).
// Recents is a front button — not duplicated in the popup menu.
enum class MinimalMenuAction : uint8_t { ReadingStats, OpdsBrowser, FileTransfer, Settings };

struct MinimalMenuItem {
  const char* label;
  UIIcon icon;
  MinimalMenuAction action;
};

int buildMinimalMenuItems(MinimalMenuItem* out, int maxItems, const bool hasOpdsServers,
                          const bool hasCurrentBook, const bool includeSettings) {
  int n = 0;
  if (hasCurrentBook && SETTINGS.readingStatsTrackingEnabled() && n < maxItems) {
    out[n++] = {tr(STR_READING_STATS), Book, MinimalMenuAction::ReadingStats};
  }
  if (hasOpdsServers && n < maxItems) {
    out[n++] = {tr(STR_OPDS_BROWSER), Library, MinimalMenuAction::OpdsBrowser};
  }
  if (n < maxItems) out[n++] = {tr(STR_FILE_TRANSFER), Transfer, MinimalMenuAction::FileTransfer};
  if (includeSettings && n < maxItems) {
    out[n++] = {tr(STR_SETTINGS_TITLE), Settings, MinimalMenuAction::Settings};
  }
  return n;
}

// Same path as EpubReaderActivity save: Epub(path, "/.crosspoint").getCachePath().
// Constructor hashes the path; load() is not required for the cache directory.
std::string getRecentBookCachePath(const RecentBook& book) {
  if (FsHelpers::hasEpubExtension(book.path)) {
    return Epub(book.path, "/.crosspoint").getCachePath();
  }
  if (FsHelpers::hasXtcExtension(book.path)) {
    return std::string("/.crosspoint/xtc_") + std::to_string(std::hash<std::string>{}(book.path));
  }
  if (FsHelpers::hasTxtExtension(book.path) || FsHelpers::hasMarkdownExtension(book.path)) {
    return std::string("/.crosspoint/txt_") + std::to_string(std::hash<std::string>{}(book.path));
  }
  return {};
}

BookReadingStats loadRecentBookStats(const RecentBook& book) {
  // Dual-path: 1.5 std::hash cache first, then CrossInk FNV-64 epub_* path.
  return BookReadingStats::loadForBook(book.path);
}

// Dashboard progress %: prefer the value written on reader exit (stats v6+).
// Fail-soft: returns -1 when missing (old books / never opened this build).
// Intentionally does NOT open the EPUB — that was a home-enter peak-heap hit.
float loadRecentBookProgressPercent(const RecentBook& book) {
  const BookReadingStats stats = BookReadingStats::loadForBook(book.path);
  return stats.getProgressPercent();
}

// Treat only real BMPs as present (corrupt partial files must re-enter gen).
bool thumbLooksValid(const std::string& path) {
  if (path.empty() || !Storage.exists(path.c_str())) return false;
  HalFile probe;
  if (!Storage.openFileForRead("HOME", path, probe)) return false;
  char sig[2] = {};
  const size_t n = probe.read(sig, 2);
  const size_t sz = probe.size();
  probe.close();
  return n == 2 && sig[0] == 'B' && sig[1] == 'M' && sz > 62;
}

// Phase 1 (A1): bind hero paths when thumbs already exist so the first home paint
// can multipass immediately (skip shell-only HALF → gen → multipass).
// Returns true when no cover generation work is required.
bool bindExistingHeroThumbsIfReady(std::vector<RecentBook>& recentBooks, int heroH, bool shelfTheme,
                                   int shelfH) {
  if (recentBooks.empty()) {
    return true;
  }
  for (RecentBook& book : recentBooks) {
    if (FsHelpers::hasEpubExtension(book.path)) {
      Epub epub(book.path, "/.crosspoint");
      const std::string heroPath = epub.getThumbBmpPath(heroH);
      if (!thumbLooksValid(heroPath)) {
        return false;
      }
      if (shelfTheme && !thumbLooksValid(epub.getThumbBmpPath(shelfH))) {
        return false;
      }
      if (book.coverBmpPath.empty()) {
        book.coverBmpPath = epub.getThumbBmpPath();
      }
    } else if (FsHelpers::hasXtcExtension(book.path)) {
      Xtc xtc(book.path, "/.crosspoint");
      // Path-only probe — avoid full XTC load when the hero BMP is already on disk.
      const std::string heroPath = xtc.getThumbBmpPath(heroH);
      if (!thumbLooksValid(heroPath)) {
        return false;
      }
      if (shelfTheme && !thumbLooksValid(xtc.getThumbBmpPath(shelfH))) {
        return false;
      }
      if (book.coverBmpPath.empty()) {
        book.coverBmpPath = xtc.getThumbBmpPath();
      }
    }
    // txt/md/etc.: no cover gen.
  }
  return true;
}
}  // namespace

int HomeActivity::getMenuItemCount() const {
  int count = 4;  // File Browser, Recents, File transfer, Settings
  if (!recentBooks.empty()) {
    count += recentBooks.size();
  }
  if (hasOpdsServers) {
    count++;
  }
  return count;
}

void HomeActivity::loadRecentBooks(int maxBooks) {
  recentBooks.clear();
  const auto& books = RECENT_BOOKS.getBooks();
  recentBooks.reserve(std::min(static_cast<int>(books.size()), maxBooks));

  for (const RecentBook& book : books) {
    // Limit to maximum number of recent books
    if (recentBooks.size() >= maxBooks) {
      break;
    }

    // Skip if file no longer exists
    if (RecentBooksStore::isMissing(book)) {
      continue;
    }

    recentBooks.push_back(book);
  }
}

void HomeActivity::loadRecentCovers(int coverHeight) {
  recentsLoading = true;
  bool showingLoading = false;
  Rect popupRect;

  // Per-theme hero height so gen matches the on-screen plate (1:1, no grids).
  // Shelf still needs a compact 168px row when that theme is active.
  const int heroH = homeHeroThumbHeight(renderer, coverHeight);
  const bool shelfTheme = isDashboardRecentsTheme();
  const int shelfH = DashboardMetrics::homeShelfThumbHeight;

  auto heroThumbExists = [&](auto& bookFmt) -> bool { return thumbLooksValid(bookFmt.getThumbBmpPath(heroH)); };
  auto ensureThumbs = [&](auto& bookFmt) -> bool {
    // Generate hero first; only then optional shelf size (no second full decode when hero ok).
    bool anyOk = bookFmt.generateThumbBmp(heroH);
    if (shelfTheme && !thumbLooksValid(bookFmt.getThumbBmpPath(shelfH))) {
      anyOk = bookFmt.generateThumbBmp(shelfH) || anyOk;
    }
    return anyOk;
  };

  // Pre-scan so we only free the on-screen cover snapshot when generation work
  // is required (returning from the reader with thumbs already on disk used to
  // wipe a good paint and race into a blank frame).
  bool anyNeedWork = false;
  for (const RecentBook& book : recentBooks) {
    if (FsHelpers::hasEpubExtension(book.path)) {
      Epub epub(book.path, "/.crosspoint");
      if (!heroThumbExists(epub) || (shelfTheme && !thumbLooksValid(epub.getThumbBmpPath(shelfH)))) {
        anyNeedWork = true;
        break;
      }
    } else if (FsHelpers::hasXtcExtension(book.path)) {
      Xtc xtc(book.path, "/.crosspoint");
      if (!xtc.load()) {
        anyNeedWork = true;
        break;
      }
      if (!heroThumbExists(xtc) || (shelfTheme && !thumbLooksValid(xtc.getThumbBmpPath(shelfH)))) {
        anyNeedWork = true;
        break;
      }
    }
  }

  if (anyNeedWork) {
    // Free snapshot RAM for decode heap, but do NOT clear coverGrayOnPanel when
    // the panel already shows a good multipass (retry would flash black 5–10s later).
    if (!coverGrayOnPanel) {
      freeCoverBuffer();
      coverRendered = false;
      coverBufferStored = false;
    } else if (coverBuffer) {
      free(coverBuffer);
      coverBuffer = nullptr;
      coverBufferSize = 0;
      coverBufferStored = false;
      // keep coverRendered / coverGrayOnPanel — panel is still correct
    }
  }

  // Draw Loading into the framebuffer only — do NOT displayWindow / half-refresh
  // here. On X4 (SSD1677), a window while greys are on glass can promote to a full
  // HALF clean and black-flash in a loop. v0.1.3 only painted the dialog into the
  // FB; the next full multipass/home paint shows the result once.
  auto showProgress = [&](int progress, int total) {
    // Stats jacket is large — pin dialog lower so it is not under the cover plate.
    const float topRatio = usesStatsFamilyCover() ? 0.72f : BaseTheme::kPopupCenterY;
    if (!showingLoading) {
      popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP), topRatio, /*refresh=*/false);
      showingLoading = true;
    }
    GUI.fillPopupProgress(renderer, popupRect, 10 + progress * (90 / std::max(1, total)), /*refresh=*/false);
  };

  const int total = std::max(1, static_cast<int>(recentBooks.size()));
  int progress = 0;
  bool anyNewThumb = false;
  bool anyTransientFail = false;
  bool pathsUpdated = false;
  for (RecentBook& book : recentBooks) {
    if (FsHelpers::hasEpubExtension(book.path)) {
      Epub epub(book.path, "/.crosspoint");
      const bool needWork =
          !heroThumbExists(epub) || (shelfTheme && !thumbLooksValid(epub.getThumbBmpPath(shelfH)));
      if (needWork) {
        showProgress(progress, total);
        // Free snapshot RAM for decode, but keep coverGrayOnPanel if the panel
        // already shows a settled multipass (avoids delayed black re-flash).
        if (!coverGrayOnPanel) {
          freeCoverBuffer();
        } else if (coverBuffer) {
          free(coverBuffer);
          coverBuffer = nullptr;
          coverBufferSize = 0;
          coverBufferStored = false;
        }
        LOG_DBG("HOME", "Cover gen free heap before: %u maxAlloc=%u", static_cast<unsigned>(ESP.getFreeHeap()),
                static_cast<unsigned>(ESP.getMaxAllocHeap()));
        // Prefer existing book.bin (fast). Only full-index if cache is missing.
        bool loaded = epub.load(/*buildIfMissing=*/false, /*skipLoadingCss=*/true);
        if (!loaded) {
          loaded = epub.load(/*buildIfMissing=*/true, /*skipLoadingCss=*/true);
        }
        if (!loaded) {
          LOG_ERR("HOME", "EPUB load failed for cover: %s", book.path.c_str());
          anyTransientFail = true;
        } else {
          // Warm synopsis cache while the EPUB is already open (miss = one OPF
          // metadata pass here instead of a multi-second stall on Synopsis).
          (void)epub.getDescription();
          if (ensureThumbs(epub)) {
            const std::string templatePath = epub.getThumbBmpPath();
            RECENT_BOOKS.updateBook(book.path, book.title, book.author, templatePath);
            book.coverBmpPath = templatePath;
            anyNewThumb = true;
          } else {
            LOG_ERR("HOME", "Thumb generate failed for: %s (heap=%u maxAlloc=%u)", book.path.c_str(),
                    static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
            anyTransientFail = true;
          }
        }
        // Epub object + metadata cache go out of scope next — yield so the heap
        // can coalesce before the next book’s JPEG decode.
        delay(1);
      } else {
        // Thumbs already ready: still warm synopsis for the focused (first) recent
        // so opening Synopsis does not cold-parse OPF on the critical path.
        if (progress == 0) {
          const std::string descPath = epub.getCachePath() + "/description.html";
          if (!Storage.exists(descPath.c_str())) {
            if (epub.load(/*buildIfMissing=*/false, /*skipLoadingCss=*/true) ||
                epub.load(/*buildIfMissing=*/true, /*skipLoadingCss=*/true)) {
              (void)epub.getDescription();
            }
          }
        }
        if (book.coverBmpPath.empty()) {
          const std::string templatePath = epub.getThumbBmpPath();
          RECENT_BOOKS.updateBook(book.path, book.title, book.author, templatePath);
          book.coverBmpPath = templatePath;
          pathsUpdated = true;
        }
      }
    } else if (FsHelpers::hasXtcExtension(book.path)) {
      Xtc xtc(book.path, "/.crosspoint");
      if (xtc.load()) {
        const bool needWork =
            !heroThumbExists(xtc) || (shelfTheme && !thumbLooksValid(xtc.getThumbBmpPath(shelfH)));
        if (needWork) {
          showProgress(progress, total);
          if (ensureThumbs(xtc)) {
            const std::string templatePath = xtc.getThumbBmpPath();
            RECENT_BOOKS.updateBook(book.path, book.title, book.author, templatePath);
            book.coverBmpPath = templatePath;
            anyNewThumb = true;
          } else {
            anyTransientFail = true;
          }
        } else if (book.coverBmpPath.empty()) {
          const std::string templatePath = xtc.getThumbBmpPath();
          RECENT_BOOKS.updateBook(book.path, book.title, book.author, templatePath);
          book.coverBmpPath = templatePath;
          pathsUpdated = true;
        }
      } else {
        anyTransientFail = true;
      }
    }
    progress++;
  }

  // Stop render-path re-entry (loop only kicks when !recentsLoaded).
  recentsLoaded = true;
  recentsLoading = false;
  coverGenAttempts = static_cast<uint8_t>(std::min<int>(255, static_cast<int>(coverGenAttempts) + 1));

  // Only retry gen when the panel still has no good gray cover (not after success).
  if (anyTransientFail && coverGenAttempts < kMaxCoverGenAttempts && !coverGrayOnPanel) {
    coverNeedsRetry = true;
    coverRetryAtMs = millis() + 1500UL * coverGenAttempts;
    LOG_DBG("HOME", "Cover gen will retry (%u/%u) in %lums", static_cast<unsigned>(coverGenAttempts),
            static_cast<unsigned>(kMaxCoverGenAttempts), static_cast<unsigned long>(1500UL * coverGenAttempts));
  } else {
    coverNeedsRetry = false;
  }

  // Repaint only for new pixels, or one multipass after the first BW shell paint.
  // Never re-multipass solely because a background gen retry ran, and never when
  // greys are already settled (idle black-flash bug).
  if (anyNewThumb && !coverGrayOnPanel) {
    freeCoverBuffer();
    coverRendered = false;
    coverBufferStored = false;
    requestUpdate();
  } else if (anyNewThumb && coverGrayOnPanel) {
    // New thumb on disk but panel already shows gray of prior art — soft update
    // next intentional paint only (do not multipass on a timer).
    LOG_DBG("HOME", "New thumb ready; panel already gray — skip timed multipass");
  } else if (!coverGrayOnPanel) {
    requestUpdate();
  } else if (pathsUpdated) {
    LOG_DBG("HOME", "Cover path templates updated; skip repaint (art unchanged)");
  }
  (void)showingLoading;
  (void)anyNeedWork;
}

void HomeActivity::onEnter() {
  Activity::onEnter();

  // Home chrome (Dashboard/Minimal/classic) is portrait-only. Reader may leave
  // landscape orientation; force portrait so X3 (528×792) and X4 (480×800)
  // pack the same layout math.
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  hasOpdsServers = OPDS_STORE.hasServers();
  minimalMenuOpen = false;
  homeMenuShellOnPanel = false;
  menuLongPressFired = false;
  minimalSuppressInitialFrontRelease = usesMinimalHomeInteraction();
  readLongPressFired = false;
  backPressSeen = false;
  backResumeArmed = false;
  minimalMenuIndex = 0;
  // Allow cover pass to run again after leaving reader / changing theme.
  // Clear settled multipass so a theme switch always redraws and re-multipasses.
  freeCoverBuffer();
  paintedUiTheme = -1;
  forceStatsUnderBoxRepaint = false;
  forceHomeShellRepaint = false;
  coverGrayNeedsRetry = false;
  coverGrayRetryAtMs = 0;
  recentsLoaded = false;
  recentsLoading = false;
  homeUiReady = false;
  coverNeedsRetry = false;
  coverGenAttempts = 0;
  coverRetryAtMs = 0;

  const auto& metrics = UITheme::getInstance().getMetrics();
  loadRecentBooks(metrics.homeRecentBooksCount);

  currentBookStats = BookReadingStats{};
  currentBookProgressPercent = -1.0f;
  globalStats = GlobalReadingStats::load();
  // Stats Recents (and other dashboard homes): selectorIndex is the focused recent.
  // Classic list themes still use it for the bottom menu row.
  selectorIndex = 0;
  if (!recentBooks.empty()) {
    loadFocusedRecentStats();
  }

  if (!usesMinimalHomeInteraction()) {
    const auto base = static_cast<int>(recentBooks.size());
    selectorIndex =
        initialMenuItem == HomeMenuItem::NONE ? 0 : base + menuItemToIndex(initialMenuItem, hasOpdsServers);
  }

  // Phase 1 (A1): when hero thumbs already exist (typical goHome after reading),
  // bind paths now so the first paint multipasses with real art — no shell-only HALF.
  {
    const int heroH = homeHeroThumbHeight(renderer, metrics.homeCoverHeight);
    const bool shelfTheme = isDashboardRecentsTheme();
    if (bindExistingHeroThumbsIfReady(recentBooks, heroH, shelfTheme, DashboardMetrics::homeShelfThumbHeight)) {
      recentsLoaded = true;
      LOG_DBG("HOME", "Hero thumbs ready — multipass on first paint (skip shell HALF)");
    }
  }

  // Paint home first so Loading can float over real UI (title/footer visible).
  requestUpdate();
}

void HomeActivity::onExit() {
  Activity::onExit();

  // Free the stored cover buffer if any
  freeCoverBuffer();
  coverGrayOnPanel = false;
}

void HomeActivity::onResume() {
  // Phase 2: Home stayed alive under the reader / Settings. The panel was painted
  // by the child, so we must redraw — but NOT always multipass. Multipass greys
  // black-flash. UI children (Settings, Library, …) use a snappy single FAST BW
  // shell; reader resume / theme change still multipass.
  Activity::onResume();

  const int themeNow = static_cast<int>(SETTINGS.uiTheme);
  const bool themeUnchanged = (paintedUiTheme == themeNow && themeNow >= 0);
  const bool snappy = leaveForUiChildSnappy && themeUnchanged && usesHomeCoverMultipass();
  leaveForUiChildSnappy = false;

  freeCoverBufferRamOnly();
  // Panel was overwritten by the child — greys on glass are gone.
  coverGrayOnPanel = false;
  coverRendered = false;
  coverGrayNeedsRetry = false;
  coverGrayRetryAtMs = 0;
  recentsLoading = false;
  homeUiReady = true;
  coverNeedsRetry = false;
  coverGenAttempts = 0;
  coverRetryAtMs = 0;
  minimalMenuOpen = false;
  homeMenuShellOnPanel = false;
  menuLongPressFired = false;
  readLongPressFired = false;
  backPressSeen = false;
  backResumeArmed = false;
  minimalSuppressInitialFrontRelease = usesMinimalHomeInteraction();
  snappyResumeNoGreys = snappy;
  if (!snappy) {
    // Full quality path: force multipass (reader return, theme change, first land).
    paintedUiTheme = -1;
  }

  // Portrait: reader may have left landscape.
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  hasOpdsServers = OPDS_STORE.hasServers();
  const auto& metrics = UITheme::getInstance().getMetrics();
  loadRecentBooks(metrics.homeRecentBooksCount);
  // Most-recent book is index 0 after a reading session.
  if (usesMinimalHomeInteraction()) {
    selectorIndex = 0;
  } else if (selectorIndex >= getMenuItemCount()) {
    selectorIndex = 0;
  }
  if (!recentBooks.empty()) {
    loadFocusedRecentStats();
  } else {
    currentBookStats = BookReadingStats{};
    currentBookProgressPercent = -1.0f;
  }
  globalStats = GlobalReadingStats::load();

  const int heroH = homeHeroThumbHeight(renderer, metrics.homeCoverHeight);
  if (bindExistingHeroThumbsIfReady(recentBooks, heroH, isDashboardRecentsTheme(),
                                    DashboardMetrics::homeShelfThumbHeight)) {
    recentsLoaded = true;
    LOG_DBG("HOME", snappy ? "onResume: snappy UI return (no multipass)" : "onResume: thumbs ready — multipass");
  } else {
    recentsLoaded = false;
    snappyResumeNoGreys = false;  // need gen path, not snappy
    LOG_DBG("HOME", "onResume: thumbs missing — gen after shell");
  }
}

bool HomeActivity::storeCoverBuffer() {
  // Theme sets coverRect* to the drawn cover art via the storeCoverBuffer(x,y,w,h) lambda.
  // Only free the previous malloc — do NOT call freeCoverBuffer() (that also clears
  // coverRendered / coverGrayOnPanel and races with the in-progress paint).
  if (coverRectW <= 0 || coverRectH <= 0) return false;
  if (coverBuffer) {
    free(coverBuffer);
    coverBuffer = nullptr;
    coverBufferSize = 0;
  }
  const size_t needed = renderer.getRegionByteSize(coverRectX, coverRectY, coverRectW, coverRectH);
  if (needed == 0) return false;
  coverBuffer = static_cast<uint8_t*>(malloc(needed));
  if (!coverBuffer) {
    LOG_ERR("HOME", "OOM: cover buffer (%u bytes)", (unsigned)needed);
    return false;
  }
  coverBufferSize = needed;
  if (!renderer.copyRegionToBuffer(coverRectX, coverRectY, coverRectW, coverRectH, coverBuffer, coverBufferSize)) {
    free(coverBuffer);
    coverBuffer = nullptr;
    coverBufferSize = 0;
    return false;
  }
  LOG_DBG("HOME", "Cover snapshot %dx%d → %u bytes", coverRectW, coverRectH, (unsigned)needed);
  return true;
}

bool HomeActivity::restoreCoverBuffer() {
  if (!coverBuffer || coverRectW <= 0 || coverRectH <= 0) return false;
  return renderer.copyBufferToRegion(coverRectX, coverRectY, coverRectW, coverRectH, coverBuffer, coverBufferSize);
}

void HomeActivity::multipassHomeCoverGrayscale() {
  // Abort if the user already left Home (e.g. Synopsis/Read while greys were
  // painting). Holding RenderLock across multipass used to freeze the main loop
  // for the full 5–15s e-ink sequence; caller unlocks before we run, and we
  // bail between stages so the pending activity can take over immediately.
  auto leavingHome = [this]() -> bool {
    return cancelBackgroundPaint || !activityManager.isCurrentActivity(this) ||
           activityManager.hasPendingActivityChange();
  };

  // Fallback: plain BW half-refresh (1-bit thumbs, missing art, classic empty state).
  // settle=true claims the panel so we do not thrash multipass retries on heap OOM.
  auto displayBw = [this, &leavingHome](bool settle) {
    if (settle) {
      coverGrayOnPanel = true;
      coverGrayNeedsRetry = false;
      paintedUiTheme = static_cast<int>(SETTINGS.uiTheme);
    } else {
      coverGrayOnPanel = false;
    }
    if (leavingHome()) {
      return;
    }
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  };

  if (leavingHome()) {
    coverGrayOnPanel = false;
    return;
  }

  if (coverRectW <= 0 || coverRectH <= 0 || recentBooks.empty()) {
    displayBw(true);
    return;
  }

  // Dashboard/Bare cycle the focused recent; classic themes always paint books[0].
  const size_t bookIdx =
      usesMinimalHomeInteraction() ? static_cast<size_t>(focusedRecentIndex()) : 0;
  const RecentBook& book = recentBooks[bookIdx];
  const int heroH = homeHeroThumbHeight(renderer, UITheme::getInstance().getMetrics().homeCoverHeight);

  auto firstExisting = [](std::initializer_list<std::string> candidates) -> std::string {
    for (const std::string& path : candidates) {
      if (!path.empty() && Storage.exists(path.c_str())) {
        return path;
      }
    }
    return {};
  };

  // Prefer the current theme's hero height (1:1). Bare may briefly open a
  // same-recipe fallback height while c30_560 regenerates after flash.
  std::string coverPath;
  if (FsHelpers::hasEpubExtension(book.path)) {
    Epub epub(book.path, "/.crosspoint");
    if (isBareTheme()) {
      coverPath = firstExisting({
          epub.getThumbBmpPath(heroH),
          epub.getThumbBmpPath(HomeCoverMetrics::thumbHeight),
      });
    } else {
      coverPath = firstExisting({epub.getThumbBmpPath(heroH)});
    }
  }
  if (coverPath.empty() && !book.coverBmpPath.empty()) {
    coverPath = firstExisting({
        UITheme::getCoverThumbPath(book.coverBmpPath, heroH),
        book.coverBmpPath.find("[HEIGHT]") == std::string::npos ? book.coverBmpPath : std::string{},
    });
  }
  if (coverPath.empty()) {
    displayBw(true);
    return;
  }

  HalFile file;
  if (!Storage.openFileForRead("HOME", coverPath, file)) {
    displayBw(true);
    return;
  }

  Bitmap bitmap(file);
  if (bitmap.parseHeaders() != BmpReaderError::Ok || bitmap.getWidth() <= 0 || bitmap.getHeight() <= 0 ||
      !bitmap.hasGreyscale()) {
    file.close();
    // 1-bit or corrupt: single-pass is correct (dither already baked in).
    displayBw(true);
    return;
  }

  // Contain-fit into the theme's coverRect. Prefer 1:1 native blit (no scale).
  const int bw = bitmap.getWidth();
  const int bh = bitmap.getHeight();
  int drawnW = bw;
  int drawnH = bh;
  if (bw > coverRectW || bh > coverRectH) {
    const float widthScale = static_cast<float>(coverRectW) / static_cast<float>(bw);
    const float heightScale = static_cast<float>(coverRectH) / static_cast<float>(bh);
    const float scale = std::min(widthScale, heightScale);
    drawnW = std::max(1, static_cast<int>(std::floor(bw * scale)));
    drawnH = std::max(1, static_cast<int>(std::floor(bh * scale)));
  }
  // Stats + Focus: top-left (stats top == cover top). Bare: center in the slot.
  const int artX = usesStatsFamilyCover() ? coverRectX : coverRectX + (coverRectW - drawnW) / 2;
  const int artY = isBareTheme() ? (coverRectY + (coverRectH - drawnH) / 2) : coverRectY;

  auto drawCoverArt = [&]() {
    bitmap.rewindToData();
    if (drawnW == bw && drawnH == bh) {
      renderer.drawBitmap(bitmap, artX, artY, bw, bh);
    } else {
      renderer.drawBitmap(bitmap, artX, artY, drawnW, drawnH);
    }
  };

  // Free the cover snapshot first so storeBwBuffer (full framebuffer) has room.
  // Bare 420×560 + full BW store was OOMing, then multipass cleared the FB and
  // settled with coverGrayOnPanel — permanent blank home until reboot.
  // X3 is especially tight after leaving the reader — never keep a snapshot here.
  if (coverBuffer) {
    free(coverBuffer);
    coverBuffer = nullptr;
    coverBufferSize = 0;
    coverBufferStored = false;
  }

  if (leavingHome()) {
    file.close();
    coverGrayOnPanel = false;
    return;
  }

  // Hardware order (sleep / reader): base first, then gray planes, then gray refresh.
  // File already open so post-base work is only two row-walks + gray refresh.
  // X3 often needs a second try after leave-reader heap fragmentation.
  bool savedBw = renderer.storeBwBuffer();
  if (!savedBw) {
    delay(40);
    yield();
    savedBw = renderer.storeBwBuffer();
  }
  if (!savedBw) {
    LOG_ERR("HOME", "Cover multipass without BW store (heap); settle BW (no retry thrash)");
    // Do not clearScreen / displayGrayBuffer without a BW restore path — that
    // was the Bare "cover disappeared after flash" bug.
    // One soft-fail: settle on BW so we do not loop OOM→retry→abort.
    file.close();
    renderer.setRenderMode(GfxRenderer::BW);
    if (!leavingHome()) {
      displayBw(true);
    }
    return;
  }

  // Drop multipass as soon as Synopsis/Library/Read is requested (cancelBackgroundPaint).
  // Without these checks, waitForRenderIdle holds the UI frozen for the full grey pass.
  auto abortMultipass = [&]() {
    renderer.setRenderMode(GfxRenderer::BW);
    renderer.restoreBwBuffer();
    renderer.cleanupGrayscaleWithFrameBuffer();
    coverGrayOnPanel = false;
    coverGrayNeedsRetry = false;
    file.close();
  };

  if (leavingHome()) {
    abortMultipass();
    return;
  }

  // v0.1.3 multipass order — stop here. No post-pass paper windows.
  // Soft base: panel already shows a matching BW shell (snappy resume / deferred
  // greys). FAST avoids a multi-second HALF that pins waitForRenderIdle when the
  // user opens Synopsis/Library mid-pass. Leave-reader / hard base still HALF.
  const bool useSoftBase = softGrayscaleBase;
  softGrayscaleBase = false;
  renderer.displayGrayscaleBase(useSoftBase ? HalDisplay::FAST_REFRESH : HalDisplay::HALF_REFRESH);

  if (leavingHome()) {
    abortMultipass();
    return;
  }

  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
  drawCoverArt();
  renderer.copyGrayscaleLsbBuffers();

  if (leavingHome()) {
    abortMultipass();
    return;
  }

  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
  drawCoverArt();
  renderer.copyGrayscaleMsbBuffers();

  if (leavingHome()) {
    abortMultipass();
    return;
  }

  renderer.displayGrayBuffer();
  renderer.setRenderMode(GfxRenderer::BW);

  renderer.restoreBwBuffer();
  // Re-sync controller RAM from BW UI for the next menu / diff paint.
  renderer.cleanupGrayscaleWithFrameBuffer();
  coverGrayOnPanel = true;
  paintedUiTheme = static_cast<int>(SETTINGS.uiTheme);
  coverGrayNeedsRetry = false;
  file.close();
}

void HomeActivity::freeCoverBufferRamOnly() {
  if (coverBuffer) {
    free(coverBuffer);
    coverBuffer = nullptr;
  }
  coverBufferSize = 0;
  coverBufferStored = false;
}

void HomeActivity::freeCoverBuffer() {
  freeCoverBufferRamOnly();
  // Force themes to redraw from SD; do not leave coverRendered=true with no buffer
  // (Bare used to skip draw and show a blank cover hole).
  coverRendered = false;
  coverGrayOnPanel = false;
  paintedUiTheme = -1;
}

void HomeActivity::markLeavingForUiChild() {
  // Only snappy if greys were settled under this theme (panel still looked good).
  leaveForUiChildSnappy = coverGrayOnPanel && paintedUiTheme == static_cast<int>(SETTINGS.uiTheme);
  cancelHomeBackgroundPaint();
}

void HomeActivity::cancelHomeBackgroundPaint() {
  // Drop deferred greys so we do not start another multipass after this.
  coverGrayNeedsRetry = false;
  deferredGreysOnly = false;
  softGrayscaleBase = false;
  // Abort multipass between stages (checked in multipassHomeCoverGrayscale).
  cancelBackgroundPaint = true;
}

bool HomeActivity::handleForcedRefresh() {
  // v0.1.3: invalidate settle and repaint (multipass restores cover greys).
  coverGrayOnPanel = false;
  coverRendered = false;
  forceHomeShellRepaint = true;
  requestUpdate();
  return true;
}

void HomeActivity::loop() {
  // SPECTRAL: when the minute rolls, window-refresh only the time digits (no
  // full-frame flash). Settled home otherwise skips paint forever.
  if (isSpectralTheme() && homeUiReady && coverRendered && !minimalMenuOpen &&
      paintedUiTheme == static_cast<int>(CrossPointSettings::UI_THEME::SPECTRAL) &&
      !forceSPECTRALClockRepaint && !forceStatsUnderBoxRepaint) {
    char now[8];
    if (ClockfaceThemeUi::formatHeroTimeNow(now, sizeof(now)) &&
        (SPECTRALLastDrawnTime[0] == '\0' || strcmp(now, SPECTRALLastDrawnTime) != 0)) {
      forceSPECTRALClockRepaint = true;
      requestUpdate();
      return;
    }
  }

  // Cover gen after first home paint so the Loading box floats over visible UI.
  if (homeUiReady && !recentsLoading) {
    if (coverNeedsRetry && static_cast<long>(millis() - coverRetryAtMs) >= 0) {
      coverNeedsRetry = false;
      // Never schedule a gen/multipass retry once greys are already on the panel —
      // that was a source of random black flashes ~seconds after home settled.
      if (!coverGrayOnPanel) {
        recentsLoaded = false;
      }
    }
    // Deferred greys after snappy resume, or multipass soft-fail retry.
    if (coverGrayNeedsRetry && !coverGrayOnPanel && static_cast<long>(millis() - coverGrayRetryAtMs) >= 0) {
      coverGrayNeedsRetry = false;
      if (deferredGreysOnly && recentsLoaded && coverRectW > 0 && coverRectH > 0 && usesHomeCoverMultipass() &&
          !minimalMenuOpen) {
        // Panel already has the snappy BW home — multipass greys in place (no clearScreen).
        softGrayscaleBase = true;
        requestUpdate(true);
      } else {
        deferredGreysOnly = false;
        softGrayscaleBase = false;
        coverRendered = false;  // force full redraw + multipass (not settled-skip)
        paintedUiTheme = -1;
        requestUpdate();
      }
      return;
    }
    if (!recentsLoaded) {
      // SPECTRAL has no cover art — skip gen / multipass wait entirely.
      if (isSpectralTheme()) {
        recentsLoaded = true;
        coverGrayOnPanel = true;
      } else {
        const auto& metrics = UITheme::getInstance().getMetrics();
        recentsLoading = true;
        loadRecentCovers(metrics.homeCoverHeight);
        return;
      }
    }
  }

  // All minimal homes: Menu · Library · Recents · Read.
  if (usesMinimalHomeInteraction()) {
    const int releasedFrontButton = mappedInput.getReleasedFrontButton();

    // After Settings / book-action menu / long-press: swallow residual edges from
    // the exit gesture. Settings leaves on wasPressed(Back); Home Menu is
    // getReleasedFrontButton(BTN_BACK) — without a full quiet frame, that release
    // re-opens the in-home popup ("Back sent me to the menu").
    if (minimalSuppressInitialFrontRelease) {
      // Drain mapped + raw edges so neither path can fire on the next arm frame.
      (void)mappedInput.wasPressed(MappedInputManager::Button::Back);
      (void)mappedInput.wasReleased(MappedInputManager::Button::Back);
      (void)mappedInput.wasPressed(MappedInputManager::Button::Confirm);
      (void)mappedInput.wasReleased(MappedInputManager::Button::Confirm);
      (void)mappedInput.wasPressed(MappedInputManager::Button::Left);
      (void)mappedInput.wasReleased(MappedInputManager::Button::Left);
      (void)mappedInput.wasPressed(MappedInputManager::Button::Right);
      (void)mappedInput.wasReleased(MappedInputManager::Button::Right);
      // wasPressed/wasReleased(Back) already include the touch back gesture.
      const int pressedFront = mappedInput.getPressedFrontButton();
      // releasedFrontButton already sampled above for this frame.
      const bool busy = isAnyFrontButtonPressed(mappedInput) || mappedInput.isPressed(MappedInputManager::Button::Back) ||
                        mappedInput.isPressed(MappedInputManager::Button::Confirm) ||
                        mappedInput.isPressed(MappedInputManager::Button::Left) ||
                        mappedInput.isPressed(MappedInputManager::Button::Right) ||
                        mappedInput.isPressed(MappedInputManager::Button::Up) ||
                        mappedInput.isPressed(MappedInputManager::Button::Down) || releasedFrontButton >= 0 ||
                        pressedFront >= 0;
      if (busy) {
        return;  // stay suppressed until a fully idle sample
      }
      // Quiet frame: arm normal input starting next loop (do not fall through).
      minimalSuppressInitialFrontRelease = false;
      return;
    }

    if (minimalMenuOpen) {
      // Recents, Stats, OPDS, Transfer, Settings (Settings not on the front bar).
      MinimalMenuItem menuItems[6];
      const int menuCount = buildMinimalMenuItems(menuItems, 6, hasOpdsServers, !recentBooks.empty(),
                                                  /*includeSettings=*/true);
      if (menuCount <= 0) {
        minimalMenuOpen = false;
        requestUpdate();
        return;
      }
      if (minimalMenuIndex >= menuCount) {
        minimalMenuIndex = menuCount - 1;
      }

      buttonNavigator.onPrevious([this, menuCount] {
        minimalMenuIndex = ButtonNavigator::previousIndex(minimalMenuIndex, menuCount);
        requestUpdate();
      });
      buttonNavigator.onNext([this, menuCount] {
        minimalMenuIndex = ButtonNavigator::nextIndex(minimalMenuIndex, menuCount);
        requestUpdate();
      });
      if (mappedInput.wasReleased(MappedInputManager::Button::Back) || releasedFrontButton == HalGPIO::BTN_BACK) {
        minimalMenuOpen = false;
        homeMenuShellOnPanel = false;
        // Snappy FAST BW shell if we still know the settled theme (menu open no
        // longer clears paintedUiTheme). Full multipass only if theme unknown.
        snappyResumeNoGreys =
            usesHomeCoverMultipass() && paintedUiTheme == static_cast<int>(SETTINGS.uiTheme);
        coverGrayOnPanel = false;
        coverRendered = false;
        requestUpdate();
        return;
      }
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
          releasedFrontButton == HalGPIO::BTN_CONFIRM) {
        switch (menuItems[minimalMenuIndex].action) {
          case MinimalMenuAction::ReadingStats: {
            if (recentBooks.empty()) break;
            const RecentBook& book = recentBooks[static_cast<size_t>(focusedRecentIndex())];
            const std::string cachePath = getRecentBookCachePath(book);
            BookReadingStats stats = BookReadingStats::loadForBook(book.path);
            const float progress = stats.getProgressPercent();
            const GlobalReadingStats global = GlobalReadingStats::load();
            const GlobalReadingStats aggregated = GlobalReadingStats::loadAggregated(global);
            markLeavingForUiChild();
            startActivityForResult(
                std::make_unique<BookStatsActivity>(renderer, mappedInput, book.title, cachePath, stats, progress,
                                                    false, 0u, global, aggregated, true),
                [this](const ActivityResult&) {
                  loadFocusedRecentStats();
                  globalStats = GlobalReadingStats::load();
                  coverRendered = false;
                  requestUpdate();
                });
            minimalMenuOpen = false;
            break;
          }
          case MinimalMenuAction::OpdsBrowser:
            onOpdsBrowserOpen();
            break;
          case MinimalMenuAction::FileTransfer:
            onFileTransferOpen();
            break;
          case MinimalMenuAction::Settings:
            minimalMenuOpen = false;
            onSettingsOpen();
            break;
        }
      }
      return;
    }

    // Shelf + Stats Scroll: side Left/Right step recent books (single-press).
    // Invalidate hero snapshot so the new book draws; free after flag so we do
    // not restore a stale cover under the new title/stats.
    if (usesRecentBookSideNav() && !recentBooks.empty()) {
      auto stepRecent = [this](int delta) {
        shiftRecentFocus(delta);
        coverRendered = false;
        coverBufferStored = false;
        freeCoverBuffer();
        requestUpdate();
      };
      buttonNavigator.onPress(ButtonNavigator::getSidePreviousButtons(), [stepRecent] { stepRecent(-1); });
      buttonNavigator.onPress(ButtonNavigator::getSideNextButtons(), [stepRecent] { stepRecent(1); });
    }

    // Stats: side Left/Right toggles under-box title/author ↔ lifetime stats.
    // Front L/R: Recents / Read (all themes).
    if (isStatsTheme() && !usesRecentBookSideNav()) {
      auto flipUnderBox = [this]() {
        FocusThemeUi::showLifeUnderBox() = !FocusThemeUi::showLifeUnderBox();
        forceStatsUnderBoxRepaint = true;
        requestUpdate();
      };
      buttonNavigator.onPress(ButtonNavigator::getSidePreviousButtons(), [flipUnderBox] { flipUnderBox(); });
      buttonNavigator.onPress(ButtonNavigator::getSideNextButtons(), [flipUnderBox] { flipUnderBox(); });
    }
    // Spectral: side buttons remappable (defaults both Panel Scroll).
    // X3 Left/Right · X4 Up/Down via getSidePrevious/Next.
    // Same action on both sides → bidirectional (Left −1 / Right +1).
    // Only one side has the action → one-way cycle (+1): Title→Stats→Lifetime,
    // or recents most-recent→least-recent.
    if (isSpectralTheme()) {
      auto runSpectralSide = [this](const bool isLeft) {
        using A = CrossPointSettings::SPECTRAL_SIDE_ACTION;
        auto clampAct = [](const uint8_t v) -> uint8_t {
          return v < A::SPECTRAL_SIDE_ACTION_COUNT ? v : static_cast<uint8_t>(A::SPECTRAL_SIDE_RECENTS);
        };
        const uint8_t leftA = clampAct(SETTINGS.spectralSideLeft);
        const uint8_t rightA = clampAct(SETTINGS.spectralSideRight);
        const uint8_t action = isLeft ? leftA : rightA;
        const bool bothPanel = leftA == A::SPECTRAL_SIDE_PANEL && rightA == A::SPECTRAL_SIDE_PANEL;
        const bool bothRecents = leftA == A::SPECTRAL_SIDE_RECENTS && rightA == A::SPECTRAL_SIDE_RECENTS;
        // Dual same-action: Left scrolls “back”, Right scrolls “forward”.
        // Solo: always forward (+1).
        int delta = 1;
        if ((action == A::SPECTRAL_SIDE_PANEL && bothPanel) ||
            (action == A::SPECTRAL_SIDE_RECENTS && bothRecents)) {
          delta = isLeft ? -1 : 1;
        }
        if (action == A::SPECTRAL_SIDE_PANEL) {
          if (SETTINGS.readingStatsTrackingEnabled() && ClockfaceThemeUi::cycleUnderMode(delta)) {
            forceStatsUnderBoxRepaint = true;
          }
        } else if (recentBooks.size() > 1) {
          // Recents: +1 older (toward least recent), −1 newer (toward most recent).
          shiftRecentFocus(delta);
          forceStatsUnderBoxRepaint = true;
        }
        forceSPECTRALClockRepaint = true;
        requestUpdate();
      };
      buttonNavigator.onPress(ButtonNavigator::getSidePreviousButtons(),
                              [runSpectralSide] { runSpectralSide(true); });
      buttonNavigator.onPress(ButtonNavigator::getSideNextButtons(),
                              [runSpectralSide] { runSpectralSide(false); });
    }

    // All themes: Menu · Library · Recents · Read
    // (Settings: Menu list / long-press Menu)
    auto activateMinimalHomeNav = [this](int index) {
      switch (index) {
        case 0:  // Menu
          minimalMenuOpen = true;
          minimalMenuIndex = 0;
          requestUpdate();
          break;
        case 1:  // Library
          onFileBrowserOpen();
          break;
        case 2:  // Recents
          onRecentsOpen();
          break;
        case 3:  // Read
          onContinueReading();
          break;
      }
    };

    // Long-press Menu (Back) → Settings. (Library long-press Recents removed —
    // Recents is a front-button shortcut.)
    if (usesBareStyleHomeNav()) {
      if (menuLongPressFired) {
        if (!mappedInput.isFrontButtonPressed(HalGPIO::BTN_BACK)) {
          menuLongPressFired = false;
        }
        return;
      }
      // Wind down multipass while the user is still holding (before threshold).
      if (mappedInput.isFrontButtonPressed(HalGPIO::BTN_BACK) &&
          mappedInput.getHeldTime() >= LONG_PRESS_PRECANCEL_MS) {
        cancelBackgroundPaint = true;
      }
      if (mappedInput.isFrontButtonPressed(HalGPIO::BTN_BACK) && mappedInput.getHeldTime() >= READ_LONG_PRESS_MS) {
        menuLongPressFired = true;
        onSettingsOpen();
        return;
      }
    }

    if (releasedFrontButton == HalGPIO::BTN_BACK) {
      activateMinimalHomeNav(0);
      return;
    }
    if (releasedFrontButton == HalGPIO::BTN_CONFIRM) {
      activateMinimalHomeNav(1);
      return;
    }
    if (releasedFrontButton == HalGPIO::BTN_LEFT) {
      activateMinimalHomeNav(2);
      return;
    }
    // Long-press Read: book action menu (same items/order as Recent Books long-press).
    if (readLongPressFired) {
      if (!mappedInput.isFrontButtonPressed(HalGPIO::BTN_RIGHT)) {
        readLongPressFired = false;
      }
      return;
    }
    if (!recentBooks.empty() && mappedInput.isFrontButtonPressed(HalGPIO::BTN_RIGHT)) {
      const unsigned long held = mappedInput.getHeldTime();
      if (held >= LONG_PRESS_PRECANCEL_MS) {
        cancelBackgroundPaint = true;  // abort greys while still holding
      }
      if (held >= READ_LONG_PRESS_MS) {
        readLongPressFired = true;
        showCurrentBookActionMenu(true);
        return;
      }
    }
    if (releasedFrontButton == HalGPIO::BTN_RIGHT) {
      if (!recentBooks.empty()) {
        activateMinimalHomeNav(3);
      }
      return;
    }

    // Touch: tap cover (or SPECTRAL book block) to continue.
    const auto& metrics = UITheme::getInstance().getMetrics();
    if (!recentBooks.empty()) {
      if (isSpectralTheme()) {
        const int pageH = renderer.getScreenHeight();
        const int contentTop = metrics.homeTopPadding;
        const int contentBottom = pageH - metrics.buttonHintsHeight;
        // Center band of the content area (title / now-reading block).
        const int bandH = contentBottom - contentTop;
        const int tapTop = contentTop + bandH / 4;
        const int tapH = bandH / 2;
        if (mappedInput.wasTapInRect(0, tapTop, renderer.getScreenWidth(), tapH)) {
          onContinueReading();
        }
      } else if (mappedInput.wasTapInRect(0, metrics.homeTopPadding, renderer.getScreenWidth(),
                                          metrics.homeCoverTileHeight)) {
        onContinueReading();
      }
    }
    return;
  }

  const int menuCount = getMenuItemCount();
  const auto& metrics = UITheme::getInstance().getMetrics();

  auto activateSelection = [this] {
    if (selectorIndex < recentBooks.size()) {
      onSelectBook(recentBooks[selectorIndex].path);
      return;
    }
    const int menuIndex = selectorIndex - static_cast<int>(recentBooks.size());
    switch (indexToMenuItem(menuIndex, hasOpdsServers)) {
      case HomeMenuItem::FILE_BROWSER:
        onFileBrowserOpen();
        break;
      case HomeMenuItem::RECENTS:
        onRecentsOpen();
        break;
      case HomeMenuItem::OPDS_BROWSER:
        onOpdsBrowserOpen();
        break;
      case HomeMenuItem::FILE_TRANSFER:
        onFileTransferOpen();
        break;
      case HomeMenuItem::SETTINGS_MENU:
        onSettingsOpen();
        break;
      default:
        break;
    }
  };

  buttonNavigator.onNext([this, menuCount] {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, menuCount);
    requestUpdate();
  });

  buttonNavigator.onPrevious([this, menuCount] {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, menuCount);
    requestUpdate();
  });

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up) {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, menuCount);
    requestUpdate();
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Down) {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, menuCount);
    requestUpdate();
    return;
  }

  // Classic themes: Back opens most recent book. Minimal/Dashboard already returned above.
  // Arm only after Back is idle with no edges so the press that left the reader
  // (or a same-frame touch back-gesture) cannot re-open the book immediately.
  if (!backResumeArmed) {
    const bool backBusy = mappedInput.isPressed(MappedInputManager::Button::Back) ||
                          mappedInput.wasPressed(MappedInputManager::Button::Back) ||
                          mappedInput.wasReleased(MappedInputManager::Button::Back);
    if (!backBusy) {
      backResumeArmed = true;
    }
  } else {
    // wasBackGesture reports both wasPressed and wasReleased true in one frame.
    // Treat that as a complete Resume gesture once armed; for physical buttons
    // require a press that began on Home, then a release.
    const bool backPressed = mappedInput.wasPressed(MappedInputManager::Button::Back);
    const bool backReleased = mappedInput.wasReleased(MappedInputManager::Button::Back);
    if (backPressed && backReleased) {
      if (!recentBooks.empty()) {
        onContinueReading();
        return;
      }
    } else if (backPressed) {
      backPressSeen = true;
    } else if (backReleased && backPressSeen && !recentBooks.empty()) {
      onContinueReading();
      return;
    }
  }

  int tx = 0;
  int ty = 0;
  if (!recentBooks.empty() && mappedInput.wasScreenTouchDown(tx, ty) && tx >= 0 && tx < renderer.getScreenWidth() &&
      ty >= metrics.homeTopPadding && ty < metrics.homeTopPadding + metrics.homeCoverTileHeight) {
    if (selectorIndex != 0) {
      selectorIndex = 0;
      requestUpdate();
    }
    return;
  }

  if (!recentBooks.empty() &&
      mappedInput.wasTapInRect(0, metrics.homeTopPadding, renderer.getScreenWidth(), metrics.homeCoverTileHeight)) {
    selectorIndex = 0;
    activateSelection();
    return;
  }

  const int menuTop = metrics.homeTopPadding + metrics.homeCoverTileHeight + metrics.homeMenuTopOffset;
  const int renderedMenuSelection =
      metrics.homeContinueReadingInMenu ? selectorIndex : selectorIndex - recentBooks.size();
  const int renderedMenuCount =
      menuCount - (metrics.homeContinueReadingInMenu ? 0 : static_cast<int>(recentBooks.size()));
  int menuRow = -1;
  const auto menuTouch = mappedInput.rowTouch(menuRow, menuTop, metrics.menuRowHeight + metrics.menuSpacing,
                                              renderedMenuCount, 0, INT32_MAX, metrics.menuRowHeight);
  if (menuTouch != MappedInputManager::RowTouch::None) {
    const int touchedIndex =
        metrics.homeContinueReadingInMenu ? menuRow : menuRow + static_cast<int>(recentBooks.size());
    if (menuTouch == MappedInputManager::RowTouch::Down) {
      if (selectorIndex != touchedIndex) {
        selectorIndex = touchedIndex;
        requestUpdate();
      }
    } else {
      selectorIndex = touchedIndex;
      activateSelection();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateSelection();
  }
}

void HomeActivity::render(RenderLock&& lock) {
  // While covers generate, keep the painted home UI under the floating Loading box.
  if (recentsLoading) {
    return;
  }

  // Never leave reader strip-target or greyscale multipass mode active — that
  // makes clearScreen only wipe a strip and 2-bit text paint grey midtones.
  renderer.endStripTarget();
  renderer.setRenderMode(GfxRenderer::BW);

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  // ---------------------------------------------------------------------------
  // SPECTRAL: fully isolated text-only path. No cover multipass, no settle-skip
  // over a dirty panel, no shared Stats/Bare paint tail. Boot is clean; this must
  // match — black ink on native white only.
  // ---------------------------------------------------------------------------
  if (isSpectralTheme()) {
    if (paintedUiTheme >= 0 && paintedUiTheme != static_cast<int>(SETTINGS.uiTheme)) {
      freeCoverBufferRamOnly();
      FocusThemeUi::showLifeUnderBox() = false;
      ClockfaceThemeUi::underMode() = ClockfaceThemeUi::UnderMode::TitleAuthor;
    }
    // Kill any deferred cover greys from a previous theme.
    deferredGreysOnly = false;
    coverGrayNeedsRetry = false;
    softGrayscaleBase = false;
    snappyResumeNoGreys = false;
    coverRectX = coverRectY = coverRectW = coverRectH = 0;
    freeCoverBufferRamOnly();

    const int clockTheme = static_cast<int>(CrossPointSettings::UI_THEME::SPECTRAL);

    // Partial updates (minute tick / side L/R): rebuild full BW frame and
    // displayWindow the whole panel. A tight clock-only window drives pure white
    // only in the digit rect and leaves a visible "white box" vs greyer residual
    // paper elsewhere; full-frame PTL matches that clean white everywhere.
    const bool clockDirty = forceSPECTRALClockRepaint;
    const bool underDirty = forceStatsUnderBoxRepaint;
    forceSPECTRALClockRepaint = false;
    forceStatsUnderBoxRepaint = false;
    if ((clockDirty || underDirty) && !minimalMenuOpen && paintedUiTheme == clockTheme && coverRendered) {
      renderer.clearScreen(0xFF);
      const bool chromeOnly =
          SETTINGS.systemStatusBarHas(CrossPointSettings::SYS_SLOT_BATTERY) ||
          SETTINGS.systemStatusBarHas(CrossPointSettings::SYS_SLOT_CLOCK);
      if (chromeOnly) {
        const int headerH =
            BaseTheme::kTopChromeBatteryY + std::max(metrics.batteryHeight + 8, metrics.statusBarVerticalMargin);
        GUI.drawHeader(renderer, Rect{0, 0, pageWidth, headerH}, nullptr);
      }
      {
        bool cr = false;
        bool cbs = false;
        bool br = false;
        auto noStore = [](int, int, int, int) -> bool { return false; };
        GUI.drawRecentBookCover(renderer, Rect{0, 0, pageWidth, pageHeight}, recentBooks, selectorIndex, cr, cbs, br,
                                noStore, &currentBookStats, currentBookProgressPercent, &globalStats, nullptr);
      }
      GUI.drawButtonHints(renderer, tr(STR_MENU), tr(STR_LIBRARY), tr(STR_RECENTS),
                          recentBooks.empty() ? "" : tr(STR_READ));
      char drawn[8];
      if (ClockfaceThemeUi::formatHeroTimeNow(drawn, sizeof(drawn)) && drawn[0] != '\0') {
        snprintf(SPECTRALLastDrawnTime, sizeof(SPECTRALLastDrawnTime), "%s", drawn);
      }
      renderer.displayWindow(0, 0, pageWidth, pageHeight);
      homeUiReady = true;
      recentsLoaded = true;
      return;
    }

    // First land / leave another theme / menu dismiss: always repaint.
    // Same-theme idle: still allow settle skip only if we already painted clean
    // SPECTRAL (not a multipass theme's panel).
    const bool needPaint =
        forceHomeShellRepaint || minimalMenuOpen || paintedUiTheme != clockTheme || !coverRendered;
    forceHomeShellRepaint = false;

    if (!needPaint && !minimalMenuOpen) {
      homeUiReady = true;
      recentsLoaded = true;
      return;
    }

    if (minimalMenuOpen) {
      const int bandTop = metrics.topPadding + metrics.homeTopPadding;
      const int bandBottom = pageHeight - metrics.buttonHintsHeight;
      MinimalMenuItem menuItems[6];
      const int menuCount = buildMinimalMenuItems(menuItems, 6, hasOpdsServers, !recentBooks.empty(),
                                                  /*includeSettings=*/true);
      const int menuH =
          menuCount > 0 ? menuCount * metrics.menuRowHeight + (menuCount - 1) * metrics.menuSpacing : 0;
      const int menuTop = bandTop + std::max(0, (bandBottom - bandTop - menuH) / 2);
      if (homeMenuShellOnPanel) {
        if (bandBottom > bandTop) {
          renderer.fillRect(0, bandTop, pageWidth, bandBottom - bandTop, false);
        }
        GUI.drawButtonMenu(renderer, Rect{0, menuTop, pageWidth, std::max(menuH, 1)}, menuCount, minimalMenuIndex,
                           [&menuItems](int index) { return std::string(menuItems[index].label); },
                           [&menuItems](int index) { return menuItems[index].icon; });
        renderer.displayWindow(0, bandTop, pageWidth, std::max(1, bandBottom - bandTop));
        homeUiReady = true;
        recentsLoaded = true;
        return;
      }
      renderer.clearScreen(0xFF);
      GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.homeTopPadding}, nullptr);
      GUI.drawButtonMenu(renderer, Rect{0, menuTop, pageWidth, std::max(menuH, 1)}, menuCount, minimalMenuIndex,
                         [&menuItems](int index) { return std::string(menuItems[index].label); },
                         [&menuItems](int index) { return menuItems[index].icon; });
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      coverGrayOnPanel = false;
      coverRendered = false;
      homeMenuShellOnPanel = true;
      paintedUiTheme = clockTheme;
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      homeUiReady = true;
      recentsLoaded = true;
      return;
    }

    homeMenuShellOnPanel = false;
    renderer.clearScreen(0xFF);
    const bool chromeOnly =
        SETTINGS.systemStatusBarHas(CrossPointSettings::SYS_SLOT_BATTERY) ||
        SETTINGS.systemStatusBarHas(CrossPointSettings::SYS_SLOT_CLOCK);
    if (chromeOnly) {
      const int headerH =
          BaseTheme::kTopChromeBatteryY + std::max(metrics.batteryHeight + 8, metrics.statusBarVerticalMargin);
      GUI.drawHeader(renderer, Rect{0, 0, pageWidth, headerH}, nullptr);
    }
    {
      bool cr = false;
      bool cbs = false;
      bool br = false;
      auto noStore = [](int, int, int, int) -> bool { return false; };
      // Pass book + lifetime stats so under-panel modes can paint without SD I/O.
      GUI.drawRecentBookCover(renderer, Rect{0, 0, pageWidth, pageHeight}, recentBooks, selectorIndex, cr, cbs, br,
                              noStore, &currentBookStats, currentBookProgressPercent, &globalStats, nullptr);
      coverRendered = true;
      coverBufferStored = false;
    }
    GUI.drawButtonHints(renderer, tr(STR_MENU), tr(STR_LIBRARY), tr(STR_RECENTS),
                        recentBooks.empty() ? "" : tr(STR_READ));

    // X3 HALF requestResync → absolute white-baseline full + settle (stronger than
    // plain FULL). One shot — no double white flash.
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    coverGrayOnPanel = true;
    paintedUiTheme = clockTheme;
    recentsLoaded = true;
    homeUiReady = true;
    // Seed minute-change detector so the next loop does not immediately re-window.
    ClockfaceThemeUi::formatHeroTimeNow(SPECTRALLastDrawnTime, sizeof(SPECTRALLastDrawnTime));
    forceSPECTRALClockRepaint = false;
    return;
  }

  // Dashboard / Minimal / Focus: full-bleed cover + bottom hints (no classic bottom menu list).
  if (usesMinimalHomeInteraction()) {
    // Theme switch while Home was stacked (Settings): invalidate settle so greys re-run.
    // Without this, Stats can keep a BW-only panel (super-dark midtones).
    if (paintedUiTheme >= 0 && paintedUiTheme != static_cast<int>(SETTINGS.uiTheme)) {
      freeCoverBufferRamOnly();
      coverGrayOnPanel = false;
      coverRendered = false;
      paintedUiTheme = -1;
      FocusThemeUi::showLifeUnderBox() = false;
      ClockfaceThemeUi::underMode() = ClockfaceThemeUi::UnderMode::TitleAuthor;
    }

    // Stats under-box title ↔ lifetime (side Up/Down on X4, Left/Right on X3).
    // X3: windowed FAST keeps multipass greys on the jacket (PTL).
    // X4 (SSD1677): displayWindow after greys can promote to a full HALF clean and
    // invert/stick the panel. Push a full FAST BW frame (under-box already in FB),
    // then remultipass greys so the jacket recovers — same end state as X3.
    if (forceStatsUnderBoxRepaint && isStatsTheme() && !minimalMenuOpen && recentsLoaded && coverRendered &&
        paintedUiTheme == static_cast<int>(SETTINGS.uiTheme)) {
      forceStatsUnderBoxRepaint = false;
      const Rect dirty = FocusThemeUi::redrawUnderBox(renderer, recentBooks, &globalStats);
      homeUiReady = true;
      if (gpio.deviceIsX4()) {
        freeCoverBufferRamOnly();
        coverGrayOnPanel = false;
        lock.unlock();
        if (activityManager.hasPendingActivityChange() || !activityManager.isCurrentActivity(this)) {
          return;
        }
        // Full BW shell with the new under-box text (not inverted partial).
        renderer.displayBuffer(HalDisplay::FAST_REFRESH);
        if (coverRectW > 0 && coverRectH > 0 && usesHomeCoverMultipass() &&
            !activityManager.hasPendingActivityChange() && activityManager.isCurrentActivity(this)) {
          multipassHomeCoverGrayscale();
        }
        return;
      }
      if (dirty.height > 0 && dirty.width > 0) {
        renderer.displayWindow(dirty.x, dirty.y, dirty.width, dirty.height);
      }
      return;
    }
    forceStatsUnderBoxRepaint = false;

    // Settled gray cover on panel: ignore spurious requestUpdate (USB detect bounce,
    // background cover-gen probes, etc.). Re-running multipass is a full black flash.
    // Do NOT require coverBufferStored — multipass frees the snapshot to make room for
    // storeBwBuffer, so requiring it made settle skip dead after every successful grey pass.
    // Intentional invalidation (onExit freeCover, menu open, new thumb) clears
    // coverGrayOnPanel so the next paint multipasses once.
    // forceHomeShellRepaint: child menu dismissed — must repaint even if settle flags
    // look good (otherwise the menu FB stays on glass while Home still takes input).
    if (!forceHomeShellRepaint && recentsLoaded && coverGrayOnPanel && coverRendered && !minimalMenuOpen &&
        paintedUiTheme == static_cast<int>(SETTINGS.uiTheme) && !deferredGreysOnly) {
      homeUiReady = true;
      return;
    }
    forceHomeShellRepaint = false;

    // Deferred greys only: FB already holds the snappy BW home — multipass without redraw.
    if (deferredGreysOnly && !minimalMenuOpen && recentsLoaded && coverRectW > 0 && coverRectH > 0 &&
        usesHomeCoverMultipass()) {
      deferredGreysOnly = false;
      homeUiReady = true;
      lock.unlock();
      if (activityManager.hasPendingActivityChange() || !activityManager.isCurrentActivity(this)) {
        return;
      }
      multipassHomeCoverGrayscale();
      return;
    }
    deferredGreysOnly = false;

    if (minimalMenuOpen) {
      MinimalMenuItem menuItems[6];
      const int menuCount = buildMinimalMenuItems(menuItems, 6, hasOpdsServers, !recentBooks.empty(),
                                                  /*includeSettings=*/true);
      // Vertically center the item stack between header band and footer hints.
      const int bandTop = metrics.topPadding + metrics.homeTopPadding;
      const int bandBottom = pageHeight - metrics.buttonHintsHeight;
      const int menuH =
          menuCount > 0
              ? menuCount * metrics.menuRowHeight + (menuCount - 1) * metrics.menuSpacing
              : 0;
      const int menuTop = bandTop + std::max(0, (bandBottom - bandTop - menuH) / 2);
      const Rect menuRect{0, menuTop, pageWidth, std::max(menuH, 1)};

      // Cursor move while menu already on glass: only repaint the menu stack.
      // (Long-press Read is a separate activity — never hits this path.)
      if (homeMenuShellOnPanel) {
        if (bandBottom > bandTop) {
          renderer.fillRect(0, bandTop, pageWidth, bandBottom - bandTop, false);
        }
        GUI.drawButtonMenu(renderer, menuRect, menuCount, minimalMenuIndex,
                           [&menuItems](int index) { return std::string(menuItems[index].label); },
                           [&menuItems](int index) { return menuItems[index].icon; });
        renderer.displayWindow(0, bandTop, pageWidth, std::max(1, bandBottom - bandTop));
        homeUiReady = true;
        return;
      }

      renderer.clearScreen(0xFF);
      GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.homeTopPadding}, nullptr);
      GUI.drawButtonMenu(renderer, menuRect, menuCount, minimalMenuIndex,
                         [&menuItems](int index) { return std::string(menuItems[index].label); },
                         [&menuItems](int index) { return menuItems[index].icon; });
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      // BW menu overwrites greys on glass. Keep paintedUiTheme so Back can snappy-
      // return with FAST shell + deferred greys (not a full multipass).
      coverGrayOnPanel = false;
      homeMenuShellOnPanel = true;
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      homeUiReady = true;
      return;
    }

    // Clear first: cover snapshot is cover-art only, not the full tile, so we
    // cannot rely on a large restore to erase the previous frame (e.g. Menu).
    renderer.clearScreen();
    // SPECTRAL never restores a cover snapshot (text-only home).
    bool bufferRestored = false;
    if (!isSpectralTheme() && coverBufferStored) {
      bufferRestored = restoreCoverBuffer();
    }

    // Top chrome (battery icon+% / clock). Bare / SPECTRAL default chrome off;
    // Stats always draws the status-bar band (same plate packing on X3 + X4).
    const bool textOnlyHome = isBareTheme() || isSpectralTheme();
    const bool chromeOnlyMinimal =
        textOnlyHome &&
        (SETTINGS.systemStatusBarHas(CrossPointSettings::SYS_SLOT_BATTERY) ||
         SETTINGS.systemStatusBarHas(CrossPointSettings::SYS_SLOT_CLOCK));
    // Bare / SPECTRAL: header only when the user has enabled battery/clock.
    if (!textOnlyHome || chromeOnlyMinimal) {
      const int headerTop = textOnlyHome ? 0 : metrics.topPadding;
      const int headerH =
          BaseTheme::kTopChromeBatteryY + std::max(metrics.batteryHeight + 8, metrics.statusBarVerticalMargin);
      GUI.drawHeader(renderer, Rect{0, headerTop, pageWidth, headerH}, nullptr);
    }

    // When greys will run next, only record the art box — do not malloc a cover
    // snapshot. X3 leave-reader multipass OOMs if ~15–30KB is still held for storeBw.
    // SPECTRAL has no cover multipass.
    const bool willMultipass = usesHomeCoverMultipass() && recentsLoaded && !coverGrayOnPanel &&
                               !minimalMenuOpen && !snappyResumeNoGreys;
    auto storeCover = [this, willMultipass](int x, int y, int w, int h) -> bool {
      if (isSpectralTheme()) {
        return false;
      }
      coverRectX = x;
      coverRectY = y;
      coverRectW = w;
      coverRectH = h;
      if (willMultipass) {
        if (coverBuffer) {
          free(coverBuffer);
          coverBuffer = nullptr;
          coverBufferSize = 0;
        }
        coverBufferStored = false;
        return false;
      }
      return storeCoverBuffer();
    };

    // Rect is a full-screen hint; themes pack cover/title (or SPECTRAL) in that band.
    GUI.drawRecentBookCover(renderer, Rect{0, 0, pageWidth, pageHeight}, recentBooks, selectorIndex, coverRendered,
                            coverBufferStored, bufferRestored, storeCover, &currentBookStats,
                            currentBookProgressPercent, &globalStats, nullptr);

    // Consistent on Bare / Spectral / Stats (and both X3 + X4).
    GUI.drawButtonHints(renderer, tr(STR_MENU), tr(STR_LIBRARY), tr(STR_RECENTS),
                        recentBooks.empty() ? "" : tr(STR_READ));

    // Release the render mutex before long e-ink ops so the main loop can push
    // Synopsis/Library/Read without blocking for the full multipass (10–15s freeze).
    // Replace/pop wait for renderInProgress so Home is not destroyed mid-multipass.
    homeUiReady = true;
    lock.unlock();
    if (activityManager.hasPendingActivityChange() || !activityManager.isCurrentActivity(this)) {
      // Still push the BW shell so the panel matches chrome; greys run on the next
      // paint if we stay on Home (never leave a stale Settings frame + dark cover).
      coverGrayOnPanel = false;
      paintedUiTheme = -1;
      homeMenuShellOnPanel = false;
      // FAST: X3 HALF would requestResync (full black GC) and feel sluggish.
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      return;
    }
    // Snappy return from Settings/Library/in-home Menu: one FAST BW shell now
    // (no multipass, no X3 HALF resync), then deferred greys after idle.
    if (snappyResumeNoGreys && recentsLoaded) {
      snappyResumeNoGreys = false;
      homeMenuShellOnPanel = false;
      coverRendered = true;
      // Not settled greys yet — midtones are BW-solid until deferred multipass.
      coverGrayOnPanel = false;
      paintedUiTheme = static_cast<int>(SETTINGS.uiTheme);
      // Free any snapshot so storeBw has room for deferred greys (keep coverRect*).
      freeCoverBufferRamOnly();
      deferredGreysOnly = true;
      softGrayscaleBase = true;
      coverGrayNeedsRetry = true;
      // Long enough that Back→home feels done before the grey multipass starts.
      coverGrayRetryAtMs = millis() + (gpio.deviceIsX3() ? 1600UL : 900UL);
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      LOG_DBG("HOME", "Snappy home refresh (FAST); greys deferred");
      return;
    }
    snappyResumeNoGreys = false;
    // Thumbs not ready yet: BW shell first so Loading can float over real chrome.
    // When bindExistingHeroThumbsIfReady already set recentsLoaded (A1), multipass now.
    if (!recentsLoaded) {
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
      // Do not set coverGrayOnPanel — multipass has not run yet.
    } else {
      multipassHomeCoverGrayscale();
    }
    return;
  }

  // Phase 1 (A5): classic list themes — same settled-skip as multipass homes.
  // Spurious requestUpdate must not re-flash greys once the panel is good.
  if (paintedUiTheme >= 0 && paintedUiTheme != static_cast<int>(SETTINGS.uiTheme)) {
    coverGrayOnPanel = false;
    coverRendered = false;
    paintedUiTheme = -1;
  }
  if (recentsLoaded && coverGrayOnPanel && coverRendered &&
      paintedUiTheme == static_cast<int>(SETTINGS.uiTheme)) {
    homeUiReady = true;
    return;
  }

  renderer.clearScreen();
  bool bufferRestored = coverBufferStored && restoreCoverBuffer();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.homeTopPadding},
                 metrics.homeContinueReadingInMenu && !recentBooks.empty() ? recentBooks[0].title.c_str() : nullptr);

  const bool willMultipassClassic = recentsLoaded && !coverGrayOnPanel;
  auto storeCover = [this, willMultipassClassic](int x, int y, int w, int h) -> bool {
    coverRectX = x;
    coverRectY = y;
    coverRectW = w;
    coverRectH = h;
    if (willMultipassClassic) {
      if (coverBuffer) {
        free(coverBuffer);
        coverBuffer = nullptr;
        coverBufferSize = 0;
      }
      coverBufferStored = false;
      return false;
    }
    return storeCoverBuffer();
  };

  GUI.drawRecentBookCover(renderer, Rect{0, metrics.homeTopPadding, pageWidth, metrics.homeCoverTileHeight},
                          recentBooks, selectorIndex, coverRendered, coverBufferStored, bufferRestored, storeCover,
                          &currentBookStats, currentBookProgressPercent, &globalStats, nullptr);

  // Build menu items dynamically
  std::vector<const char*> menuItems = {tr(STR_BROWSE_FILES), tr(STR_MENU_RECENT_BOOKS), tr(STR_FILE_TRANSFER),
                                        tr(STR_SETTINGS_TITLE)};
  std::vector<UIIcon> menuIcons = {Folder, Recent, Transfer, Settings};

  if (hasOpdsServers) {
    menuItems.insert(menuItems.begin() + 2, tr(STR_OPDS_BROWSER));
    menuIcons.insert(menuIcons.begin() + 2, Library);
  }

  if (metrics.homeContinueReadingInMenu && !recentBooks.empty()) {
    // Insert Continue Reading at the top if enabled in theme
    menuItems.insert(menuItems.begin(), tr(STR_CONTINUE_READING));
    menuIcons.insert(menuIcons.begin(), Book);
  }

  GUI.drawButtonMenu(
      renderer,
      Rect{0, metrics.homeTopPadding + metrics.homeCoverTileHeight + metrics.homeMenuTopOffset, pageWidth,
           pageHeight - (metrics.headerHeight + metrics.homeTopPadding + metrics.verticalSpacing +
                         metrics.homeMenuTopOffset + metrics.buttonHintsHeight)},
      static_cast<int>(menuItems.size()),
      metrics.homeContinueReadingInMenu ? selectorIndex : selectorIndex - recentBooks.size(),
      [&menuItems](int index) { return std::string(menuItems[index]); },
      [&menuIcons](int index) { return menuIcons[index]; });

  const auto labels = mappedInput.mapLabels(recentBooks.empty() ? "" : tr(STR_RESUME), tr(STR_SELECT), tr(STR_DIR_UP),
                                            tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  homeUiReady = true;
  lock.unlock();
  if (activityManager.hasPendingActivityChange() || !activityManager.isCurrentActivity(this)) {
    coverGrayOnPanel = false;
    paintedUiTheme = -1;
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    return;
  }
  multipassHomeCoverGrayscale();
}

void HomeActivity::onSelectBook(const std::string& path) {
  // Returning from reader should multipass greys again (full quality), not snappy BW.
  leaveForUiChildSnappy = false;
  snappyResumeNoGreys = false;
  // Abort home multipass so waitForRenderIdle is not a full grey pass.
  // Loading popup: only spend an e-ink cycle when greys were still running (user
  // would otherwise stare at a frozen multipass). When greys already settled, skip
  // the Loading refresh — it adds ~0.5–1s and makes "Loading" feel like most of
  // the open; first page paint is the next panel update.
  const bool greysSettled = coverGrayOnPanel;
  markLeavingForUiChild();
  if (!greysSettled) {
    GUI.drawPopup(renderer, tr(STR_LOADING_POPUP), BaseTheme::kPopupCenterY, /*refresh=*/true);
  }
  const uint32_t t0 = millis();
  activityManager.waitForRenderIdle();
  LOG_DBG("HOME", "Read open: waitIdle %lums greysSettled=%d", static_cast<unsigned long>(millis() - t0),
          greysSettled ? 1 : 0);
  cancelBackgroundPaint = false;
  // Snappy open: FAST first page when greys already clean; always BW-first then AA.
  // (Consumed by EpubReaderActivity via ReaderActivity open hints.)
  ReaderActivity::setOpenHints(/*preferFastFirstRefresh=*/greysSettled, /*deferFirstPageTextAa=*/true);
  activityManager.goToReader(path);
}

void HomeActivity::reloadHomeAfterBookAction() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  loadRecentBooks(metrics.homeRecentBooksCount);
  if (selectorIndex >= static_cast<int>(recentBooks.size())) {
    selectorIndex = std::max(0, static_cast<int>(recentBooks.size()) - 1);
  }
  loadFocusedRecentStats();
  globalStats = GlobalReadingStats::load();
  freeCoverBuffer();
  coverRendered = false;
  coverBufferStored = false;
  coverGrayOnPanel = false;
  // Book/cache actions may have deleted thumbs — allow a fresh gen pass.
  recentsLoaded = false;
  recentsLoading = false;
  homeUiReady = true;  // UI already visible; gen may float Loading again
  coverNeedsRetry = false;
  coverGenAttempts = 0;
  coverRetryAtMs = 0;
  requestUpdate();
}

int HomeActivity::focusedRecentIndex() const {
  if (recentBooks.empty()) return 0;
  return std::clamp(selectorIndex, 0, static_cast<int>(recentBooks.size()) - 1);
}

void HomeActivity::loadFocusedRecentStats() {
  if (recentBooks.empty()) {
    currentBookStats = BookReadingStats{};
    currentBookProgressPercent = -1.0f;
    return;
  }
  const auto& book = recentBooks[static_cast<size_t>(focusedRecentIndex())];
  currentBookStats = loadRecentBookStats(book);
  currentBookProgressPercent = currentBookStats.getProgressPercent();
}

void HomeActivity::shiftRecentFocus(const int delta) {
  if (recentBooks.empty()) return;
  const int n = static_cast<int>(recentBooks.size());
  // Wrap within the loaded recents window (SPECTRAL: at most 4).
  selectorIndex = (focusedRecentIndex() + delta % n + n) % n;
  loadFocusedRecentStats();
}

void HomeActivity::showCurrentBookActionMenu(const bool ignoreInitialConfirmRelease) {
  if (recentBooks.empty()) {
    return;
  }
  // Cancel greys first so ActivityManager waitForRenderIdle is not a multi-second stall.
  const uint32_t tMenu = millis();
  markLeavingForUiChild();
  cancelBackgroundPaint = true;
  activityManager.waitForRenderIdle();
  LOG_DBG("HOME", "long-press menu after waitIdle %lums greysSettled=%d",
          static_cast<unsigned long>(millis() - tMenu), coverGrayOnPanel ? 1 : 0);
  const RecentBook book = recentBooks[static_cast<size_t>(focusedRecentIndex())];
  // Menu handles side-effect actions itself and stays open. Parent only sees
  // Open / Delete / RemoveFromRecents / cancel — so Back returns to dashboard.
  startActivityForResult(
      std::make_unique<FileBrowserActionActivity>(renderer, mappedInput, book.title, book.path,
                                                  /*includeRemoveFromRecents=*/true, ignoreInitialConfirmRelease),
      [this, book](const ActivityResult& result) {
        // Popping back does not re-run onEnter. The Confirm (or Back) release that
        // closed the action/confirmation dialog is still latched and would fire
        // Library / Menu on the next home loop — swallow that residual edge.
        minimalSuppressInitialFrontRelease = true;
        readLongPressFired = false;

        // Handler runs BEFORE onResume. Do NOT freeCoverBuffer() here — that
        // zeroes paintedUiTheme so onResume cannot snappy-repaint; settle then
        // skips paint and the menu stays on glass while Home still takes input.
        auto softRepaintHome = [this]() {
          freeCoverBufferRamOnly();
          coverGrayOnPanel = false;
          coverRendered = false;
          deferredGreysOnly = false;
          forceHomeShellRepaint = true;  // never settle-skip over the menu FB
          // Keep paintedUiTheme so onResume snappy HALF can run.
          recentsLoading = false;
          coverNeedsRetry = false;
          requestUpdate();
        };

        if (result.isCancelled) {
          softRepaintHome();
          return;
        }
        const auto* actionResult = std::get_if<FileBrowserActionResult>(&result.data);
        if (!actionResult) {
          softRepaintHome();
          return;
        }
        switch (static_cast<FileBrowserAction>(actionResult->action)) {
          case FileBrowserAction::Open:
            onSelectBook(book.path);
            return;
          case FileBrowserAction::Delete:
          case FileBrowserAction::RemoveFromRecents:
            // Book list / cache changed — full reload + gen pass.
            reloadHomeAfterBookAction();
            return;
          default:
            // Side-effect actions (e.g. clear cache) ran inside the menu; thumbs
            // may be gone — force a gen re-scan on the next loop.
            freeCoverBuffer();
            recentsLoaded = false;
            recentsLoading = false;
            coverNeedsRetry = false;
            coverGenAttempts = 0;
            coverRetryAtMs = 0;
            requestUpdate();
            return;
        }
      });
}

void HomeActivity::onContinueReading() {
  if (!recentBooks.empty()) {
    onSelectBook(recentBooks[static_cast<size_t>(focusedRecentIndex())].path);
  }
}

void HomeActivity::onFileBrowserOpen() {
  markLeavingForUiChild();
  activityManager.waitForRenderIdle();
  cancelBackgroundPaint = false;
  activityManager.goToFileBrowser();
}

void HomeActivity::onRecentsOpen() {
  markLeavingForUiChild();
  activityManager.waitForRenderIdle();
  cancelBackgroundPaint = false;
  activityManager.goToRecentBooks();
}

void HomeActivity::onSettingsOpen() {
  markLeavingForUiChild();
  // Abort greys so Settings is not delayed behind a multipass wait.
  activityManager.waitForRenderIdle();
  cancelBackgroundPaint = false;
  activityManager.goToSettings();
}

void HomeActivity::onFileTransferOpen() {
  markLeavingForUiChild();
  activityManager.goToFileTransfer();
}

void HomeActivity::onOpdsBrowserOpen() {
  markLeavingForUiChild();
  activityManager.goToBrowser();
}
