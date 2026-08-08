#pragma once
#include <Epub.h>
#include <Epub/FootnoteEntry.h>
#include <Epub/Section.h>
#include <I18n.h>

#include <optional>

#include "BookmarkEntry.h"
#include "BookReadingStats.h"
#include "EndOfBookOptions.h"
#include "EpubReaderMenuActivity.h"
#include "GlobalReadingStats.h"
#include "ProgressMapper.h"
#include "ReaderUtils.h"
#include "activities/Activity.h"

class EpubReaderActivity final : public Activity {
  std::shared_ptr<Epub> epub;
  // Minimal reading-stats session tracking for Dashboard (fail-soft).
  BookReadingStats readingStats;
  GlobalReadingStats globalReadingStats;
  unsigned long readingSessionStartMs = 0;
  // Smoothed book time-left for the status bar (mutable: updated from const format helper).
  // Dampens page-to-page jumps from chapter density / dwell noise.
  mutable uint32_t smoothedBookTimeLeftSeconds = 0;
  std::unique_ptr<Section> section = nullptr;
  int currentSpineIndex = 0;
  int nextPageNumber = 0;
  std::optional<uint16_t> pendingPageJump;
  // Set when navigating to a footnote href with a fragment (e.g. #note1).
  // Cleared on the next render after the new section loads and resolves it to a page.
  std::string pendingAnchor;
  int pagesUntilFullRefresh = 0;
  // Image pages use a dedicated double-FAST refresh path, so retain a manual
  // refresh request until renderContents can issue its clean base pass.
  bool forcedRefreshPending = false;
  int cachedSpineIndex = 0;
  int cachedChapterTotalPageCount = 0;
  // Set when pageTurn advances past the current watermark while a section is still
  // building. If the chapter then finishes with no further pages, render goes to the
  // next spine instead of clamping back to the same last page (looks like "next
  // reformatted the page" / stuck page).
  bool pendingForwardPastEnd = false;
  // Chapter hop requested while render held the lock (0 = none, +1/-1). Applied
  // under RenderLock from loop() or pageTurn when the lock is free — never block
  // main on a hung grey multipass.
  int8_t pendingChapterHop = 0;
  unsigned long lastPageTurnTime = 0UL;
  unsigned long pageTurnDuration = 0UL;
  // Absorbs bounce so one physical press cannot advance multiple pages.
  ReaderUtils::PageTurnLatch pageTurnLatch;
  // Signals that the next render should reposition within the newly loaded section
  // based on a cross-book percentage jump.
  bool pendingPercentJump = false;
  // Normalized 0.0-1.0 progress within the target spine item, computed from book percentage.
  float pendingSpineProgress = 0.0f;
  // Clipping jump: stored page index is best-effort; render may re-resolve via text match.
  uint16_t pendingParagraphIndex = UINT16_MAX;
  uint16_t pendingClippingIndex = UINT16_MAX;
  bool pendingScreenshot = false;
  bool pendingSyncSaveError = false;
  // Consecutive page-load failures. Each failure drops the section and rebuilds on the next render,
  // which recovers a transiently corrupt cache; capped so a persistently bad page can't spin forever.
  uint8_t pageLoadRetryCount = 0;
  static constexpr uint8_t MAX_PAGE_LOAD_RETRIES = 3;
  bool skipNextButtonCheck = false;  // Skip button processing for one frame after subactivity exit
  bool automaticPageTurnActive = false;
  bool showBookmarkMessage = false;
  // "No dictionary set" popup, shown when a lookup is triggered without a configured dictionary.
  bool showDictionaryMessage = false;
  StrId dictionaryMessageId = StrId::STR_DICT_NO_DICT_SET;
  unsigned long dictionaryMessageTime = 0UL;
  // When non-zero, wall-clock time while menus/dict are open is excluded from
  // session reading totals and page-pace samples.
  unsigned long statsPauseStartMs = 0UL;
  bool ignoreNextConfirmRelease = false;
  bool currentPageBookmarked = false;
  // Idle-time glyph prewarm: after a page settles, scan the LIKELY next page
  // (scan mode draws nothing) and load glyphs into the font cache (kept until
  // the next turn when heap allows). One attempt per position.
  int idlePrewarmSpine = -1;
  int idlePrewarmPage = -1;  // page that was *current* when idle prewarm ran
  // Spine/page whose glyphs are currently retained in FontCacheManager (or -1).
  int glyphCacheSpine = -1;
  int glyphCachePage = -1;
  unsigned long lastRenderCompleteMs = 0;

  // Next-spine pre-index (Background B): page-budget runway of later spines so
  // chapter turns hit a finished section cache with full Embedded Style.
  // Original Casper design — not a copy of another firmware.
  enum class NeighborBuildState : uint8_t { Idle, Probe, Building, Settled };
  std::unique_ptr<Section> neighborSection_;
  int neighborSpineIndex_ = -1;
  int neighborBaseSpine_ = -1;       // reading spine when window was anchored
  int neighborPagesBuilt_ = 0;       // pages already laid out in later spines
  NeighborBuildState neighborState_ = NeighborBuildState::Idle;
  unsigned long neighborHeapGateMs_ = 0;
  // WH imageProcessingActive_: while images are decoding for paint, do not start
  // neighbor layout (font/PNG heap fight → PTX OOM / freezes).
  bool imageProcessingActive_ = false;
  static constexpr int NEIGHBOR_LOOKAHEAD_PAGES = 40;
  static constexpr size_t NEIGHBOR_CSS_MIN_FREE_HEAP = 56 * 1024;
  static constexpr size_t NEIGHBOR_CSS_MIN_MAX_ALLOC = 24 * 1024;
  // Only start neighbor builds when HTML is already on SD (inflate under lock freezes UI).
  void resetNeighborBuild();
  void stepNeighborSectionBuild();
  bool bookmarkRemoved = false;  // true when last toggle removed (controls popup text)
  std::vector<BookmarkEntry> cachedBookmarks;
  // Tracks whether this book is currently removed from Recent Books by the
  // removeReadBooksFromRecents feature (set at End-of-Book, cleared if paged back in).
  bool recentsEntryRemoved = false;
  unsigned long bookmarkMessageTime = 0UL;
  // Set when end-of-book / Mark Finished needs a Finished Books move on exit
  // (or as a retry if an immediate move failed). Consumed in onExit().
  bool pendingReadFolderMove = false;
  // Next-book suggestion menu for the End-of-Book screen
  EndOfBookOptions endOfBookOptions;

  // Footnote support
  std::vector<FootnoteEntry> currentPageFootnotes;
  struct SavedPosition {
    int spineIndex;
    int pageNumber;
  };
  static constexpr int MAX_FOOTNOTE_DEPTH = 3;
  SavedPosition savedPositions[MAX_FOOTNOTE_DEPTH] = {};
  int footnoteDepth = 0;

  // Viewport of the last render(), captured so loop()'s lazy partial-extension start
  // builds with IDENTICAL layout parameters to the pages already rendered (a mismatch
  // would paginate differently than the partial being extended). 0 = no render yet.
  uint16_t buildViewportWidth = 0;
  uint16_t buildViewportHeight = 0;
  // Set when the lazy extension start failed, so loop() doesn't retry (and log) every
  // tick; the blocking extension in render() remains the fallback past the watermark.
  bool partialRebuildStartFailed = false;

  // Last position persisted by render()'s saveProgress, used to skip redundant
  // writeAtomic calls on no-op re-renders (menu/bookmark/screenshot).
  int lastSavedSpineIndex = -1;
  int lastSavedPage = -1;
  int lastSavedPageCount = -1;

  // Non-critical open I/O (recents write, bookmarks, clippings, global stats)
  // deferred until after the first page paints so reopen feels snappier.
  bool pendingOpenSideWork = false;
  void runDeferredOpenSideWork();

  // Snappy open (Home Read): first page FAST when greys settled; first ink BW-only
  // with text AA scheduled as a follow-up render so the page is readable sooner.
  bool openPreferFastFirstRefresh = false;
  bool openDeferTextAa = false;
  // Next paint should run deferred text greys (not skip them again).
  bool forceGreysThisFrame = false;
  bool pendingDeferredOpenAa = false;
  // Image-plate greys: at most one idle catch-up per page. Re-arming after a
  // successful (or failed) attempt caused black full-frame flashes every few
  // seconds on Alice illustration pages.
  bool imageGreysSettledForPage = false;
  int imageGreysSettledSpine = -1;
  int imageGreysSettledPage = -1;
  // Set when INDEXING/Loading chrome was drawn this open — first page must scrub
  // (HALF) or residual popup greys wash out the text until a page turn.
  bool openNeedsScrubAfterChrome = false;
  uint32_t openWallStartMs = 0;
  bool openFirstInkLogged = false;

  void renderContents(std::unique_ptr<Page> page, int orientedMarginTop, int orientedMarginRight,
                      int orientedMarginBottom, int orientedMarginLeft);
  void drawClippingHighlights(const Page& page, int fontId, int orientedMarginTop, int orientedMarginLeft) const;
  void renderStatusBar() const;
  // Fills buf with a compact time-left label ("12m · Book") or "Learning Pace...".
  // Returns false when the setting is hide or there is nothing useful to show.
  bool formatTimeLeftLabel(char* buf, size_t len, bool bookEstimate) const;
  // Pages laid out per incremental-build pump: on the render path (catching up to the page
  // being shown) and per loop() tick (background build of a large chapter). Kept small so a
  // background build chunk never noticeably delays input or a pending render.
  static constexpr int BUILD_PAGES_PER_CHUNK = 8;
  // Keep small: each page under RenderLock must not starve Home/buttons (logs: activity_slow ~2.6s).
  static constexpr int BACKGROUND_BUILD_PAGES_PER_TICK = 1;
  // Wall-clock cap for one background build step while holding RenderLock.
  static constexpr unsigned long BACKGROUND_BUILD_LOCK_BUDGET_MS = 60;

  // MEMFIX-PORT: background-build heap floor; portable
  // Skip background build ticks below this free-heap floor. The parse path grows
  // word vectors of heap strings — throwing allocations that abort() on OOM under
  // -fno-exceptions (field crash: bad_alloc in ParsedText::addWord during a
  // background tick under heap pressure). The tick is deferrable work:
  // page-turn transients free up between turns and the build resumes; the render
  // path still builds the page it actually needs regardless of this floor.
  static constexpr size_t BACKGROUND_BUILD_MIN_FREE_HEAP = 32 * 1024;
  // Fragmentation floor for the same gate: a tick passed the free-heap floor at
  // 34.7 KB free but the largest block was ~11 KB, and a parse allocation inside the
  // tick aborted anyway. Free heap says how much memory exists; maxAlloc says whether
  // any single allocation can actually have it. 16 KB also keeps the advance-table
  // batch path (16 KB scratch) viable during builds.
  static constexpr size_t BACKGROUND_BUILD_MIN_MAX_ALLOC = 16 * 1024;
  // Open-path (blocking build-to-target): prefer smaller chunks under pressure so
  // ParsedText / CSS parse never grows huge vectors in one pump (field abort #1/#2).
  static constexpr size_t OPEN_BUILD_TIGHT_FREE_HEAP = 48 * 1024;
  static constexpr size_t OPEN_BUILD_TIGHT_MAX_ALLOC = 20 * 1024;
  // Pages per chunk when heap is tight (vs BUILD_PAGES_PER_CHUNK).
  static constexpr int OPEN_BUILD_PAGES_TIGHT = 2;
  // Gate for a background build tick: true when the heap can take parse allocations.
  // Updates buildHeapPaused as a side effect.
  bool buildTickHeapGate();
  // Adaptive chunk size for render-path build pumps (open / page catch-up).
  int openBuildPagesPerChunk() const;
  // Suspend mid-chapter build + drop retained glyphs before stacking a child UI
  // (menu/dict/etc.) so nested activities have headroom.
  void parkHeavyWorkForChild();
  // True when free heap can keep page glyph buffers after paint / idle prewarm.
  static bool canRetainGlyphCache();
  // True while the background build is gated on the heap floors. Lets skipLoopDelay()
  // return the loop to normal delay/power-saving during the pause: isBuilding() stays
  // true the whole time, and without this the loop would spin at full CPU speed doing
  // no build work — indefinitely, if the build context itself keeps the heap low.
  bool buildHeapPaused = false;
  // Heap floor for optional render-adjacent work (idle prewarm). Page
  // deserialization (TextBlock word vectors/strings) and glyph caching allocate
  // through throwing paths that abort() on OOM; skip deferrable work below it.
  static constexpr size_t RENDER_MIN_FREE_HEAP = 24 * 1024;
  // How many pages to keep laid out ahead of the reader for a still-building section. A page
  // turn is ~1s on e-ink and a page builds in ~30ms, so the reader can't out-click the builder
  // -- a tiny buffer is enough. The background build stops once the watermark is this far
  // ahead and resumes as the reader advances; building unbounded instead locked up input by
  // monopolizing the RenderLock. A giant single-spine book therefore never finalizes its .bin
  // in one sitting -- instant reopen comes from Section::suspendBuild() persisting the pages
  // already laid out as a partial file on exit/sleep.
  static constexpr int BUILD_WINDOW_AHEAD = 5;
  // Reopening a partial does NOT immediately restart its extension build (a whole-chapter
  // re-layout from page 0 -- minutes of background CPU + SD writes on a giant spine, wasted
  // when the reader never crosses the watermark that session). Instead loop() starts it once
  // the reader is within this many pages of the watermark: at ~30s per page read and ~100-300ms
  // per page rebuilt, this margin gives the rebuild ample runway to catch up (and finalize)
  // before the reader arrives.
  static constexpr int PARTIAL_REBUILD_START_MARGIN = 15;
  // Show the indexing popup when an initial build must lay out more than this many pages up front
  // (a deep resume/jump into a not-yet-built section), so it isn't a silent wait. Kept independent
  // of the small look-ahead window so ordinary landings stay popup-free.
  static constexpr int BUILD_POPUP_PAGE_THRESHOLD = 20;
  // Also show the popup when first building a spine larger than this (uncompressed bytes): its
  // whole HTML must be inflated before page 1 can lay out (the giant single-spine case), which is
  // a multi-second wait. Normal chapters are well under this and stay popup-free.
  static constexpr size_t BUILD_POPUP_BYTE_THRESHOLD = 96 * 1024;
  // Deadline backstop: surface INDEXING sooner so long opens are never a silent freeze
  // (quick reopen / deep layout without predictive popup was ~30s of blank home).
  static constexpr unsigned long BUILD_POPUP_DEADLINE_MS = 400;
  // True only during onEnter's blocking build-to-target phase, until the popup has been
  // drawn. Gates showBuildPopup() so the parser's popup callback (which persists into
  // background buildSomeMore chunks) can never draw over a displayed page.
  bool buildPopupPending = false;
  // Draw the indexing popup mid-build (parser image-probe callback and deadline backstop).
  void showBuildPopup();
  // Remap the cached relative reading position once the section's real page count is known
  // (used after a settings change re-paginates a chapter). Returns true if currentPage moved.
  // No-op while the section is still building or when the pagination is unchanged (plain resume).
  bool applyDeferredReposition();
  // Layout-affecting fields from Manage Fonts / Text Settings (cache key + margin).
  uint32_t layoutFingerprint() const;
  // After Text Settings: if layout changed, drop section.bin and rebuild with Loading.
  void reflowAfterTextSettings(uint32_t fingerprintBefore);
  bool saveProgress(int spineIndex, int currentPage, int pageCount);
  // Jump to a percentage of the book (0-100), mapping it to spine and page.
  void jumpToPercent(int percent);
  void onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action);
  // Opens the reader menu for the current position (short-press Confirm)
  void openReaderMenu();
  void openDictionaryWordSelect();
  void startClipSelection();
  void handleClippingJump(const ClippingJumpResult& clipping);
  void openBookStats();
  // Returns true if sync acted (launched, or surfaced a save error); false if it was a no-op
  // because no KOReader credentials are stored.
  bool launchKOReaderSync();
  // Book-leave KOReader behavior (Ask confirm / Smart / Percent / Time).
  bool tryStartAutoKoUpload();
  // Shared launch after gates/confirm; uploadOnly=true for Percent/Time leave path.
  bool launchLeaveKoSync(bool uploadOnly);
  // Prefer leave-sync when applicable; otherwise onGoHome().
  void leaveReaderToHome();
  float getCurrentBookProgressPercent() const;
  // Write progress % into the book's stats file for Home Recents bars (X4-safe).
  void persistHomeProgressPercent();
  void applyOrientation(uint8_t orientation);
  void toggleAutoPageTurn(uint8_t selectedPageTurnOption);
  void pageTurn(bool isForwardTurn);
  void loadCachedBookmarks();
  void addBookmark();
  void updateBookmarkFlag();
  void setBookCompleted(bool isCompleted);

  // Footnote navigation
  void navigateToHref(const std::string& href, bool savePosition = false);
  void restoreSavedPosition();

  // Exclude wall time spent in menus / dictionary from session totals and pace.
  void pauseReadingStatsClock();
  void resumeReadingStatsClock();
  void resetReadingPaceData();

  // After a child (Text Settings, dictionary, BT, …) finishes on Back press, the
  // release edge would hit handleBackNavigation and leave the book. Wait until
  // Back is idle and drain residual edges before treating Back as leave-home.
  bool awaitChildButtonRelease = false;

 public:
  explicit EpubReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Epub> epub)
      : Activity("EpubReader", renderer, mappedInput), epub(std::move(epub)) {}
  void onEnter() override;
  void startActivityForResult(std::unique_ptr<Activity>&& activity, ActivityResultHandler resultHandler) override;
  void onExit() override;
  void onResume() override;
  void loop() override;
  void render(RenderLock&& lock) override;
  // Full CPU speed + fast loop ticks while a section build runs: at the low-power
  // frequency a giant chapter's background rebuild stretches from ~40s to many
  // minutes, so the reader exits before it can finalize and the next open restarts
  // it from page 0. Reverts to normal power behavior the moment the build finishes,
  // and while the build is heap-paused (no work is happening, so spinning at full
  // speed would only burn battery; the paused gate still retries every loop pass).
  bool skipLoopDelay() override {
    // Full speed while current or neighbor spine is actively building.
    if (section && section->isBuilding() && !buildHeapPaused) return true;
    if (neighborSection_ && neighborSection_->isBuilding() && !buildHeapPaused) return true;
    return false;
  }
  bool isReaderActivity() const override { return true; }
  bool handleForcedRefresh() override {
    {
      RenderLock lock(*this);
      // FORCE_SCRUB (0) → hard HALF on X3/X4. pagesUntilFullRefresh=1 used the
      // X3 soft reinforce path (no visible flash) and felt broken.
      pagesUntilFullRefresh = CrossPointSettings::REFRESH_COUNTDOWN_FORCE_SCRUB;
      forcedRefreshPending = true;
    }
    requestUpdate();
    return true;
  }
  ScreenshotInfo getScreenshotInfo() const override;
  CrossPointPosition getCurrentPosition() const;
};
