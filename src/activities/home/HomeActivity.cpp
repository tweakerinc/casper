#include "HomeActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
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

#include "BookActions.h"
#include "CasperSettings.h"
#include "CasperState.h"
#include "FileBrowserActionActivity.h"
#include "MappedInputManager.h"
#include "util/CasperPaths.h"

#include "OpdsServerStore.h"
#include "RecentBooksStore.h"
#include "activities/ActivityManager.h"
#include "activities/RenderLock.h"
#include "activities/reader/BookStatsActivity.h"
#include "activities/reader/GlobalReadingStats.h"
#include "activities/reader/ReaderActivity.h"
#include "components/UITheme.h"
#include "components/themes/BaseTheme.h"
#include "components/themes/HomeCoverMetrics.h"
#include "components/themes/bare/BareTheme.h"
#include "components/themes/penumbra/PenumbraTheme.h"
#include "fontIds.h"
#include "util/SystemLog.h"
#include "util/UiGhostPolicy.h"

namespace {

// Home long-press (Menu→Settings, Read→book menu).
// Keep clearly above debounce (~5–20ms) and below "sticky hold" feel.
// 200ms: snappy hold; short taps still release before threshold.
constexpr unsigned long READ_LONG_PRESS_MS = 200;
// Abort greys while still holding so ActivityManager idle wait is short.
constexpr unsigned long LONG_PRESS_PRECANCEL_MS = 70;

// Legacy Dashboard shelf/scroll themes are not in the firmware; stubs keep
// shared helper call sites compiling without pulling DashboardTheme.cpp.
bool isDashboardRecentsTheme() { return false; }
bool isDashboardScrollTheme() { return false; }
bool usesRecentBookSideNav() { return false; }

bool isBareTheme() {
  return static_cast<CasperSettings::UI_THEME>(SETTINGS.uiTheme) == CasperSettings::UI_THEME::BARE;
}

bool isPenumbraTheme() {
  return static_cast<CasperSettings::UI_THEME>(SETTINGS.uiTheme) == CasperSettings::UI_THEME::PENUMBRA;
}

// Stats (FocusTheme) is not in this firmware build.
bool isStatsTheme() { return false; }

// Front-button home chrome (no classic bottom list).
bool usesMinimalHomeInteraction() { return isBareTheme() || isPenumbraTheme(); }

// Menu · Library · Recents · Read (Settings under Menu / long-press Menu).
bool usesBareStyleHomeNav() { return usesMinimalHomeInteraction(); }

// Stats family cover gen — disabled with Stats theme.
bool usesStatsFamilyCover() { return false; }

// Themes that paint a cover and need multipass greys. Penumbra is text-only.
bool usesHomeCoverMultipass() { return usesMinimalHomeInteraction() && !isPenumbraTheme(); }

// Hero thumb height for the *current* theme so gen size matches on-screen blit
// (1:1). Scaling 2-bit Atkinson is what creates gridlines.
int homeHeroThumbHeight(const GfxRenderer& renderer, const int fallbackCoverHeight) {
  if (isBareTheme()) {
    return HomeCoverMetrics::thumbHeight;  // 560 → 420×560, Bare 1:1
  }
  // Stats: shared height key (same thumb file, same plate).
  if (usesStatsFamilyCover()) {
    return HomeCoverMetrics::statsFamilyHeroThumbHeight(renderer.getScreenWidth(), renderer.getScreenHeight());
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
  MinimalMenuAction action;
};

int buildMinimalMenuItems(MinimalMenuItem* out, int maxItems, const bool hasOpdsServers, const bool hasCurrentBook,
                          const bool includeSettings) {
  int n = 0;
  if (hasCurrentBook && SETTINGS.readingStatsTrackingEnabled() && n < maxItems) {
    out[n++] = {tr(STR_READING_STATS), MinimalMenuAction::ReadingStats};
  }
  if (hasOpdsServers && n < maxItems) {
    out[n++] = {tr(STR_OPDS_BROWSER), MinimalMenuAction::OpdsBrowser};
  }
  if (n < maxItems) out[n++] = {tr(STR_FILE_TRANSFER), MinimalMenuAction::FileTransfer};
  if (includeSettings && n < maxItems) {
    out[n++] = {tr(STR_SETTINGS_TITLE), MinimalMenuAction::Settings};
  }
  return n;
}

// Same path as EpubReaderActivity save: Epub(path, CasperPaths::kPackageCacheRoot).getCachePath().
// Constructor hashes the path; load() is not required for the cache directory.
std::string getRecentBookCachePath(const RecentBook& book) {
  if (FsHelpers::hasEpubExtension(book.path)) {
    return Epub(book.path, CasperPaths::kPackageCacheRoot).getCachePath();
  }
  if (FsHelpers::hasXtcExtension(book.path)) {
    return std::string(CasperPaths::kPackageCacheRoot) + "/xtc_" +
           std::to_string(std::hash<std::string>{}(book.path));
  }
  if (FsHelpers::hasTxtExtension(book.path) || FsHelpers::hasMarkdownExtension(book.path)) {
    return std::string("/.crosspoint/txt_") + std::to_string(std::hash<std::string>{}(book.path));
  }
  return {};
}

BookReadingStats loadRecentBookStats(const RecentBook& book) {
  // Full stats only when Stats UI needs them — single book dir via loadForBook.
  return BookReadingStats::loadForBook(book.path);
}

// Dashboard progress %: prefer recent.json (CasperStats); no multi-path SD hunt.
float loadRecentBookProgressPercent(const RecentBook& book) {
  if (book.progressPercentMilli != 0xFFFF) {
    return static_cast<float>(book.progressPercentMilli) / 100.0f;
  }
  return BookReadingStats::loadForBook(book.path).getProgressPercent();
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
bool bindExistingHeroThumbsIfReady(std::vector<RecentBook>& recentBooks, int heroH, bool shelfTheme, int shelfH) {
  if (recentBooks.empty()) {
    return true;
  }
  for (RecentBook& book : recentBooks) {
    if (FsHelpers::hasEpubExtension(book.path)) {
      Epub epub(book.path, CasperPaths::kPackageCacheRoot);
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
      Xtc xtc(book.path, CasperPaths::kPackageCacheRoot);
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

void HomeActivity::paintMinimalMenu(const bool bandOnly) {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int bandTop = metrics.topPadding + metrics.homeTopPadding;
  const int bandBottom = pageHeight - metrics.buttonHintsHeight;

  MinimalMenuItem menuItems[6];
  const int menuCount =
      buildMinimalMenuItems(menuItems, 6, hasOpdsServers, !recentBooks.empty(), /*includeSettings=*/true);
  const int menuH =
      menuCount > 0 ? menuCount * metrics.menuRowHeight + (menuCount - 1) * metrics.menuSpacing : 0;
  const int menuTop = bandTop + std::max(0, (bandBottom - bandTop - menuH) / 2);
  const Rect menuRect{0, menuTop, pageWidth, std::max(menuH, 1)};
  auto menuLabel = [&menuItems](int index) { return std::string(menuItems[index].label); };
  const std::function<UIIcon(int)> noIcon;

  if (bandOnly) {
    if (bandBottom > bandTop) {
      renderer.fillRect(0, bandTop, pageWidth, bandBottom - bandTop, false);
    }
    GUI.drawButtonMenu(renderer, menuRect, menuCount, minimalMenuIndex, menuLabel, noIcon);
    UiGhostPolicy::displayMenuBand(renderer, 0, bandTop, pageWidth, std::max(1, bandBottom - bandTop));
    return;
  }

  // Greys teardown must happen *before* painting the menu plate, never after.
  // Multipass ends by rebasing controller DTM1/DTM2 to the *home* BW shell while
  // greys stay on glass. Library open diffs that home baseline → dense list and
  // looks clean. If we cleanup with the *new* menu FB after draw, both planes
  // become the sparse menu with no glass update — the following FAST sees zero
  // differential and the main home image remains fully visible on glass.
  renderer.setRenderMode(GfxRenderer::BW);
  renderer.restoreBwBuffer();  // no-op if multipass already freed the store

  renderer.clearScreen(0xFF);
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.homeTopPadding}, nullptr);
  GUI.drawButtonMenu(renderer, menuRect, menuCount, minimalMenuIndex, menuLabel, noIcon);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  homeMenuShellOnPanel = true;
  coverGrayOnPanel = false;
  coverRendered = false;
  // Same open path as Library (FAST + soft settle). DTM1 still holds home BW so
  // the differential actually drives home → menu. Up/Down = band FAST only.
  UiGhostPolicy::displaySoftOpen(renderer, /*softCount=*/1);
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

  // Per-theme hero height so gen matches the on-screen plate (1:1, no grids).
  // Shelf still needs a compact 168px row when that theme is active.
  const int heroH = homeHeroThumbHeight(renderer, coverHeight);
  const bool shelfTheme = isDashboardRecentsTheme();
  const int shelfH = HomeCoverMetrics::homeShelfThumbHeight;

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
      Epub epub(book.path, CasperPaths::kPackageCacheRoot);
      if (!heroThumbExists(epub) || (shelfTheme && !thumbLooksValid(epub.getThumbBmpPath(shelfH)))) {
        anyNeedWork = true;
        break;
      }
    } else if (FsHelpers::hasXtcExtension(book.path)) {
      Xtc xtc(book.path, CasperPaths::kPackageCacheRoot);
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

  // Upper-left "Loading" into the framebuffer only — do NOT window/HALF refresh
  // here. On X4 (SSD1677), a window while greys are on glass can promote to a full
  // HALF clean and black-flash in a loop. Next multipass/home paint shows it once.
  auto showProgress = [&](int /*progress*/, int /*total*/) {
    if (!showingLoading) {
      GUI.drawTopLeftStatus(renderer, tr(STR_LOADING_POPUP), /*refresh=*/false);
      showingLoading = true;
    }
  };

  const int total = std::max(1, static_cast<int>(recentBooks.size()));
  int progress = 0;
  bool anyNewThumb = false;
  bool anyTransientFail = false;
  bool pathsUpdated = false;
  for (RecentBook& book : recentBooks) {
    if (FsHelpers::hasEpubExtension(book.path)) {
      Epub epub(book.path, CasperPaths::kPackageCacheRoot);
      const bool needWork = !heroThumbExists(epub) || (shelfTheme && !thumbLooksValid(epub.getThumbBmpPath(shelfH)));
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
      Xtc xtc(book.path, CasperPaths::kPackageCacheRoot);
      if (xtc.load()) {
        const bool needWork = !heroThumbExists(xtc) || (shelfTheme && !thumbLooksValid(xtc.getThumbBmpPath(shelfH)));
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
  suppressMenuBackUntilMs = 0;
  readLongPressFired = false;
  backPressSeen = false;
  backResumeArmed = false;
  minimalMenuIndex = 0;
  // Cold/first Home: HALF scrub (boot logo residual / first shell).
  UiGhostPolicy::requestHardScrub();
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
  if (SETTINGS.readingStatsTrackingEnabled()) {
    globalStats = GlobalReadingStats::load();
  } else {
    globalStats = GlobalReadingStats{};
  }
  // Stats Recents (and other dashboard homes): selectorIndex is the focused recent.
  // Classic list themes still use it for the bottom menu row.
  selectorIndex = 0;
  penumbraRecentsFocus = 0;
  // Penumbra: land on the device default under-page (X4 = Recents leftmost).
  // Always reset so a stale session mode cannot leave Recents on the far-right dot.
  if (isPenumbraTheme()) {
    PenumbraThemeUi::resetUnderModeToDefault();
    PenumbraThemeUi::clampUnderModeToTracking();
  }
  if (!recentBooks.empty()) {
    loadFocusedRecentStats();
  }

  if (!usesMinimalHomeInteraction()) {
    const auto base = static_cast<int>(recentBooks.size());
    selectorIndex = initialMenuItem == HomeMenuItem::NONE ? 0 : base + menuItemToIndex(initialMenuItem, hasOpdsServers);
  }

  // Phase 1 (A1): when hero thumbs already exist (typical goHome after reading),
  // bind paths now so the first paint multipasses with real art — no shell-only HALF.
  {
    const int heroH = homeHeroThumbHeight(renderer, metrics.homeCoverHeight);
    const bool shelfTheme = isDashboardRecentsTheme();
    if (bindExistingHeroThumbsIfReady(recentBooks, heroH, shelfTheme, HomeCoverMetrics::homeShelfThumbHeight)) {
      recentsLoaded = true;
      LOG_DBG("HOME", "Hero thumbs ready — multipass on first paint (skip shell HALF)");
    }
  }

  // Paint home first so Loading can float over real UI (title/footer visible).
  requestUpdate();
}

void HomeActivity::onExit() {
  Activity::onExit();
  deferredHalfScrubOnly = false;

  // Free the stored cover buffer if any
  freeCoverBuffer();
  coverGrayOnPanel = false;
}

void HomeActivity::markSnappyResumeReady() {
  // Used when Home is seeded under the reader without a first paint (QR/cold open).
  paintedUiTheme = static_cast<int>(SETTINGS.uiTheme);
  homeUiReady = true;
  recentsLoaded = true;
  if (isPenumbraTheme()) {
    // Text-only: FAST resume is correct (no cover greys to restore).
    penumbraHalfBaselineDone = true;
    leaveForUiChildSnappy = true;
    coverGrayOnPanel = true;
  } else if (usesHomeCoverMultipass()) {
    // Bare / Dashboard / Focus: first PopToHome must run perfect multipass greys.
    // Forcing snappy here left solid-BW jackets + menu ghosting on X4.
    leaveForUiChildSnappy = false;
    coverGrayOnPanel = false;
    coverRendered = false;
    penumbraHalfBaselineDone = false;
  } else {
    leaveForUiChildSnappy = true;
    coverGrayOnPanel = false;
  }
}

void HomeActivity::seedUnderReader() {
  // QR critical path: do not touch SD or request paint. Back from book runs
  // onResume → full enter load once RECENT_BOOKS (etc.) are available.
  deferredEnterLoad_ = true;
  minimalMenuOpen = false;
  homeMenuShellOnPanel = false;
  markSnappyResumeReady();
  // recentsLoaded=true from markSnappy is a lie until deferred load — paint
  // path rebinds when deferredEnterLoad_ completes on resume.
  recentsLoaded = false;
  homeUiReady = false;
}

void HomeActivity::onResume() {
  // Back from reader / settings / library / menu: hard HALF on first home paint so
  // residual from soft FAST children is scrubbed. One intentional flash; panel looks clean.
  // Cover themes may still defer greys after the BW shell.
  Activity::onResume();

  bool justLoadedDeferredEnter = false;
  if (deferredEnterLoad_) {
    deferredEnterLoad_ = false;
    justLoadedDeferredEnter = true;
    // Same data load as onEnter, without re-arming a second cold boot scrub race.
    hasOpdsServers = OPDS_STORE.hasServers();
    const auto& metrics = UITheme::getInstance().getMetrics();
    loadRecentBooks(metrics.homeRecentBooksCount);
    currentBookStats = BookReadingStats{};
    currentBookProgressPercent = -1.0f;
    if (SETTINGS.readingStatsTrackingEnabled()) {
      globalStats = GlobalReadingStats::load();
    } else {
      globalStats = GlobalReadingStats{};
    }
    selectorIndex = 0;
    penumbraRecentsFocus = 0;
    if (isPenumbraTheme()) {
      PenumbraThemeUi::resetUnderModeToDefault();
      PenumbraThemeUi::clampUnderModeToTracking();
    }
    if (!recentBooks.empty()) {
      loadFocusedRecentStats();
    }
    const int heroH = homeHeroThumbHeight(renderer, metrics.homeCoverHeight);
    const bool shelfTheme = isDashboardRecentsTheme();
    if (bindExistingHeroThumbsIfReady(recentBooks, heroH, shelfTheme, HomeCoverMetrics::homeShelfThumbHeight)) {
      recentsLoaded = true;
    }
    homeUiReady = true;
  }

  // Capture the snappy-return hint BEFORE clearing it. This flag was written in
  // five places and never read — so every return to Home armed a hard scrub and
  // paid a full HALF (~3.2s on X3), even coming back from a light chrome child
  // like the book action sheet (device log: 2.2s stall + 3184ms HALF + 1108ms
  // clock ≈ 6.5s back to Home).
  const bool snappyReturnFromUiChild = leaveForUiChildSnappy;
  leaveForUiChildSnappy = false;

  freeCoverBufferRamOnly();
  // Panel was overwritten by the child — greys on glass are gone. Tear down any
  // leftover grey controller state from an aborted multipass so BW shell is clean
  // (Bare: Loading/reader residual as "glitched" chrome without this).
  renderer.setRenderMode(GfxRenderer::BW);
  renderer.restoreBwBuffer();
  renderer.cleanupGrayscaleWithFrameBuffer();
  coverGrayOnPanel = false;
  coverRendered = false;
  coverGrayNeedsRetry = false;
  coverGrayRetryAtMs = 0;
  deferredHalfScrubOnly = false;
  deferredHalfScrubAtMs = 0;
  deferredGreysOnly = false;
  softGrayscaleBase = false;
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
  penumbraHalfBaselineDone = false;
  // Reader → Home genuinely needs the HALF: a dense book page ghosts badly and a
  // FAST pass settles that residual into the panel. A light UI child (book action
  // sheet, settings, menus) leaves almost nothing to scrub, so it gets a FAST
  // paint and keeps the deferred clock AA.
  if (snappyReturnFromUiChild) {
    UiGhostPolicy::clearHardScrub();
    // Device log (47e06f62): FAST first paint then a second full FAST+clockAA
    // (~950ms). Defer the clock greys to the windowed digit path so Home is
    // already on glass. cancelBackgroundPaint was left true from the child
    // and skipped clock AA on the first paint anyway — clear it here.
    deferScrubAfterFirstPaint_ = true;
    cancelBackgroundPaint = false;
    SystemLog::logTiming("HOME", "resume snappy (light UI child) — FAST paint");
  } else {
    UiGhostPolicy::requestHardScrub();
    // ...but get content on glass sooner. Returning from the reader / settings /
    // the book quick-menu used to wait ~2s of SD work plus a ~4s HALF+clockAA
    // before anything appeared.
    //
    // The first attempt deferred the *scrub* (FAST now, HALF a tick later) and
    // that ghosted: a FAST pass settles the previous frame's residual into the
    // panel, and a later HALF cannot fully lift it. What is actually slow is the
    // full-screen greyscale clock multipass, not the HALF — so the HALF stays as
    // the single first paint and the clock's AA is what gets deferred.
    deferScrubAfterFirstPaint_ = true;
    cancelBackgroundPaint = false;
  }
  suppressMenuBackUntilMs = millis() + 900UL;
  // Cover themes: multipass greys on the *first* home paint when thumbs exist
  // (one HALF greys-base). Deferred "shell HALF → wait → greys HALF" felt like
  // a second flash 1–10s later on Bare (X3 multipass alone is multi-second).
  snappyResumeNoGreys = false;
  paintedUiTheme = -1;

  // Portrait: reader may have left landscape.
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  // v36 X3 log: Back from the long-press book menu still logged
  // `activity_slow 2226ms` then `penumbra_full FAST` — the snappy flag avoided
  // HALF but onResume still re-stat'd every recent, re-warmed progress, and
  // reloaded lifetime stats. None of that changed. Skip when RAM is fresh.
  const bool skipSdReload = justLoadedDeferredEnter || skipResumeSdReload_;
  skipResumeSdReload_ = false;
  if (skipSdReload) {
    if (isPenumbraTheme()) recentsLoaded = true;
    LOG_DBG("HOME", "onResume: skip SD reload (RAM recents still valid)");
    return;
  }

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
  // Recents micro-bars: warm RAM cache only (reuse % by path; SD only for new
  // rows). Reader already called updateRecentsProgressForPath for the book left.
  if (isPenumbraTheme() && !recentBooks.empty()) {
    PenumbraThemeUi::warmRecentsProgressCache(recentBooks);
  }
  // X4: tracking hard-off — skip global stats load (no under-panel / menu use).
  if (SETTINGS.readingStatsTrackingEnabled()) {
    globalStats = GlobalReadingStats::load();
  } else {
    globalStats = GlobalReadingStats{};
  }

  // Penumbra paints no cover art, so probing hero thumbs is pure dead work — it
  // constructs an Epub per recent book (SD open + parse each) on the return-to-home
  // path, which is a large part of the 2s of "nothing happens" before the paint.
  const int heroH = homeHeroThumbHeight(renderer, metrics.homeCoverHeight);
  if (isPenumbraTheme()) {
    recentsLoaded = true;
    LOG_DBG("HOME", "onResume: penumbra text home — no thumb probe");
  } else if (bindExistingHeroThumbsIfReady(recentBooks, heroH, isDashboardRecentsTheme(),
                                           HomeCoverMetrics::homeShelfThumbHeight)) {
    recentsLoaded = true;
    LOG_DBG("HOME", "onResume: thumbs ready — HALF shell then multipass");
  } else if (!usesHomeCoverMultipass()) {
    recentsLoaded = true;
    // First paint honors hardScrubArmed → HALF (baseline set after that paint).
    LOG_DBG("HOME", "onResume: text home — HALF scrub on first paint");
  } else {
    recentsLoaded = false;
    snappyResumeNoGreys = false;  // need gen path before multipass
    LOG_DBG("HOME", "onResume: thumbs missing — gen after shell free=%u maxAlloc=%u",
            static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
    SystemLog::logTiming("HOME", "thumbs_missing free=%u maxAlloc=%u", static_cast<unsigned>(ESP.getFreeHeap()),
                         static_cast<unsigned>(ESP.getMaxAllocHeap()));
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
  const uint32_t tMultipass = millis();
  const uint8_t themeId = SETTINGS.uiTheme;
  auto logMultipassDone = [tMultipass, themeId](const char* outcome) {
    SystemLog::logTimed("HOME", millis() - tMultipass, "multipass theme=%u outcome=%s fre=%u",
                        static_cast<unsigned>(themeId), outcome ? outcome : "?",
                        static_cast<unsigned>(ESP.getFreeHeap()));
  };
  auto leavingHome = [this]() -> bool {
    return cancelBackgroundPaint || !activityManager.isCurrentActivity(this) ||
           activityManager.hasPendingActivityChange();
  };

  // Fallback: plain BW half-refresh (1-bit thumbs, missing art, classic empty state).
  // settle=true claims the panel so we do not thrash multipass retries on heap OOM.
  auto displayBw = [this, &leavingHome, &logMultipassDone](bool settle, const char* outcome) {
    if (settle) {
      coverGrayOnPanel = true;
      coverGrayNeedsRetry = false;
      paintedUiTheme = static_cast<int>(SETTINGS.uiTheme);
    } else {
      coverGrayOnPanel = false;
    }
    if (leavingHome()) {
      logMultipassDone("leave_bw");
      return;
    }
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    UiGhostPolicy::noteHalf();
    logMultipassDone(outcome);
  };

  if (leavingHome()) {
    coverGrayOnPanel = false;
    logMultipassDone("leave_early");
    return;
  }

  if (coverRectW <= 0 || coverRectH <= 0 || recentBooks.empty()) {
    displayBw(true, "bw_no_cover");
    return;
  }

  // Dashboard/Bare cycle the focused recent; classic themes always paint books[0].
  const size_t bookIdx = usesMinimalHomeInteraction() ? static_cast<size_t>(focusedRecentIndex()) : 0;
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
    Epub epub(book.path, CasperPaths::kPackageCacheRoot);
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
    displayBw(true, "bw_no_path");
    return;
  }

  HalFile file;
  if (!Storage.openFileForRead("HOME", coverPath, file)) {
    displayBw(true, "bw_open_fail");
    return;
  }

  Bitmap bitmap(file);
  if (bitmap.parseHeaders() != BmpReaderError::Ok || bitmap.getWidth() <= 0 || bitmap.getHeight() <= 0 ||
      !bitmap.hasGreyscale()) {
    file.close();
    // 1-bit or corrupt: single-pass is correct (dither already baked in).
    displayBw(true, "bw_1bit");
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
    logMultipassDone("leave_pre_store");
    return;
  }

  // Skip greys when heap is too tight for a full BW store + gray planes.
  // Logs showed repeated bw_oom_store at ~65KB free after reading — wastes ~3s.
  // Thresholds are conservative; settle on BW half instead of thrashing OOM.
  constexpr uint32_t kMinFreeForCoverMultipass = 90 * 1024;
  constexpr uint32_t kMinMaxAllocForCoverMultipass = 48 * 1024;
  const uint32_t freeHeap = ESP.getFreeHeap();
  const uint32_t maxAlloc = ESP.getMaxAllocHeap();
  if (freeHeap < kMinFreeForCoverMultipass || maxAlloc < kMinMaxAllocForCoverMultipass) {
    LOG_DBG("HOME", "Skip cover multipass (heap free=%u maxAlloc=%u)", static_cast<unsigned>(freeHeap),
            static_cast<unsigned>(maxAlloc));
    file.close();
    renderer.setRenderMode(GfxRenderer::BW);
    if (!leavingHome()) {
      displayBw(true, "bw_skip_low_heap");
    } else {
      logMultipassDone("leave_low_heap");
    }
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
      displayBw(true, "bw_oom_store");
    } else {
      logMultipassDone("leave_oom");
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
    logMultipassDone("abort_leave");
  };

  if (leavingHome()) {
    abortMultipass();
    return;
  }

  // Greys base: HALF for true midtones (default). Soft/FAST only when caller already
  // scrubbed the BW shell and set softGrayscaleBase (avoid a second hard flash).
  const bool softBase = softGrayscaleBase;
  softGrayscaleBase = false;
  if (softBase) {
    renderer.displayGrayscaleBase(HalDisplay::FAST_REFRESH);
    LOG_DBG("HOME", "multipass greys base=FAST (shell already scrubbed)");
  } else {
    renderer.displayGrayscaleBase(HalDisplay::HALF_REFRESH);
  }

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
  logMultipassDone("ok_half");
}

void HomeActivity::freeCoverBufferRamOnly() {
  if (coverBuffer) {
    free(coverBuffer);
    coverBuffer = nullptr;
  }
  coverBufferSize = 0;
  coverBufferStored = false;
}

void HomeActivity::releaseHeavyResourcesForReader() {
  freeCoverBufferRamOnly();
  coverRendered = false;
  // Keep coverGrayOnPanel so PopToHome can still prefer snappy FAST when greys
  // were already on glass; multipass will re-snapshot the cover on demand.
  LOG_DBG("HOME", "Released cover buffer for reader (heap free=%u)", static_cast<unsigned>(ESP.getFreeHeap()));
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
  // Penumbra is text-only (no cover greys). Other themes need settled multipass greys.
  const bool themeOk = paintedUiTheme == static_cast<int>(SETTINGS.uiTheme);
  leaveForUiChildSnappy = themeOk && (isPenumbraTheme() ? penumbraHalfBaselineDone : coverGrayOnPanel);
  skipResumeSdReload_ = recentsLoaded;
  cancelHomeBackgroundPaint();
}

void HomeActivity::cancelHomeBackgroundPaint() {
  // Drop deferred greys / deferred HALF so we do not flash after leaving Home.
  coverGrayNeedsRetry = false;
  deferredGreysOnly = false;
  // A pending HALF means the panel is still holding a FAST-only paint over
  // whatever was there before (a dense book page, typically). Cancelling it
  // without re-arming left that residual on glass — the "huge amount of ghosting
  // when exiting to menu". Hand the scrub back to whoever paints next.
  if (deferredHalfScrubOnly) {
    UiGhostPolicy::requestHardScrub();
    penumbraHalfBaselineDone = false;
  }
  deferredHalfScrubOnly = false;
  softGrayscaleBase = false;
  // A queued minute-tick must not start clock AA after we leave — that pass
  // displayGrayscaleBase + clearScreen(0x00) on the render task while Opening
  // displayWindow runs on main, hangs UC8253 BUSY for 30s, and leaves the
  // panel black (FAST first page then ghosts that black under the text).
  forcePenumbraClockRepaint = false;
  forceStatsUnderBoxRepaint = false;
  // Abort multipass between stages (checked in multipassHomeCoverGrayscale).
  cancelBackgroundPaint = true;
}

bool HomeActivity::handleForcedRefresh() {
  // Long-press power → Force Refresh. Must hard-scrub (X3 HALF+resync flash),
  // never soft FAST. Critical: if the home Menu is open, paint used to take
  // displayMenuFrame(FAST) and the user only saw a soft grey pull with grain
  // still on the plate (SUNDAY ghost, salt-and-pepper). Arm hard scrub and
  // force a full shell redraw so the next paint is a real flash clean.
  coverGrayOnPanel = false;
  coverRendered = false;
  forceHomeShellRepaint = true;
  penumbraHalfBaselineDone = false;
  homeMenuShellOnPanel = false;  // re-draw full menu/home plate, not band FAST
  paintedUiTheme = -1;
  snappyResumeNoGreys = false;
  deferredGreysOnly = false;
  softGrayscaleBase = false;
  cancelBackgroundPaint = false;
  coverGrayNeedsRetry = false;
  UiGhostPolicy::requestHardScrub();
  SystemLog::logTiming("HOME", "force_refresh (hard HALF scrub)");
  requestUpdateAndWait();
  return true;
}

// Index the most recently read book's page maps while Home is untouched.
//
// DISABLED (kBookIndexerEnabled = false). Kept, not deleted, because the pieces
// that work are worth keeping and the remaining blocker is already scheduled
// work.
//
// What was fixed and does work: the page-map walk is properly sliced, a device
// capture shows `HIDX | spine=9 pages=12 ms=6779 bursts=3` — twelve pages
// measured across three short bursts with input sampled between them. The maps
// it writes are also valid now that the render key is shared with the reader.
//
// What still does not: loading the chapter is one indivisible step and it costs
// 5-15 seconds, which is most of the total. From the same capture:
//
//   HIDX | spine=13 loaded cache=0 ms=12950
//   LOOP | activity_slow 13936ms
//   HIDX | spine=14 loaded cache=0 ms=14712
//   LOOP | activity_slow 15710ms
//
// Home stops sampling input for that whole window, which reads as a freeze, and
// free heap decays across passes (106K -> 59K) until the indexer starves below
// its own floor and abandons chapters half-done. Slicing the load needs
// HtmlToIr to convert incrementally, which is the streaming-converter work still
// outstanding. Until then this costs the user a frozen home screen and buys
// nothing they can perceive, so it stays off.
void HomeActivity::tickBookIndexer() {
  if constexpr (!kBookIndexerEnabled) return;
  if (!homeUiReady || recentsLoading || minimalMenuOpen) return;
  if (deferredHalfScrubOnly || deferredGreysOnly || coverGrayNeedsRetry || coverNeedsRetry) return;
  if (activityManager.hasPendingActivityChange() || cancelBackgroundPaint) return;
  if (recentBooks.empty()) return;

  // Any control held → not idle. Reset the timer so a scroll never gets
  // interrupted by a multi-second chapter convert starting underneath it.
  if (isAnyFrontButtonPressed(mappedInput) || mappedInput.isPressed(MappedInputManager::Button::Back) ||
      mappedInput.isPressed(MappedInputManager::Button::Confirm) ||
      mappedInput.isPressed(MappedInputManager::Button::Left) ||
      mappedInput.isPressed(MappedInputManager::Button::Right) ||
      mappedInput.isPressed(MappedInputManager::Button::Up) ||
      mappedInput.isPressed(MappedInputManager::Button::Down)) {
    indexerIdleSinceMs_ = millis();
    return;
  }

  const unsigned long now = millis();
  if (indexerIdleSinceMs_ == 0) {
    indexerIdleSinceMs_ = now;
    return;
  }
  if (now - indexerIdleSinceMs_ < kIndexIdleMs) return;
  if (lastIndexStepMs_ != 0 && (now - lastIndexStepMs_) < kIndexGapMs) return;

  // Last-read book: the one most likely to be continued.
  bookIndexer_.begin(recentBooks[0].path);
  lastIndexStepMs_ = millis();
  (void)bookIndexer_.step(renderer);
  lastIndexStepMs_ = millis();
}

void HomeActivity::loop() {
  // Home menu owns the panel — do not gen covers, multipass greys, or partial
  // home updates under it. (Deferred greys used to requestUpdate() even when
  // the menu was open; residual of home through a FAST menu plate looked like
  // home repainting behind the menu.)
  if (minimalMenuOpen) {
    forcePenumbraClockRepaint = false;
    forceStatsUnderBoxRepaint = false;
    deferredHalfScrubOnly = false;
    // Leave coverNeedsRetry / coverGrayNeedsRetry armed for after menu dismiss.
  } else if (homeUiReady && !recentsLoading) {
    // Penumbra text home: FB still holds the FAST shell — scrub residual with HALF
    // without a full redraw so first ink stayed snappy.
    if (deferredHalfScrubOnly && static_cast<long>(millis() - deferredHalfScrubAtMs) >= 0) {
      deferredHalfScrubOnly = false;
      if (!RenderLock::peek() && !activityManager.hasPendingActivityChange()) {
        RenderLock lock;
        if (activityManager.isCurrentActivity(this) && !minimalMenuOpen) {
          UiGhostPolicy::displayHalf(renderer);
          penumbraHalfBaselineDone = true;
          LOG_DBG("HOME", "Deferred HALF scrub complete");
        }
      } else {
        // Retry next tick if render mutex busy or a child is about to launch.
        deferredHalfScrubOnly = true;
        deferredHalfScrubAtMs = millis() + 100UL;
      }
      return;
    }
    // Penumbra X3 hero clock: live minute tick while idle on home.
    // Stacked activities never call this loop. Only the changing digits are
    // dirtied in redrawClockBlock (tight windowed refresh).
    if (isPenumbraTheme() && gpio.deviceIsX3() && !forcePenumbraClockRepaint && !deferredHalfScrubOnly) {
      char now[8];
      PenumbraThemeUi::formatHeroTimeNow(now, sizeof(now));
      if (now[0] != '\0' && (penumbraLastDrawnTime[0] == '\0' || strcmp(now, penumbraLastDrawnTime) != 0)) {
        forcePenumbraClockRepaint = true;
        requestUpdate();
        return;
      }
    }
    // Cover gen after first home paint so the Loading box floats over visible UI.
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
      if (deferredGreysOnly && recentsLoaded && coverRectW > 0 && coverRectH > 0 && usesHomeCoverMultipass()) {
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
      // Penumbra has no cover art — skip gen / multipass wait entirely.
      if (isPenumbraTheme()) {
        recentsLoaded = true;
        coverGrayOnPanel = true;
      } else {
        // Field #3: cover gen + UI font decompress under pressure → FDC fail / reboot.
        // Paint shell without covers first; arm retry when heap recovers.
        constexpr uint32_t kCoverGenMinFree = 40U * 1024U;
        constexpr uint32_t kCoverGenMinMaxAlloc = 20U * 1024U;
        const uint32_t freeH = ESP.getFreeHeap();
        const uint32_t maxA = ESP.getMaxAllocHeap();
        if (freeH < kCoverGenMinFree || maxA < kCoverGenMinMaxAlloc) {
          LOG_DBG("HOME", "defer cover gen free=%u maxAlloc=%u", static_cast<unsigned>(freeH),
                  static_cast<unsigned>(maxA));
          recentsLoaded = true;  // allow text shell paint this cycle
          coverNeedsRetry = true;
          coverRetryAtMs = millis() + 400UL;
          requestUpdate();
          // fall through to input + eventual shell paint
        } else {
          const auto& metrics = UITheme::getInstance().getMetrics();
          recentsLoading = true;
          loadRecentCovers(metrics.homeCoverHeight);
          return;
        }
      }
    }
  }

  // Idle background work. Runs before input handling so a step that decides to
  // act still leaves this frame's edges to be sampled next loop.
  tickBookIndexer();

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
      const bool busy =
          isAnyFrontButtonPressed(mappedInput) || mappedInput.isPressed(MappedInputManager::Button::Back) ||
          mappedInput.isPressed(MappedInputManager::Button::Confirm) ||
          mappedInput.isPressed(MappedInputManager::Button::Left) ||
          mappedInput.isPressed(MappedInputManager::Button::Right) ||
          mappedInput.isPressed(MappedInputManager::Button::Up) ||
          mappedInput.isPressed(MappedInputManager::Button::Down) || releasedFrontButton >= 0 || pressedFront >= 0;
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

      // Home chrome is position-based (Menu · Library · Recents · Read) while remap
      // still assigns logical functions to those same physical keys. Handle dismiss /
      // select first and suppress nav while the physical Menu key is busy so a
      // Bottom1→Left remap cannot scroll then exit on one press (Left + Menu).
      const bool physicalMenuReleased = releasedFrontButton == HalGPIO::BTN_BACK;
      const bool logicalBackReleased = mappedInput.wasReleased(MappedInputManager::Button::Back);
      if (physicalMenuReleased || logicalBackReleased) {
        minimalMenuOpen = false;
        homeMenuShellOnPanel = false;
        // Opening the menu aborts in-flight greys (cancelBackgroundPaint). That flag
        // must be cleared on dismiss — otherwise multipassHomeCoverGrayscale() bails
        // at leave_early without pushing the home FB and the menu stays on glass.
        cancelBackgroundPaint = false;
        // Menu was FAST; return home with FAST shell (snappy). Force Refresh
        // still hard-scrubs if residual builds up.
        softGrayscaleBase = false;
        coverGrayOnPanel = false;
        coverRendered = false;
        forceHomeShellRepaint = true;
        snappyResumeNoGreys = usesHomeCoverMultipass();
        penumbraHalfBaselineDone = false;
        UiGhostPolicy::clearHardScrub();
        requestUpdate();
        return;
      }

      const bool physicalConfirmReleased = releasedFrontButton == HalGPIO::BTN_CONFIRM;
      const bool logicalConfirmReleased = mappedInput.wasReleased(MappedInputManager::Button::Confirm);
      if (physicalConfirmReleased || logicalConfirmReleased) {
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
        return;
      }

      // Press only (no continuous). Skip while physical Menu is down/edge so its
      // remapped function (e.g. Left → NavPrevious) cannot dual-fire with Menu.
      const bool menuKeyBusy = mappedInput.isFrontButtonPressed(HalGPIO::BTN_BACK) ||
                               mappedInput.getPressedFrontButton() == HalGPIO::BTN_BACK;
      if (!menuKeyBusy) {
        buttonNavigator.onPreviousPress([this, menuCount] {
          minimalMenuIndex = ButtonNavigator::previousIndex(minimalMenuIndex, menuCount);
          requestUpdate();
        });
        buttonNavigator.onNextPress([this, menuCount] {
          minimalMenuIndex = ButtonNavigator::nextIndex(minimalMenuIndex, menuCount);
          requestUpdate();
        });
      }
      return;
    }

    // Physical side keys only (HalGPIO::BTN_UP / BTN_DOWN = hw 4 / 5).
    // Do not use remapped Left/Right here: a front key remapped to Left would
    // dual-fire with Menu, and frontChromeBusy gating also blocked real side
    // presses on X4. Sides always drive home side actions regardless of remap.
    auto sidePrevPressed = [] { return gpio.wasPressed(HalGPIO::BTN_UP); };
    auto sideNextPressed = [] { return gpio.wasPressed(HalGPIO::BTN_DOWN); };

    // Shelf + Stats Scroll: side keys step recent books (single-press).
    // Invalidate hero snapshot so the new book draws; free after flag so we do
    // not restore a stale cover under the new title/stats.
    if (usesRecentBookSideNav() && !recentBooks.empty()) {
      if (sidePrevPressed()) {
        shiftRecentFocus(-1);
        coverRendered = false;
        coverBufferStored = false;
        freeCoverBuffer();
        requestUpdate();
      } else if (sideNextPressed()) {
        shiftRecentFocus(1);
        coverRendered = false;
        coverBufferStored = false;
        freeCoverBuffer();
        requestUpdate();
      }
    }

    // Penumbra physical sides:
    //   X3 left/right sides → cycle under-panels (Title · Recents · Stats · Lifetime)
    //   X4 upper/lower sides → scroll on-screen Recents list
    if (isPenumbraTheme()) {
      auto runPenumbraSide = [this](const bool isPrev) {
        const int delta = isPrev ? -1 : 1;
        if (!gpio.deviceIsX3()) {
          if (penumbraRecentsListCount() > 0) {
            stepPenumbraRecentsFocus(delta);
            forceStatsUnderBoxRepaint = true;
            forcePenumbraClockRepaint = false;
          }
        } else if (PenumbraThemeUi::cycleUnderMode(delta)) {
          forceStatsUnderBoxRepaint = true;
          forcePenumbraClockRepaint = false;
        }
        requestUpdate();
      };
      if (sidePrevPressed()) {
        runPenumbraSide(true);
      } else if (sideNextPressed()) {
        runPenumbraSide(false);
      }
    }

    // All themes: Menu · Library · Recents · Read
    // X3 Penumbra on Recents under-panel: mid = Down (scroll list / View All).
    // X4: mid always opens full Recents; side Up/Down scroll the on-screen list.
    auto activateMinimalHomeNav = [this](int index) {
      switch (index) {
        case 0:  // Menu
          // Stop cover multipass / partial home work under the popup.
          cancelBackgroundPaint = true;
          forcePenumbraClockRepaint = false;
          forceStatsUnderBoxRepaint = false;
          minimalMenuOpen = true;
          minimalMenuIndex = 0;
          homeMenuShellOnPanel = false;
          requestUpdate();
          break;
        case 1:  // Library
          onFileBrowserOpen();
          break;
        case 2:  // Recents — or Down on X3 when under-panel is the recents list
          if (isPenumbraTheme() && PenumbraThemeUi::isRecentsUnderPanel() && gpio.deviceIsX3()) {
            if (penumbraRecentsFocusCount() > 0) {
              stepPenumbraRecentsFocus(1);
              forceStatsUnderBoxRepaint = true;
              forcePenumbraClockRepaint = false;
              requestUpdate();
            }
          } else {
            onRecentsOpen();
          }
          break;
        case 3:  // Read / Open
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
      if (mappedInput.isFrontButtonPressed(HalGPIO::BTN_BACK) && mappedInput.getHeldTime() >= LONG_PRESS_PRECANCEL_MS) {
        cancelBackgroundPaint = true;
      }
      if (mappedInput.isFrontButtonPressed(HalGPIO::BTN_BACK) && mappedInput.getHeldTime() >= READ_LONG_PRESS_MS) {
        menuLongPressFired = true;
        onSettingsOpen();
        return;
      }
    }

    if (releasedFrontButton == HalGPIO::BTN_BACK) {
      // After Back-from-reader, ignore short Menu for a short window so a double
      // mash does not open the in-home menu (user already asked to go Home).
      if (suppressMenuBackUntilMs != 0 && static_cast<long>(millis() - suppressMenuBackUntilMs) < 0) {
        return;
      }
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

    // Touch: tap cover (or Penumbra book block) to continue.
    const auto& metrics = UITheme::getInstance().getMetrics();
    if (!recentBooks.empty()) {
      if (isPenumbraTheme()) {
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
  // Penumbra: fully isolated text-only path. No cover multipass, no settle-skip
  // over a dirty panel, no shared Stats/Bare paint tail. Boot is clean; this must
  // match — black ink on native white only.
  // ---------------------------------------------------------------------------
  if (isPenumbraTheme()) {
    if (paintedUiTheme >= 0 && paintedUiTheme != static_cast<int>(SETTINGS.uiTheme)) {
      freeCoverBufferRamOnly();
      // X4 lands on Recents (#1); X3 lands on Title/Author under the clock.
      PenumbraThemeUi::resetUnderModeToDefault();
      penumbraHalfBaselineDone = false;  // new theme: re-HALF once
    }
    // Kill any deferred cover greys from a previous theme.
    deferredGreysOnly = false;
    coverGrayNeedsRetry = false;
    softGrayscaleBase = false;
    snappyResumeNoGreys = false;
    coverRectX = coverRectY = coverRectW = coverRectH = 0;
    freeCoverBufferRamOnly();

    const int clockTheme = static_cast<int>(CasperSettings::UI_THEME::PENUMBRA);
    PenumbraThemeUi::clampUnderModeToTracking();

    // Partial updates (side L/R / Recents Down) — never while menu owns the panel.
    if (minimalMenuOpen) {
      forcePenumbraClockRepaint = false;
      forceStatsUnderBoxRepaint = false;
    }
    const bool clockDirty = !minimalMenuOpen && forcePenumbraClockRepaint;
    const bool underDirty = !minimalMenuOpen && forceStatsUnderBoxRepaint;
    forcePenumbraClockRepaint = false;
    forceStatsUnderBoxRepaint = false;

    if ((clockDirty || underDirty) && paintedUiTheme == clockTheme && coverRendered) {
      // List focus is under-panel only. Upper Now Reading is always last-read (0).
      clampPenumbraRecentsFocus();
      const int listFocus = penumbraRecentsFocus;

      // Prefer under-only whenever the under-panel changed — even if the minute
      // clock also flipped — so scrolling Recents never rebinds the top title.
      // Recents % is cached at panel load only (no live SD refresh on scroll).
      if (underDirty) {
        const uint32_t t0 = millis();
        const Rect dirty = PenumbraThemeUi::redrawUnderPanel(renderer, recentBooks, listFocus, &currentBookStats,
                                                             currentBookProgressPercent, &globalStats);
        {
          const bool recentsPanel = PenumbraThemeUi::isRecentsUnderPanel();
          // X3 Recents page: Down. X4: always Recents (sides scroll the list).
          const char* mid = (recentsPanel && gpio.deviceIsX3()) ? tr(STR_DIR_DOWN) : tr(STR_RECENTS);
          const char* action = recentBooks.empty() ? "" : (recentsPanel ? tr(STR_OPEN) : tr(STR_READ));
          GUI.drawButtonHints(renderer, tr(STR_MENU), tr(STR_LIBRARY), mid, action);
        }
        const uint32_t tDraw = millis() - t0;
        // Plain FAST with global UI anti-ghost (no AA-pre mid-bank on BW home).
        const uint32_t t1 = millis();
        const int winTop = dirty.y;
        const int winH = std::max(1, pageHeight - winTop);
        UiGhostPolicy::displayHomeUnderUpdate(renderer, 0, winTop, pageWidth, winH);
        SystemLog::logTimed("HOME", millis() - t0, "penumbra_under focus=%d draw=%lums disp=%lums x3=%d", listFocus,
                            static_cast<unsigned long>(tDraw), static_cast<unsigned long>(millis() - t1),
                            gpio.deviceIsX3() ? 1 : 0);
        if (clockDirty) {
          forcePenumbraClockRepaint = true;
          requestUpdate();
        }
        homeUiReady = true;
        recentsLoaded = true;
        return;
      }

      // Clock-only: tight digit band (not full upper half). Prefer windowed
      // refresh so the rest of the home panel is not greyscale-flashed.
      if (gpio.deviceIsX3()) {
        char drawn[8];
        const Rect dirty = PenumbraThemeUi::redrawClockBlock(renderer, penumbraLastDrawnTime, drawn, sizeof(drawn));
        if (drawn[0] != '\0') {
          snprintf(penumbraLastDrawnTime, sizeof(penumbraLastDrawnTime), "%s", drawn);
        }
        if (dirty.width > 0 && dirty.height > 0) {
          const uint32_t tClock = millis();
          // Skip every panel write when leaving. Clock AA does a full-frame
          // greyscale base then clearScreen(0x00); even a tiny displayWindow
          // here races Opening's corner FAST and can wedge BUSY for 30s.
          const bool leave =
              cancelBackgroundPaint || activityManager.hasPendingActivityChange();
          if (leave) {
            SystemLog::logTiming("HOME", "penumbra_clock_digits skip (leave)");
          } else {
            if (!PenumbraThemeUi::displayClockAntiAliased(renderer, static_cast<int>(HalDisplay::FAST_REFRESH),
                                                          &dirty)) {
              renderer.displayWindow(dirty.x, dirty.y, dirty.width, dirty.height);
            }
            SystemLog::logTimed("HOME", millis() - tClock, "penumbra_clock_digits x=%d y=%d w=%d h=%d fre=%u", dirty.x,
                                dirty.y, dirty.width, dirty.height, static_cast<unsigned>(ESP.getFreeHeap()));
          }
        }
        homeUiReady = true;
        recentsLoaded = true;
        return;
      }

      homeUiReady = true;
      recentsLoaded = true;
      return;
    }

    // First land / leave another theme / menu dismiss: always repaint.
    // Same-theme idle: still allow settle skip only if we already painted clean
    // Penumbra (not a multipass theme's panel).
    const bool needPaint = forceHomeShellRepaint || minimalMenuOpen || paintedUiTheme != clockTheme || !coverRendered;
    forceHomeShellRepaint = false;

    if (!needPaint && !minimalMenuOpen) {
      homeUiReady = true;
      recentsLoaded = true;
      return;
    }

    if (minimalMenuOpen) {
      paintMinimalMenu(homeMenuShellOnPanel);
      paintedUiTheme = clockTheme;
      cancelBackgroundPaint = false;
      homeUiReady = true;
      recentsLoaded = true;
      return;
    }

    homeMenuShellOnPanel = false;
    renderer.clearScreen(0xFF);
    const bool chromeOnly = SETTINGS.systemStatusBarHas(CasperSettings::SYS_SLOT_BATTERY) ||
                            SETTINGS.systemStatusBarHas(CasperSettings::SYS_SLOT_CLOCK);
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
      // Penumbra: listFocus is under-panel only; stats/upper use last-read (index 0).
      if (isPenumbraTheme()) clampPenumbraRecentsFocus();
      const int listFocus = isPenumbraTheme() ? penumbraRecentsFocus : selectorIndex;
      GUI.drawRecentBookCover(renderer, Rect{0, 0, pageWidth, pageHeight}, recentBooks, listFocus, cr, cbs, br, noStore,
                              &currentBookStats, currentBookProgressPercent, &globalStats, nullptr);
      coverRendered = true;
      coverBufferStored = false;
    }
    {
      const bool recentsPanel = PenumbraThemeUi::isRecentsUnderPanel();
      const char* mid = (recentsPanel && gpio.deviceIsX3()) ? tr(STR_DIR_DOWN) : tr(STR_RECENTS);
      const char* action = recentBooks.empty() ? "" : (recentsPanel ? tr(STR_OPEN) : tr(STR_READ));
      GUI.drawButtonHints(renderer, tr(STR_MENU), tr(STR_LIBRARY), mid, action);
    }

    // Progress % already warmed in onResume (path-merge). Do not warm again —
    // a second pass was pure duplicate SD / log spam (serial: cache[0..4] twice).

    snappyResumeNoGreys = false;
    deferredHalfScrubOnly = false;
    const uint32_t tPenumbra = millis();
    // Back→Home arms hard scrub in onResume → HALF cleans residual. Other full
    // paints stay FAST when scrub is not armed. X3: greyscale multipass on the
    // 72pt clock band so 2-bit AA fringes land (BW-only looked jagged).
    // Resume: downgrade this paint to FAST and queue the HALF right behind it.
    // The scrub still happens (same anti-ghosting), it just happens with Home
    // already on glass rather than making the user wait ~4s for first pixels.
    const bool hard = UiGhostPolicy::hardScrubArmed();
    // Snappy return to Home used to defer the *scrub*: paint FAST now, HALF a
    // tick later. That produced ghosted words. On e-ink a FAST pass is a weak
    // differential update, so it settles the previous frame's residual into the
    // panel; a HALF afterwards then has to clear both the book page and the FAST
    // pass, and does not fully manage it. HALF applied directly does.
    //
    // So keep the HALF as the first and only paint, and defer the expensive part
    // instead: the full-screen greyscale clock multipass, which is what made this
    // ~4s (penumbra_full mode=HALF+clockAA took=4145ms). The clock re-renders
    // anti-aliased on the next tick through the windowed clock path, which is
    // both cheaper and invisible to someone waiting for Home to appear.
    const bool deferClockAa = deferScrubAfterFirstPaint_;
    deferScrubAfterFirstPaint_ = false;
    const int baseMode =
        hard ? static_cast<int>(HalDisplay::HALF_REFRESH) : static_cast<int>(HalDisplay::FAST_REFRESH);
    SystemLog::logTiming("HOME", "penumbra_full pre_disp mode=%s theme=%u fre=%u", hard ? "HALF" : "FAST",
                         static_cast<unsigned>(SETTINGS.uiTheme), static_cast<unsigned>(ESP.getFreeHeap()));
    // Clock greys can take 400–900ms — skip if user already navigated away.
    const bool leave = cancelBackgroundPaint || activityManager.hasPendingActivityChange();
    if (!leave && gpio.deviceIsX3() && !deferClockAa &&
        PenumbraThemeUi::displayClockAntiAliased(renderer, baseMode, /*dirtyOverride=*/nullptr)) {
      if (hard) UiGhostPolicy::noteHalf();
      penumbraHalfBaselineDone = true;
      SystemLog::logTimed("HOME", millis() - tPenumbra, "penumbra_full mode=%s+clockAA theme=%u fre=%u",
                          hard ? "HALF" : "FAST", static_cast<unsigned>(SETTINGS.uiTheme),
                          static_cast<unsigned>(ESP.getFreeHeap()));
    } else if (hard) {
      UiGhostPolicy::displayHalf(renderer);
      penumbraHalfBaselineDone = true;
      SystemLog::logTimed("HOME", millis() - tPenumbra, "penumbra_full mode=HALF%s theme=%u fre=%u",
                          deferClockAa ? "+deferAA" : "", static_cast<unsigned>(SETTINGS.uiTheme),
                          static_cast<unsigned>(ESP.getFreeHeap()));
    } else {
      UiGhostPolicy::displayFastFull(renderer);
      penumbraHalfBaselineDone = true;
      SystemLog::logTimed("HOME", millis() - tPenumbra, "penumbra_full mode=FAST theme=%u fre=%u",
                          static_cast<unsigned>(SETTINGS.uiTheme), static_cast<unsigned>(ESP.getFreeHeap()));
    }
    coverGrayOnPanel = true;
    paintedUiTheme = clockTheme;
    recentsLoaded = true;
    homeUiReady = true;
    if (deferClockAa) {
      // Hand the clock's greyscale pass to the windowed clock path on the next
      // tick. Clearing the remembered time makes redrawClockBlock treat the whole
      // band as dirty, so it re-renders anti-aliased over a Home that is already
      // on glass — the same pixels, without the multi-second wait for first ink.
      penumbraLastDrawnTime[0] = '\0';
      forcePenumbraClockRepaint = true;
      requestUpdate();
    } else {
      PenumbraThemeUi::formatHeroTimeNow(penumbraLastDrawnTime, sizeof(penumbraLastDrawnTime));
      forcePenumbraClockRepaint = false;
    }
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
      PenumbraThemeUi::resetUnderModeToDefault();
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
      paintMinimalMenu(homeMenuShellOnPanel);
      cancelBackgroundPaint = false;
      homeUiReady = true;
      return;
    }

    // Clear first: cover snapshot is cover-art only, not the full tile, so we
    // cannot rely on a large restore to erase the previous frame (e.g. Menu).
    renderer.clearScreen();
    // Penumbra never restores a cover snapshot (text-only home).
    bool bufferRestored = false;
    if (!isPenumbraTheme() && coverBufferStored) {
      bufferRestored = restoreCoverBuffer();
    }

    // Top chrome (battery icon+% / clock). Bare / Penumbra default chrome off;
    // Stats always draws the status-bar band (same plate packing on X3 + X4).
    const bool textOnlyHome = isBareTheme() || isPenumbraTheme();
    const bool chromeOnlyMinimal = textOnlyHome && (SETTINGS.systemStatusBarHas(CasperSettings::SYS_SLOT_BATTERY) ||
                                                    SETTINGS.systemStatusBarHas(CasperSettings::SYS_SLOT_CLOCK));
    // Bare / Penumbra: header only when the user has enabled battery/clock.
    if (!textOnlyHome || chromeOnlyMinimal) {
      const int headerTop = textOnlyHome ? 0 : metrics.topPadding;
      const int headerH =
          BaseTheme::kTopChromeBatteryY + std::max(metrics.batteryHeight + 8, metrics.statusBarVerticalMargin);
      GUI.drawHeader(renderer, Rect{0, headerTop, pageWidth, headerH}, nullptr);
    }

    // When greys will run next, only record the art box — do not malloc a cover
    // snapshot. X3 leave-reader multipass OOMs if ~15–30KB is still held for storeBw.
    // Penumbra has no cover multipass.
    const bool willMultipass =
        usesHomeCoverMultipass() && recentsLoaded && !coverGrayOnPanel && !minimalMenuOpen && !snappyResumeNoGreys;
    auto storeCover = [this, willMultipass](int x, int y, int w, int h) -> bool {
      if (isPenumbraTheme()) {
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

    // Rect is a full-screen hint; themes pack cover/title (or Penumbra) in that band.
    {
      if (isPenumbraTheme()) clampPenumbraRecentsFocus();
      const int listFocus = isPenumbraTheme() ? penumbraRecentsFocus : selectorIndex;
      GUI.drawRecentBookCover(renderer, Rect{0, 0, pageWidth, pageHeight}, recentBooks, listFocus, coverRendered,
                              coverBufferStored, bufferRestored, storeCover, &currentBookStats,
                              currentBookProgressPercent, &globalStats, nullptr);
    }

    // Bare / Penumbra / Stats. X3 Recents under-panel: mid = Down; X4: Recents.
    {
      const bool recentsPanel = isPenumbraTheme() && PenumbraThemeUi::isRecentsUnderPanel();
      const char* midHint = (recentsPanel && gpio.deviceIsX3()) ? tr(STR_DIR_DOWN) : tr(STR_RECENTS);
      const char* action = recentBooks.empty() ? "" : (recentsPanel ? tr(STR_OPEN) : tr(STR_READ));
      GUI.drawButtonHints(renderer, tr(STR_MENU), tr(STR_LIBRARY), midHint, action);
    }

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
      // Hard scrub — FAST left Settings residual on the home plate.
      UiGhostPolicy::displayHalf(renderer);
      return;
    }
    // Legacy snappy-resume path (shell now, greys later) — kept for rare callers
    // that still arm snappyResumeNoGreys. Prefer multipass-on-first-paint above.
    if (snappyResumeNoGreys && recentsLoaded) {
      snappyResumeNoGreys = false;
      homeMenuShellOnPanel = false;
      coverRendered = true;
      coverGrayOnPanel = false;
      paintedUiTheme = static_cast<int>(SETTINGS.uiTheme);
      freeCoverBufferRamOnly();
      deferredGreysOnly = true;
      softGrayscaleBase = true;  // multipass uses FAST greys-base (no second HALF)
      coverGrayNeedsRetry = true;
      // Short defer so Back feels instant; greys base is soft (not a second scrub).
      coverGrayRetryAtMs = millis() + 250UL;
      if (UiGhostPolicy::hardScrubArmed()) {
        UiGhostPolicy::displayHalf(renderer);
        LOG_DBG("HOME", "Home shell HALF scrub; greys deferred soft");
      } else {
        UiGhostPolicy::displayFastFull(renderer);
        LOG_DBG("HOME", "Home shell FAST; greys deferred soft");
      }
      return;
    }
    snappyResumeNoGreys = false;
    // Thumbs not ready yet: BW shell first so Loading can float over real chrome.
    // When bindExistingHeroThumbsIfReady already set recentsLoaded (A1), multipass now.
    if (!recentsLoaded) {
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
      UiGhostPolicy::noteHalf();
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
  if (recentsLoaded && coverGrayOnPanel && coverRendered && paintedUiTheme == static_cast<int>(SETTINGS.uiTheme)) {
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

  // Build menu items dynamically — File Transfer, then Settings.
  std::vector<const char*> menuItems = {tr(STR_BROWSE_FILES), tr(STR_MENU_RECENT_BOOKS), tr(STR_FILE_TRANSFER)};
  std::vector<UIIcon> menuIcons = {Folder, Recent, Transfer};
  menuItems.push_back(tr(STR_SETTINGS_TITLE));
  menuIcons.push_back(Settings);

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
    UiGhostPolicy::noteHalf();
    return;
  }
  multipassHomeCoverGrayscale();
}

void HomeActivity::onSelectBook(const std::string& path) {
  // Abort home multipass so waitForRenderIdle is not a full grey pass.
  // cancelBackgroundPaint only — do NOT use markLeavingForUiChild() here.
  // Multipass cover themes: force hard multipass on return (FAST after dense page
  // text ghosts on Bare covers). Penumbra is pure BW home chrome — keep snappy so
  // resume can FAST-redraw (~0.45s) instead of HALF (~3.2s on X3 every Back).
  cancelHomeBackgroundPaint();
  leaveForUiChildSnappy = isPenumbraTheme();
  // Reader updates progress / last-read — onResume must reload recents + stats.
  skipResumeSdReload_ = false;
  snappyResumeNoGreys = false;

  // Bare/Dashboard multipass leaves greys in controller RAM. Opening a book with
  // FAST on that residual paints "Loading" or first page as salt-and-pepper /
  // glitched glyphs. Tear greys down and force a hard scrub before reader ink.
  const bool coverTheme = usesHomeCoverMultipass();
  const bool greysSettled = coverGrayOnPanel;
  if (coverTheme) {
    renderer.setRenderMode(GfxRenderer::BW);
    renderer.restoreBwBuffer();  // no-op if multipass never stored BW
    renderer.cleanupGrayscaleWithFrameBuffer();
    coverGrayOnPanel = false;
  }

  // v0.1.5-style open: cached book.bin → FAST first ink, no home HALF, no
  // "Opening" paint. Controller greys already torn down above when coverTheme.
  // Cold index miss: short status only (reader builds book.bin).
  bool bookIndexReady = true;
  if (FsHelpers::hasEpubExtension(path)) {
    const std::string cacheDir = Epub(path, CasperPaths::kPackageCacheRoot).getCachePath();
    bookIndexReady = Storage.exists((cacheDir + "/book.bin").c_str());
  }
  // Only force a panel scrub when greys are still mid-flight (not settled) —
  // never HALF a clean home just because the theme multipasses covers.
  const bool greysDirty = coverTheme && !greysSettled;
  const bool preferFast = bookIndexReady && !greysDirty;
  // Wait out any in-flight Home paint BEFORE touching the panel. Opening's
  // windowed FAST on the main task raced the render-task clock AA (log:
  // opening_status then penumbra_clock_digits 30048ms = BUSY timeout) and
  // left the plate black; FAST first page then ghosted that black under text.
  const uint32_t t0 = millis();
  activityManager.waitForRenderIdle();
  LOG_DBG("HOME", "Read open: waitIdle %lums greysSettled=%d bookIndex=%d preferFast=%d penumbra=%d coverTheme=%d",
          static_cast<unsigned long>(millis() - t0), greysSettled ? 1 : 0, bookIndexReady ? 1 : 0, preferFast ? 1 : 0,
          isPenumbraTheme() ? 1 : 0, coverTheme ? 1 : 0);

  // Show "Opening" for EVERY book open. Do not gate on Dark Mode — the cue is the
  // only feedback between Confirm and first ink, and windowed refresh keeps it
  // visible through the activity swap.
  GUI.drawTopLeftStatus(renderer, tr(STR_STATUS_OPENING), /*refresh=*/true);
  SystemLog::logTiming("HOME", "opening_status painted dark=%d", SETTINGS.readerDarkMode ? 1 : 0);
  cancelBackgroundPaint = false;

  // Defer AA only when we scrubbed or cold-opened; warm FAST open can AA next turn.
  ReaderActivity::setOpenHints(/*preferFastFirstRefresh=*/preferFast, /*deferFirstPageTextAa=*/!preferFast);
  SystemLog::logTiming("HOME", "read_open preferFast=%d greysSettled=%d bookIndex=%d penumbra=%d cover=%d",
                       preferFast ? 1 : 0, greysSettled ? 1 : 0, bookIndexReady ? 1 : 0, isPenumbraTheme() ? 1 : 0,
                       coverTheme ? 1 : 0);
  activityManager.goToReader(path);
}

void HomeActivity::reloadHomeAfterBookAction() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  loadRecentBooks(metrics.homeRecentBooksCount);
  if (selectorIndex >= static_cast<int>(recentBooks.size())) {
    selectorIndex = std::max(0, static_cast<int>(recentBooks.size()) - 1);
  }
  loadFocusedRecentStats();
  // Mark finished / clear cache / delete can change % — full SD refresh of rows.
  if (isPenumbraTheme()) {
    PenumbraThemeUi::invalidateRecentsProgressCache();
    if (!recentBooks.empty()) {
      PenumbraThemeUi::warmRecentsProgressCache(recentBooks);
    }
  }
  if (SETTINGS.readingStatsTrackingEnabled()) {
    globalStats = GlobalReadingStats::load();
  } else {
    globalStats = GlobalReadingStats{};
  }
  freeCoverBuffer();
  coverRendered = false;
  coverBufferStored = false;
  coverGrayOnPanel = false;
  // Book/cache actions may have deleted thumbs — allow a fresh gen pass.
  recentsLoaded = false;
  recentsLoading = false;
  skipResumeSdReload_ = true;  // this method already reloaded; onResume must not
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

int HomeActivity::penumbraRecentsListCount() const {
  if (recentBooks.empty()) return 0;
  // Must match PenumbraTheme penumbraRecentsListCap(): X3 draws ≤4 books + View All;
  // X4 draws ≤5 books and no View All. Using metrics.homeRecentBooksCount (5) on X3
  // left an extra focus slot after View All (needed two Downs to wrap).
  int maxN = UITheme::getInstance().getMetrics().homeRecentBooksCount;
  if (isPenumbraTheme() && gpio.deviceIsX3()) {
    maxN = 4;
  } else if (isPenumbraTheme() && !gpio.deviceIsX3()) {
    maxN = 5;
  }
  return std::min(static_cast<int>(recentBooks.size()), std::max(1, maxN));
}

// Focus slots: books, plus View All on X3 Penumbra Recents under-panel only.
int HomeActivity::penumbraRecentsFocusCount() const {
  const int books = penumbraRecentsListCount();
  if (books <= 0) return 0;
  if (gpio.deviceIsX3() && isPenumbraTheme() && PenumbraThemeUi::isRecentsUnderPanel()) {
    return books + 1;  // last slot = View All (X3); indices 0..books-1 books, books = View All
  }
  return books;
}

bool HomeActivity::penumbraViewAllFocused() const {
  if (!gpio.deviceIsX3() || !isPenumbraTheme() || !PenumbraThemeUi::isRecentsUnderPanel()) {
    return false;
  }
  const int books = penumbraRecentsListCount();
  return books > 0 && penumbraRecentsFocus == books;
}

void HomeActivity::clampPenumbraRecentsFocus() {
  const int n = penumbraRecentsFocusCount();
  if (n <= 0) {
    penumbraRecentsFocus = 0;
    return;
  }
  if (penumbraRecentsFocus < 0 || penumbraRecentsFocus >= n) {
    penumbraRecentsFocus = 0;
  }
}

void HomeActivity::stepPenumbraRecentsFocus(const int delta) {
  const int n = penumbraRecentsFocusCount();
  if (n <= 0) {
    penumbraRecentsFocus = 0;
    return;
  }
  // Wrap: last slot (View All on X3) + Down → 0; 0 + Up → last slot.
  int next = penumbraRecentsFocus + delta;
  next %= n;
  if (next < 0) next += n;
  penumbraRecentsFocus = next;
}

void HomeActivity::loadFocusedRecentStats() {
  if (recentBooks.empty()) {
    currentBookStats = BookReadingStats{};
    currentBookProgressPercent = -1.0f;
    penumbraRecentsFocus = 0;
    return;
  }
  // Penumbra: upper Now Reading + Stats always describe the last-read book (0).
  // List scrolling uses penumbraRecentsFocus and does not change these stats.
  const int idx = isPenumbraTheme() ? 0 : focusedRecentIndex();
  const auto& book = recentBooks[static_cast<size_t>(idx)];
  currentBookStats = loadRecentBookStats(book);
  currentBookProgressPercent = currentBookStats.getProgressPercent();
  clampPenumbraRecentsFocus();
}

void HomeActivity::shiftRecentFocus(const int delta) {
  if (recentBooks.empty()) return;
  const int n = static_cast<int>(recentBooks.size());
  // Wrap within the loaded recents window.
  int next = focusedRecentIndex() + delta;
  next %= n;
  if (next < 0) next += n;
  selectorIndex = next;
  loadFocusedRecentStats();
}

void HomeActivity::showCurrentBookActionMenu(const bool ignoreInitialConfirmRelease) {
  if (recentBooks.empty()) {
    return;
  }
  // Abort greys and queue the child immediately. ActivityManager waits for the
  // render task before Push — a second wait here made long-press feel stuck
  // (especially during Penumbra clock AA / Bare multipass).
  markLeavingForUiChild();
  cancelBackgroundPaint = true;
  // Penumbra list focus when on Recents under-panel; else last-read / bare focus.
  // View All is not a book — open the full Recents list instead.
  if (isPenumbraTheme() && penumbraViewAllFocused()) {
    onRecentsOpen();
    return;
  }
  int menuIdx = focusedRecentIndex();
  if (isPenumbraTheme()) {
    clampPenumbraRecentsFocus();
    menuIdx = PenumbraThemeUi::isRecentsUnderPanel() ? penumbraRecentsFocus : 0;
    menuIdx = std::clamp(menuIdx, 0, penumbraRecentsListCount() - 1);
  }
  const RecentBook book = recentBooks[static_cast<size_t>(menuIdx)];
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
  if (recentBooks.empty()) return;
  // View All: open the full Recent Books activity.
  if (isPenumbraTheme() && penumbraViewAllFocused()) {
    onRecentsOpen();
    return;
  }
  // Penumbra Recents under-panel: open the list focus. Otherwise last-read (index 0)
  // so scrolling the list never changes which book "Read" opens from other pages.
  int idx = 0;
  if (isPenumbraTheme() && PenumbraThemeUi::isRecentsUnderPanel()) {
    clampPenumbraRecentsFocus();
    idx = penumbraRecentsFocus;
  } else if (!isPenumbraTheme()) {
    idx = focusedRecentIndex();
  }
  idx = std::clamp(idx, 0, penumbraRecentsListCount() - 1);
  idx = std::clamp(idx, 0, static_cast<int>(recentBooks.size()) - 1);
  onSelectBook(recentBooks[static_cast<size_t>(idx)].path);
}

void HomeActivity::onFileBrowserOpen() {
  markLeavingForUiChild();
  // Do not waitForRenderIdle here — ActivityManager does before Push.
  activityManager.goToFileBrowser();
}

void HomeActivity::onRecentsOpen() {
  markLeavingForUiChild();
  activityManager.goToRecentBooks();
}

void HomeActivity::onSettingsOpen() {
  // Cancel greys; queue Settings without blocking the hold on idle wait.
  markLeavingForUiChild();
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
