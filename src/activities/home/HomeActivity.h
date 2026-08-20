#pragma once
#include <functional>
#include <vector>

#include "./FileBrowserActivity.h"
#include "activities/Activity.h"
#include "activities/reader/BookReadingStats.h"
#include "activities/reader/GlobalReadingStats.h"
#include "util/ButtonNavigator.h"
#include "util/HomeBookIndexer.h"

struct RecentBook;
struct Rect;

class HomeActivity final : public Activity {
  ButtonNavigator buttonNavigator;
  int selectorIndex = 0;
  bool recentsLoading = false;
  bool recentsLoaded = false;
  // First home paint finished — cover gen waits so Loading can float over real UI.
  bool homeUiReady = false;
  // Transient thumb-gen failures (heap/decode) schedule a deferred retry so we do
  // not burn the render path every frame, but also do not give up forever.
  bool coverNeedsRetry = false;
  uint8_t coverGenAttempts = 0;
  unsigned long coverRetryAtMs = 0;
  static constexpr uint8_t kMaxCoverGenAttempts = 3;
  bool hasOpdsServers = false;
  bool coverRendered = false;      // Track if cover has been rendered once
  bool coverBufferStored = false;  // Track if cover buffer is stored
  // True after a successful gray multipass while this Home instance still owns the panel.
  // Cleared on exit, cover free, or any BW-only home refresh (menu popup).
  // Used to avoid stacking multipass flashes from cover-gen retries / path-only updates.
  bool coverGrayOnPanel = false;
  // uiTheme value that last completed a successful cover multipass. Theme switch
  // (Stats ↔ Bare / Penumbra) must not settled-skip with the old greys/layout.
  int paintedUiTheme = -1;
  // storeBwBuffer OOM / aborted multipass: retry greys without thrashing every frame.
  bool coverGrayNeedsRetry = false;
  unsigned long coverGrayRetryAtMs = 0;
  // Return from Settings / Library / etc.: one FAST BW shell first (snappy), then
  // deferred multipass greys restore jacket quality without blocking the return.
  bool snappyResumeNoGreys = false;
  // In-home Menu is on glass (BW popup). Keeps paintedUiTheme so Back can snappy-
  // return without a full multipass; also gates windowed cursor repaints.
  bool homeMenuShellOnPanel = false;
  // Set before leaving for a UI child so onResume uses snappy path.
  bool leaveForUiChildSnappy = false;
  // After snappy BW shell is on the panel: multipass greys only (no clear/redraw).
  bool deferredGreysOnly = false;
  // Penumbra / text home: FAST first ink on resume, then HALF scrub without redraw
  // so Back→Home is ~0.4s not ~2s of multipass. Timer armed after FAST paints.
  bool deferredHalfScrubOnly = false;
  unsigned long deferredHalfScrubAtMs = 0;
  // Clock AA is a full-frame greyscale pass (HalDisplay windowed grey is a no-op).
  // Run it only after this idle window so Read can cancel it first.
  static constexpr unsigned long kClockAaIdleMs = 400;
  bool pendingClockAaAfterIdle_ = false;
  unsigned long lastHomeInputMs_ = 0;
  // Minute tick: BW window only. Unchanged digits keep the last AA raster.
  bool forcePenumbraClockBwOnly_ = false;
  // Soft FAST grayscale base (panel already shows matching BW shell).
  bool softGrayscaleBase = false;
  // Abort in-flight multipass between stages (Recents/Settings must not freeze
  // waiting for a full grey pass, and must not race loadBookDescription).
  bool cancelBackgroundPaint = false;
  // Home can be entered while Back is still held (e.g. leaving Settings with
  // Back): ignore that stale release until a fresh press is seen here.
  bool backPressSeen = false;
  // Classic themes: do not arm Resume-on-Back until Back has been fully idle
  // after onEnter. Prevents the leave-from-reader edge (or same-frame touch
  // back gesture reporting pressed+released) from bouncing straight back into
  // the book — which feels like "Back needs two presses to stay on home".
  bool backResumeArmed = false;
  // Minimal/Dashboard: direct front-button actions + optional popup menu.
  bool minimalMenuOpen = false;
  bool minimalSuppressInitialFrontRelease = false;
  // After return from reader/Settings: ignore short Back→Menu for a short window so
  // a second mash of Back does not open the in-home menu (feels like "need 2 Backs").
  unsigned long suppressMenuBackUntilMs = 0;
  // Long-press Read (BTN_RIGHT) opens the same book-action menu as Recent Books.
  bool readLongPressFired = false;
  // Long-press Menu (BTN_BACK) opens Settings.
  bool menuLongPressFired = false;
  // Stats: side Left/Right toggled under-box title ↔ lifetime.
  bool forceStatsUnderBoxRepaint = false;
  // Resume path: paint the shell FAST, then run the anti-ghost HALF just after,
  // so Home is visible while it scrubs instead of the user staring at the old
  // frame for the whole refresh.
  bool deferScrubAfterFirstPaint_ = false;
  // Cold onEnter: FAST first, then arm deferred HALF after that paint (not at
  // enter — a 1s FAST would make millis()-atMs already due).
  bool armDeferredHalfAfterFirstPaint_ = false;

  // Whole-book page-map indexing, run only while Home sits idle. See
  // HomeBookIndexer: Home is the one place no chapter is resident, so indexing
  // costs nothing to evict and never disturbs a page turn or reading-pace stats.
  HomeBookIndexer bookIndexer_;
  unsigned long indexerIdleSinceMs_ = 0;
  unsigned long lastIndexStepMs_ = 0;
  // Home must be untouched this long before indexing may take the bus, and this
  // long between chapters so input is always sampled in between.
  // Background whole-book indexing. Off until HtmlToIr can convert a chapter
  // incrementally — see tickBookIndexer for the measurements that closed it.
  static constexpr bool kBookIndexerEnabled = false;
  static constexpr unsigned long kIndexIdleMs = 8000;
  static constexpr unsigned long kIndexGapMs = 600;
  void tickBookIndexer();

  // Penumbra (X3): windowed digit-only (or clock-block) refresh — no full-frame flash.
  bool forcePenumbraClockRepaint = false;
  // Last hero time string drawn on panel ("H:MM"); used for minute-change detect.
  char penumbraLastDrawnTime[8] = "";
  // Penumbra Recents under-panel list focus (independent of upper "last read" book).
  int penumbraRecentsFocus = 0;
  // After one HALF this theme session, further full Penumbra shells use the
  // snappy path (UiGhostPolicy may still promote FAST→HALF every N UI paints).
  bool penumbraHalfBaselineDone = false;

 public:
  // After onEnter when seeding Home under the reader (QR/cold open): mark state
  // so PopToHome onResume takes the snappy FAST path instead of a new-Home HALF
  // multipass (~5s on X3). Does not paint.
  void markSnappyResumeReady();
  // QR/cold open: push Home under the reader with zero SD / paint (critical path).
  // Full recents/stats load happens on first onResume (Back from book).
  void seedUnderReader();

 private:
  // Book-action menu (etc.) dismissed: must paint home over the child FB once.
  bool forceHomeShellRepaint = false;
  // seedUnderReader: defer loadRecentBooks / stats until first onResume.
  bool deferredEnterLoad_ = false;
  // Light UI child (book action sheet / Settings / Library) did not change
  // recents. onResume skips the SD cluster that showed up as activity_slow
  // 2226ms before first ink on X3 v36 (47e06f62). Cleared when opening a book
  // or after a mutating book action (those already reloaded).
  bool skipResumeSdReload_ = false;
  int minimalMenuIndex = 0;
  uint8_t* coverBuffer = nullptr;  // HomeActivity's own buffer for cover image
  size_t coverBufferSize = 0;      // Bytes allocated to coverBuffer
  // Cover-art snapshot region set by the theme's storeCoverBuffer(x,y,w,h).
  // Intentionally the drawn cover only — not the full home tile (~15 KB vs ~40 KB).
  int coverRectX = 0;
  int coverRectY = 0;
  int coverRectW = 0;
  int coverRectH = 0;
  std::vector<RecentBook> recentBooks;
  BookReadingStats currentBookStats;
  GlobalReadingStats globalStats;
  float currentBookProgressPercent = -1.0f;
  const HomeMenuItem initialMenuItem;

  // Convert HomeMenuItem to menu index (used in onEnter)
  static int menuItemToIndex(HomeMenuItem item, bool hasOpdsUrl) {
    int i = 0;
    if (item == HomeMenuItem::FILE_BROWSER) return i;
    ++i;
    if (item == HomeMenuItem::RECENTS) return i;
    ++i;
    if (item == HomeMenuItem::OPDS_BROWSER) return hasOpdsUrl ? i : 0;
    if (hasOpdsUrl) ++i;
    if (item == HomeMenuItem::FILE_TRANSFER) return i;
    ++i;
    if (item == HomeMenuItem::SETTINGS_MENU) return i;
    return 0;
  }

  // Convert menu index to HomeMenuItem (used in loop)
  static HomeMenuItem indexToMenuItem(int idx, bool hasOpdsUrl) {
    int i = 0;
    if (idx == i++) return HomeMenuItem::FILE_BROWSER;
    if (idx == i++) return HomeMenuItem::RECENTS;
    if (hasOpdsUrl && idx == i++) return HomeMenuItem::OPDS_BROWSER;
    if (idx == i++) return HomeMenuItem::FILE_TRANSFER;
    if (idx == i) return HomeMenuItem::SETTINGS_MENU;
    return HomeMenuItem::NONE;
  }
  void onSelectBook(const std::string& path);
  void onContinueReading();
  void onFileBrowserOpen();
  void onRecentsOpen();
  void onSettingsOpen();
  void onFileTransferOpen();
  void onOpdsBrowserOpen();
  void showCurrentBookActionMenu(bool ignoreInitialConfirmRelease = false);
  void reloadHomeAfterBookAction();
  // Stats Recents home: which recent book drives the hero + shelf highlight.
  int focusedRecentIndex() const;
  void loadFocusedRecentStats();
  void shiftRecentFocus(int delta);
  // Penumbra Recents under-panel: book rows drawn (X3 ≤4, X4 ≤5).
  int penumbraRecentsListCount() const;
  // Focus slots: books (+ View All on X3 Recents under-panel only).
  int penumbraRecentsFocusCount() const;
  // True when focus is on X3 "View All" (not a book).
  bool penumbraViewAllFocused() const;
  // Step list focus within the focus window (wraps).
  void stepPenumbraRecentsFocus(int delta);
  // Keep penumbraRecentsFocus in range after reloads / theme changes.
  void clampPenumbraRecentsFocus();

  int getMenuItemCount() const;
  bool storeCoverBuffer();    // Store frame buffer for cover image
  bool restoreCoverBuffer();  // Restore frame buffer from stored cover
  void freeCoverBuffer();     // Free the stored cover buffer
  // Free snapshot RAM only (keep settle flags). Used when leaving for a child
  // so resume can choose snappy vs full multipass.
  void freeCoverBufferRamOnly();
  // Mark next onResume as snappy (no multipass black flash).
  void markLeavingForUiChild();
  // Stop deferred greys and abort multipass between stages (fast UI handoff).
  void cancelHomeBackgroundPaint();
  // 2-bit hero covers: BW base + LSB/MSB gray planes (sleep-screen style).
  // Without this, midtones paint solid black on a single BW half-refresh.
  // Keep this path aligned with v0.1.3 (HALF base → planes → restore) — no
  // post-multipass paper window experiments (those regressed splotch).
  void multipassHomeCoverGrayscale();
  void loadRecentBooks(int maxBooks);
  void loadRecentCovers(int coverHeight);

  void paintMinimalMenu(bool bandOnly);

 public:
  explicit HomeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                        HomeMenuItem initialMenuItemValue = HomeMenuItem::NONE)
      : Activity("Home", renderer, mappedInput), initialMenuItem(initialMenuItemValue) {}
  void onEnter() override;
  void onExit() override;
  // Restored from stack after Read (Phase 2) — refresh stats, multipass once, keep thumbs.
  void onResume() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isHomeActivity() const override { return true; }
  // Drop large cover snapshot while Home is parked under the reader (snappy Back
  // keeps Home on the stack — without this, ~10–40 KB stays reserved all session).
  void releaseHeavyResourcesForReader();
  // Power long-press FORCE_REFRESH: full redraw with HALF scrub (Penumbra) or
  // cover multipass (Bare). Must not FAST-only — that leaves grey mud on glass.
  bool handleForcedRefresh() override;
};
