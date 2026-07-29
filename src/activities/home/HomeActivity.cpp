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
#include <cstring>
#include <functional>
#include <vector>

#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalStorage.h>

#include "BookActions.h"
#include "BookDescriptionActivity.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "FileBrowserActionActivity.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "RecentBooksStore.h"
#include "activities/reader/BookStatsActivity.h"
#include "activities/reader/GlobalReadingStats.h"
#include "components/UITheme.h"
#include "components/themes/BaseTheme.h"
#include "components/themes/HomeCoverMetrics.h"
#include "components/themes/bare/BareTheme.h"
#include "components/themes/dashboard/DashboardTheme.h"
#include "components/themes/focus/FocusTheme.h"
#include "fontIds.h"

namespace {

// Match RecentBooksActivity long-press threshold.
// Half of the prior 1s menu hold — same value used for Bare Library and Read.
constexpr unsigned long READ_LONG_PRESS_MS = 500;

// Stats-Life home (+ legacy ids remapped on load). Bare / Stats are separate.
bool isStatsLifeTheme() {
  using T = CrossPointSettings::UI_THEME;
  const auto theme = static_cast<T>(SETTINGS.uiTheme);
  return theme == T::STATS_LIFE || theme == T::DASHBOARD_RECENTS || theme == T::DASHBOARD_SCROLL ||
         theme == T::DASHBOARD_MAGAZINE || theme == T::DASHBOARD_CARD || theme == T::MINIMAL ||
         theme == T::LYRA_CAROUSEL;
}

// Shelf / Stats Scroll parked (remapped → STATS_LIFE). Dead until picker restore.
bool isDashboardRecentsTheme() {
  // Shelf is parked; Stats-Life no longer uses DashboardTheme for home.
  return false;
}

bool isDashboardScrollTheme() { return false; }

bool usesRecentBookSideNav() { return isDashboardRecentsTheme() || isDashboardScrollTheme(); }

bool isBareTheme() {
  return static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme) == CrossPointSettings::UI_THEME::BARE;
}

// Stats home (cover + this-book stats only).
bool isStatsTheme() {
  return static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme) == CrossPointSettings::UI_THEME::STATS;
}

// Front-button home chrome (no classic bottom list): Bare + Stats + Stats-Life.
bool usesMinimalHomeInteraction() { return isStatsLifeTheme() || isBareTheme() || isStatsTheme(); }

// Stats and Stats-Life share FocusTheme layout and the same cover gen size.
bool usesStatsFamilyCover() { return isStatsTheme() || isStatsLifeTheme(); }

// Hero thumb height for the *current* theme so gen size matches on-screen blit
// (1:1). Scaling 2-bit Atkinson is what creates gridlines.
int homeHeroThumbHeight(const GfxRenderer& renderer, const int fallbackCoverHeight) {
  if (isBareTheme()) {
    return HomeCoverMetrics::thumbHeight;  // 560 → 420×560, Bare 1:1
  }
  // Stats + Stats-Life: ONE shared height key (same thumb file, same plate).
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
// Bare also appends Settings at the bottom (Stats keeps Settings as its own key).
enum class MinimalMenuAction : uint8_t { RecentBooks, ReadingStats, OpdsBrowser, FileTransfer, Settings };

struct MinimalMenuItem {
  const char* label;
  UIIcon icon;
  MinimalMenuAction action;
};

int buildMinimalMenuItems(MinimalMenuItem* out, int maxItems, const bool hasOpdsServers,
                          const bool hasCurrentBook, const bool includeSettings) {
  int n = 0;
  if (n < maxItems) out[n++] = {tr(STR_MENU_RECENT_BOOKS), Recent, MinimalMenuAction::RecentBooks};
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

  // Treat only real BMPs as present (corrupt partial files must re-enter gen).
  auto thumbLooksValid = [](const std::string& path) -> bool {
    if (path.empty() || !Storage.exists(path.c_str())) return false;
    HalFile probe;
    if (!Storage.openFileForRead("HOME", path, probe)) return false;
    char sig[2] = {};
    const size_t n = probe.read(sig, 2);
    const size_t sz = probe.size();
    probe.close();
    return n == 2 && sig[0] == 'B' && sig[1] == 'M' && sz > 62;
  };
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

  // Draw Loading into the framebuffer only — do NOT half-refresh here.
  // A HALF over a just-multipassed gray home flashes white/inverted and forces
  // another full multipass. Final art is shown once via requestUpdate below.
  auto showProgress = [&](int progress, int total) {
    if (!showingLoading) {
      popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP), BaseTheme::kPopupCenterY, /*refresh=*/false);
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
        } else if (ensureThumbs(epub)) {
          const std::string templatePath = epub.getThumbBmpPath();
          RECENT_BOOKS.updateBook(book.path, book.title, book.author, templatePath);
          book.coverBmpPath = templatePath;
          anyNewThumb = true;
        } else {
          LOG_ERR("HOME", "Thumb generate failed for: %s (heap=%u maxAlloc=%u)", book.path.c_str(),
                  static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
          anyTransientFail = true;
        }
        // Epub object + metadata cache go out of scope next — yield so the heap
        // can coalesce before the next book’s JPEG decode.
        delay(1);
      } else if (book.coverBmpPath.empty()) {
        const std::string templatePath = epub.getThumbBmpPath();
        RECENT_BOOKS.updateBook(book.path, book.title, book.author, templatePath);
        book.coverBmpPath = templatePath;
        pathsUpdated = true;
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
  libraryLongPressFired = false;
  menuLongPressFired = false;
  minimalSuppressInitialFrontRelease = usesMinimalHomeInteraction();
  readLongPressFired = false;
  backPressSeen = false;
  backResumeArmed = false;
  minimalMenuIndex = 0;
  // Allow cover pass to run again after leaving reader / changing theme.
  // Clear settled multipass so a theme switch (Stats ↔ Stats-Life ↔ Bare)
  // always redraws and re-multipasses the shared/family thumb.
  freeCoverBuffer();
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

  // Paint home first so Loading can float over real UI (title/footer visible).
  requestUpdate();
}

void HomeActivity::onExit() {
  Activity::onExit();

  // Free the stored cover buffer if any
  freeCoverBuffer();
  coverGrayOnPanel = false;
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
  // Fallback: plain BW half-refresh (1-bit thumbs, missing art, classic empty state).
  auto displayBw = [this]() {
    coverGrayOnPanel = false;
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  };

  if (coverRectW <= 0 || coverRectH <= 0 || recentBooks.empty()) {
    displayBw();
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
    displayBw();
    return;
  }

  HalFile file;
  if (!Storage.openFileForRead("HOME", coverPath, file)) {
    displayBw();
    return;
  }

  Bitmap bitmap(file);
  if (bitmap.parseHeaders() != BmpReaderError::Ok || bitmap.getWidth() <= 0 || bitmap.getHeight() <= 0 ||
      !bitmap.hasGreyscale()) {
    file.close();
    // 1-bit or corrupt: single-pass is correct (dither already baked in).
    displayBw();
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
  if (coverBuffer) {
    free(coverBuffer);
    coverBuffer = nullptr;
    coverBufferSize = 0;
    coverBufferStored = false;
  }

  // Hardware order (sleep / reader): base first, then gray planes, then gray refresh.
  // File already open so post-base work is only two row-walks + gray refresh.
  const bool savedBw = renderer.storeBwBuffer();
  if (!savedBw) {
    LOG_ERR("HOME", "Cover multipass without BW store (heap); not settling blank panel");
    // Do not clearScreen / displayGrayBuffer without a BW restore path — that
    // was the Bare "cover disappeared after flash" bug.
    file.close();
    renderer.setRenderMode(GfxRenderer::BW);
    coverGrayOnPanel = false;
    coverRendered = false;
    // Keep coverRect* so a retry still knows the art box; show BW shell as-is.
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    return;
  }

  // Base: UI already in FB (2-bit midtones as black). HALF for clean leave-reader base.
  renderer.displayGrayscaleBase(HalDisplay::HALF_REFRESH);

  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
  drawCoverArt();
  renderer.copyGrayscaleLsbBuffers();

  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
  drawCoverArt();
  renderer.copyGrayscaleMsbBuffers();

  renderer.displayGrayBuffer();
  renderer.setRenderMode(GfxRenderer::BW);

  renderer.restoreBwBuffer();
  // Re-sync controller RAM from BW UI for the next menu / diff paint.
  renderer.cleanupGrayscaleWithFrameBuffer();
  coverGrayOnPanel = true;
  file.close();
}

void HomeActivity::freeCoverBuffer() {
  if (coverBuffer) {
    free(coverBuffer);
    coverBuffer = nullptr;
  }
  coverBufferSize = 0;
  coverBufferStored = false;
  // Force themes to redraw from SD; do not leave coverRendered=true with no buffer
  // (Bare used to skip draw and show a blank cover hole).
  coverRendered = false;
  coverGrayOnPanel = false;
}

bool HomeActivity::handleForcedRefresh() {
  // Default main.cpp path is renderer.displayBuffer(HALF) on the current FB.
  // That paints 2-bit cover midtones as solid black and never re-runs multipass.
  // Settled-skip (coverGrayOnPanel) would also block the next requestUpdate.
  coverGrayOnPanel = false;
  // Redraw shell + art, then multipassHomeCoverGrayscale() restores greys.
  coverRendered = false;
  requestUpdate();
  return true;
}

void HomeActivity::loop() {
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
    if (!recentsLoaded) {
      const auto& metrics = UITheme::getInstance().getMetrics();
      recentsLoading = true;
      loadRecentCovers(metrics.homeCoverHeight);
      return;
    }
  }

  // Home themes: Bare = Menu/Library/Synopsis/Read; Stats family = Menu/Library/Settings/Read.
  if (usesMinimalHomeInteraction()) {
    const int releasedFrontButton = mappedInput.getReleasedFrontButton();

    if (minimalSuppressInitialFrontRelease) {
      if (releasedFrontButton >= 0) {
        minimalSuppressInitialFrontRelease = false;
        return;
      }
      if (isAnyFrontButtonPressed(mappedInput)) {
        return;
      }
      minimalSuppressInitialFrontRelease = false;
    }

    if (minimalMenuOpen) {
      // Up to: Recents, Stats, OPDS, Transfer; Settings only on Bare (no Settings front key).
      MinimalMenuItem menuItems[6];
      const int menuCount = buildMinimalMenuItems(menuItems, 6, hasOpdsServers, !recentBooks.empty(),
                                                  /*includeSettings=*/isBareTheme());
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
        requestUpdate();
        return;
      }
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
          releasedFrontButton == HalGPIO::BTN_CONFIRM) {
        switch (menuItems[minimalMenuIndex].action) {
          case MinimalMenuAction::RecentBooks:
            onRecentsOpen();
            break;
          case MinimalMenuAction::ReadingStats: {
            if (recentBooks.empty()) break;
            const RecentBook& book = recentBooks[static_cast<size_t>(focusedRecentIndex())];
            const std::string cachePath = getRecentBookCachePath(book);
            BookReadingStats stats = BookReadingStats::loadForBook(book.path);
            const float progress = stats.getProgressPercent();
            const GlobalReadingStats global = GlobalReadingStats::load();
            const GlobalReadingStats aggregated = GlobalReadingStats::loadAggregated(global);
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

    // Bare:     Menu · Library · Synopsis · Read  (Settings under Menu / long-press Menu)
    // Stats family: Menu · Library · Settings · Read
    auto activateMinimalHomeNav = [this](int index) {
      if (isBareTheme()) {
        switch (index) {
          case 0:  // Menu
            minimalMenuOpen = true;
            minimalMenuIndex = 0;
            requestUpdate();
            break;
          case 1:  // Library
            onFileBrowserOpen();
            break;
          case 2:  // Synopsis → focused book
            if (!recentBooks.empty()) {
              const RecentBook& book = recentBooks[static_cast<size_t>(focusedRecentIndex())];
              std::string desc = BookActions::loadBookDescription(book.path);
              startActivityForResult(
                  std::make_unique<BookDescriptionActivity>(renderer, mappedInput, book.title, std::move(desc)),
                  [this](const ActivityResult&) { requestUpdate(); });
            }
            break;
          case 3:  // Read
            onContinueReading();
            break;
        }
        return;
      }
      switch (index) {
        case 0:  // Menu
          minimalMenuOpen = true;
          minimalMenuIndex = 0;
          requestUpdate();
          break;
        case 1:  // Library
          onFileBrowserOpen();
          break;
        case 2:  // Settings (Stats / Stats Scroll / Shelf)
          onSettingsOpen();
          break;
        case 3:  // Read
          onContinueReading();
          break;
      }
    };

    // Bare-only long-press: Menu → Settings, Library → Recent Books.
    if (isBareTheme()) {
      if (libraryLongPressFired) {
        if (!mappedInput.isFrontButtonPressed(HalGPIO::BTN_CONFIRM)) {
          libraryLongPressFired = false;
        }
        return;
      }
      if (menuLongPressFired) {
        if (!mappedInput.isFrontButtonPressed(HalGPIO::BTN_BACK)) {
          menuLongPressFired = false;
        }
        return;
      }
      if (mappedInput.isFrontButtonPressed(HalGPIO::BTN_BACK) && mappedInput.getHeldTime() >= READ_LONG_PRESS_MS) {
        menuLongPressFired = true;
        onSettingsOpen();
        return;
      }
      if (mappedInput.isFrontButtonPressed(HalGPIO::BTN_CONFIRM) && mappedInput.getHeldTime() >= READ_LONG_PRESS_MS) {
        libraryLongPressFired = true;
        onRecentsOpen();
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
    if (!recentBooks.empty() && mappedInput.isFrontButtonPressed(HalGPIO::BTN_RIGHT) &&
        mappedInput.getHeldTime() >= READ_LONG_PRESS_MS) {
      readLongPressFired = true;
      showCurrentBookActionMenu(true);
      return;
    }
    if (releasedFrontButton == HalGPIO::BTN_RIGHT) {
      if (!recentBooks.empty()) {
        activateMinimalHomeNav(3);
      }
      return;
    }

    // Touch: tap cover to continue reading.
    const auto& metrics = UITheme::getInstance().getMetrics();
    if (!recentBooks.empty() &&
        mappedInput.wasTapInRect(0, metrics.homeTopPadding, renderer.getScreenWidth(), metrics.homeCoverTileHeight)) {
      onContinueReading();
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

void HomeActivity::render(RenderLock&&) {
  // While covers generate, keep the painted home UI under the floating Loading box.
  if (recentsLoading) {
    return;
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  // Dashboard / Minimal / Focus: full-bleed cover + bottom hints (no classic bottom menu list).
  if (usesMinimalHomeInteraction()) {
    // Settled gray cover on panel: ignore spurious requestUpdate (USB detect bounce,
    // background cover-gen probes, etc.). Re-running multipass is a full black flash.
    // Intentional invalidation (onExit freeCover, menu open, new thumb) clears
    // coverGrayOnPanel so the next paint multipasses once.
    if (recentsLoaded && coverGrayOnPanel && coverBufferStored && coverRendered && !minimalMenuOpen) {
      homeUiReady = true;
      return;
    }

    renderer.clearScreen();

    if (minimalMenuOpen) {
      GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.homeTopPadding}, nullptr);
      MinimalMenuItem menuItems[6];
      const int menuCount = buildMinimalMenuItems(menuItems, 6, hasOpdsServers, !recentBooks.empty(),
                                                  /*includeSettings=*/isBareTheme());
      // Vertically center the item stack between header band and footer hints
      // (was top-aligned under the header — more white around helps hide cover ghosting).
      const int bandTop = metrics.topPadding + metrics.homeTopPadding;
      const int bandBottom = pageHeight - metrics.buttonHintsHeight;
      const int menuH =
          menuCount > 0
              ? menuCount * metrics.menuRowHeight + (menuCount - 1) * metrics.menuSpacing
              : 0;
      const int menuTop = bandTop + std::max(0, (bandBottom - bandTop - menuH) / 2);
      GUI.drawButtonMenu(renderer, Rect{0, menuTop, pageWidth, std::max(menuH, 1)}, menuCount, minimalMenuIndex,
                         [&menuItems](int index) { return std::string(menuItems[index].label); },
                         [&menuItems](int index) { return menuItems[index].icon; });
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      // BW menu overwrites gray cover state on the panel.
      coverGrayOnPanel = false;
      renderer.displayBuffer();
      homeUiReady = true;
      return;
    }

    // Clear first: cover snapshot is cover-art only, not the full tile, so we
    // cannot rely on a large restore to erase the previous frame (e.g. Menu).
    renderer.clearScreen();
    bool bufferRestored = coverBufferStored && restoreCoverBuffer();

    // Top chrome (battery icon+% / clock). Bare defaults those off, but if the
    // user enables them, reserve a header band so they do not sit on the cover.
    const bool bareChrome =
        isBareTheme() &&
        (SETTINGS.systemStatusBarHas(CrossPointSettings::SYS_SLOT_BATTERY) ||
         SETTINGS.systemStatusBarHas(CrossPointSettings::SYS_SLOT_CLOCK));
    if (!isBareTheme() || bareChrome) {
      const int headerTop = isBareTheme() ? 0 : metrics.topPadding;
      const int headerH =
          BaseTheme::kTopChromeBatteryY + std::max(metrics.batteryHeight + 8, metrics.statusBarVerticalMargin);
      GUI.drawHeader(renderer, Rect{0, headerTop, pageWidth, headerH}, nullptr);
    }

    auto storeCover = [this](int x, int y, int w, int h) -> bool {
      coverRectX = x;
      coverRectY = y;
      coverRectW = w;
      coverRectH = h;
      return storeCoverBuffer();
    };

    // Rect is a full-screen hint; themes pack cover/title in that band.
    GUI.drawRecentBookCover(renderer, Rect{0, 0, pageWidth, pageHeight}, recentBooks, selectorIndex, coverRendered,
                            coverBufferStored, bufferRestored, storeCover, &currentBookStats,
                            currentBookProgressPercent, &globalStats, nullptr);

    if (isBareTheme()) {
      GUI.drawButtonHints(renderer, tr(STR_MENU), tr(STR_LIBRARY), recentBooks.empty() ? "" : tr(STR_SYNOPSIS),
                          recentBooks.empty() ? "" : tr(STR_READ));
    } else {
      // Stats / Focus: Menu · Library · Settings · Read
      GUI.drawButtonHints(renderer, tr(STR_MENU), tr(STR_LIBRARY), tr(STR_SETTINGS_SHORT),
                          recentBooks.empty() ? "" : tr(STR_READ));
    }

    // Defer multipass until loadRecentCovers has run once (BW shell first).
    // loadRecentCovers then requestUpdate once for multipass if needed.
    if (!recentsLoaded) {
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
      // Do not set coverGrayOnPanel — multipass has not run yet.
    } else {
      multipassHomeCoverGrayscale();
    }
    homeUiReady = true;
    return;
  }

  renderer.clearScreen();
  bool bufferRestored = coverBufferStored && restoreCoverBuffer();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.homeTopPadding},
                 metrics.homeContinueReadingInMenu && !recentBooks.empty() ? recentBooks[0].title.c_str() : nullptr);

  auto storeCover = [this](int x, int y, int w, int h) -> bool {
    coverRectX = x;
    coverRectY = y;
    coverRectW = w;
    coverRectH = h;
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

  multipassHomeCoverGrayscale();
  homeUiReady = true;
}

void HomeActivity::onSelectBook(const std::string& path) { activityManager.goToReader(path); }

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
  selectorIndex = (focusedRecentIndex() + delta % n + n) % n;
  loadFocusedRecentStats();
}

void HomeActivity::showCurrentBookActionMenu(const bool ignoreInitialConfirmRelease) {
  if (recentBooks.empty()) {
    return;
  }
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

        // Always invalidate settled home paint. coverGrayOnPanel skip would leave
        // the action-menu framebuffer on the panel ("Back does nothing"), and the
        // next Back would open Menu instead of showing home.
        freeCoverBuffer();
        // Delete Cache (and similar) may have removed thumbs while the menu stayed
        // open — re-scan so multipass has real art again.
        recentsLoaded = false;
        recentsLoading = false;
        coverNeedsRetry = false;
        coverGenAttempts = 0;
        coverRetryAtMs = 0;

        if (result.isCancelled) {
          requestUpdate();
          return;
        }
        const auto* actionResult = std::get_if<FileBrowserActionResult>(&result.data);
        if (!actionResult) {
          requestUpdate();
          return;
        }
        switch (static_cast<FileBrowserAction>(actionResult->action)) {
          case FileBrowserAction::Open:
            onSelectBook(book.path);
            return;
          case FileBrowserAction::Delete:
          case FileBrowserAction::RemoveFromRecents:
            reloadHomeAfterBookAction();
            return;
          default:
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

void HomeActivity::onFileBrowserOpen() { activityManager.goToFileBrowser(); }

void HomeActivity::onRecentsOpen() { activityManager.goToRecentBooks(); }

void HomeActivity::onSettingsOpen() { activityManager.goToSettings(); }

void HomeActivity::onFileTransferOpen() { activityManager.goToFileTransfer(); }

void HomeActivity::onOpdsBrowserOpen() { activityManager.goToBrowser(); }
