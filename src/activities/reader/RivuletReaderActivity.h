#pragma once

#include <Epub.h>
#include <Epub/FootnoteEntry.h>
#include <RivuletEngine.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "BookReadingStats.h"
#include "BookmarkEntry.h"
#include "GlobalReadingStats.h"
#include "ProgressFlushPolicy.h"
#include "ReaderUtils.h"
#include "activities/Activity.h"
#include "activities/ActivityResult.h"

// Rivulet EPUB reader (CROSSPOINT_RIVULET_READER=1).
// Layout engine: lib/Rivulet. Ownership: /.crosspoint/book_<stableId>/ for progress,
// stats, and IR (path-independent id + ledger — see util/CrossPointBookStore).
class RivuletReaderActivity final : public Activity {
 public:
  RivuletReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::shared_ptr<Epub> epub);
  ~RivuletReaderActivity() override = default;

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
  void persistProgressForSleep() override;
  bool handleForcedRefresh() override;
  ScreenshotInfo getScreenshotInfo() const override;

 private:
  // startPage: layout that page after load (0 = chapter start). Avoids goToStart+goToPage double work.
  // requireCompleteIr: prev-chapter last-page path — refuse partial OOM IR (false chapter end).
  bool loadSpine(int spineIndex, int startPage = 0, bool requireCompleteIr = false);
  // Load TOC target spine only. May try a few spines that belong to the same TOC
  // entry (empty/cover fragments) — never walks into the next chapter's range.
  // On failure restores the previous spine so the reader never lands on Empty page.
  bool loadTocChapter(int tocSpineIndex, int startPage = 0);
  // Drop chapter IR / image decode cache / font advances so convert has maxAlloc.
  // aggressive=true always clears font cache (prev-chapter full convert).
  void prepareHeapForChapterLoad(bool aggressive = false);
  // Book menu / settings: save place, free chapter IR + caches so UI/settings
  // have contiguous heap. Place kept in heldSpine_/heldPage_; epub_ stays.
  void releaseHeavyForUi();
  // Reload IR + page after UI. showLoading: corner status on the *current*
  // orientation (callers that already switched orientation must pass false and
  // paint Loading before applyOrientation). No-op if still resident.
  bool restoreAfterUi(bool showLoading = true);
  void configureRenderKey();
  void showError(const char* msg);
  void renderStatusBar() const;
  void openReaderMenu();
  void onReaderMenuAction(int action);
  bool turnNext(int skipPages = 1);
  bool turnPrev(int skipPages = 1);
  float bookProgress01() const;
  using ProgressFlush = progressflush::Mode;
  bool saveProgress(ProgressFlush flush = ProgressFlush::Now) const;
  void tickDeferredProgress();
  void loadProgress(int& outSpine, int& outPage);
  void persistHomeProgress(bool writeToDisk);
  void noteForwardPageTurn();
  void jumpToPercent(int percent);
  void openBookStats();
  void openDictionary();
  void openClippingList();
  // Word-range Clipping Tool (double-press Menu / Create Clipping menu).
  void openClippingTool();
  // Manage Reader UI: time-left slots (same model as classic EpubReader).
  bool formatTimeLeftLabel(char* buf, size_t len, bool bookEstimate) const;
  const std::string& crosspointDir() const { return crosspointBookDir_; }
  // Resolve img hrefs, probe dims, scale to viewport; rewrite IR paths.
  void prepareChapterImages(const std::string& spineHref);
  void paintPageImages();
  static bool extractEpubItem(void* ctx, const char* srcPath, const char* destPath);
  bool fireMenuShortcut(uint8_t function);
  bool tryLongPressShortcut(uint8_t function, bool& suppressRelease);
  bool trySideLongPressShortcut();
  // Side long-press: cycle all orientations, or flip Portrait ↔ Flip With.
  void cycleReadingOrientation(bool nextTriggered);
  void flipReadingOrientation();
  void applyReadingOrientation(uint8_t newOrientation);
  // Re-layout the current spine under the render key already on the engine.
  // keepCount must be the *old* viewport estimate, captured before setRenderKey.
  bool relayoutChapterForViewport(int keepSpine, int keepPage, int keepCount);
  // Paint a center popup on the page already on glass. Do not follow with
  // requestUpdate — that races the X3 HALF refresh and wipes the message.
  void flashHeldReaderPopup(const char* msg);
  // Chapter skip: land at chapter start (next) or previous chapter last page / this chapter start.
  void chapterSkipNext();
  void chapterSkipPrev();

  // Bookmarks: path-keyed JSON under /.crosspoint/bookmarks (same files as classic).
  void loadCachedBookmarks();
  void updateBookmarkFlag();
  void toggleBookmark();
  void jumpToBookmarkProgress(const ProgressChangeResult& sync);
  std::string pageSummaryForBookmark() const;
  std::string currentPagePlainText(size_t maxChars = 1200) const;

  void ensureClippingsLoaded();
  void paintClippingHighlights();
  void ensureChapterFootnotes();
  void refreshPageFootnotes();
  // Paint-only: underline note markers on the page (legacy style). No layout change.
  void paintFootnoteMarkers();
  void openFootnotesMenu();
  void navigateToHref(const std::string& href, bool savePosition);
  void restoreFootnotePosition();
  void setBookCompleted(bool completed);
  bool launchKOReaderSync(bool leaveToHome = false, bool uploadOnly = false);
  bool launchLeaveKoSync(bool uploadOnly);
  bool tryStartAutoKoUpload();
  void leaveReaderToHome();
  // Back release → home even while future-chapter index owns the engine.
  [[nodiscard]] bool tryHandleBackExit();
  [[nodiscard]] bool backHeldLeaving() const;
  // Progress + session stats to SD (leave path and onExit).
  void flushExitProgressAndStats();
  void takeReaderScreenshot();
  // Persist current chapter page map when complete (idle or turn).
  void persistPageMapIfComplete();
  // Idle: extend current-spine page map a few pages (B). No full-book map (D).
  void tickIdlePageMap();
  // After open/spine land: index ~10 pages ahead (+ behind RAM) for fast turns.
  void warmOpenNavigationWindow();
  [[nodiscard]] bool spineHasPageMap(int spine) const;
  [[nodiscard]] bool spineHasIrCache(int spine) const;
  // Immediate next readable spine that still needs IR or page-1 cache.
  // Already prefetched (IR + page 0 .rvpg) or skipped → none.
  [[nodiscard]] int nextForwardUnmappedSpine() const;
  // After the current chapter map is sealed: convert the next spine to IR and
  // lay out page 1 onto SD so a hop is deserialize + paint. Evicts the resident
  // IR; restore before any mid-chapter tap (see FutureChapterIndex.h).
  void tickFutureChapterIndex();
  bool startFutureChapterIndex();
  void restoreAfterFutureIndex(bool forUser);
  void persistFutureMap(bool completeOnly);
  [[nodiscard]] bool futureIndexUserWantsControl() const;
  // PageForward during last-page swap: keep the next IR (this IS the hop).
  [[nodiscard]] bool futureIndexForwardHeld() const;
  [[nodiscard]] bool futureIndexBackHeld() const;
  void promoteFutureIndexToCurrent();
  // Hop: paint page 1 from .rvpg with no IR in RAM, then load IR after ink.
  [[nodiscard]] bool tryHopToCachedFirstPage(int targetSpine);
  void tickPendingChapterIr();
  void finishPendingChapterIr();
  void bindEnginePageCacheDir();
  void persistPageMapBestEffort();
  void pinnedReaderPlace(int& spine, int& page) const;
  // Layout the next page without touching the framebuffer (safe during async
  // FAST). Glyph prewarm of that page waits until idle — scan-paint writes FB.
  void overlapAheadDuringRefresh();
  void prewarmAheadGlyphs();
  // Rewrite the current page into the FB without a panel refresh. Idle glyph
  // prewarm scans the *next* page then used to leave the buffer white; QR sleep
  // then FASTed that white plate over the live page (v52 white sleep).
  void paintCurrentPageToFramebuffer();

  // Text + overlays only, in whatever render mode is active. Shared by the BW
  // paint and the greyscale AA passes (images are already 1-bit plates baked
  // into the BW frame, so re-decoding them under the ~48 KB AA hold is what
  // aborted image-heavy pages).
  void paintTextLayerForAa();
  // Re-run the AA passes over the page already on glass.
  //
  // First ink deliberately paints BW so the page appears fast, and a transient
  // heap dip can decline AA mid-book as well. ReaderActivity.h has advertised
  // "first ink is BW only; AA catch-up render follows" since deferFirstPageTextAa
  // was introduced, but nothing ever performed that follow-up — the page just
  // stayed un-smoothed until the next turn. This is the missing render, and it
  // is what produces the "text appears, then sharpens a moment later" behaviour.
  void tickAaCatchUp();
  void scheduleAaCatchUp();
  // Keep page glyph buffers only when free/maxAlloc leave room for next turn/UI.
  static bool canRetainGlyphCache();

  std::shared_ptr<Epub> epub_;
  rivulet::RivuletEngine engine_;
  int imageCounter_ = 0;
  // Spine/page whose glyph page-buffer was retained after last paint (skip rescan).
  int glyphCacheSpine_ = -1;
  int glyphCachePage_ = -1;
  ReaderUtils::PageTurnLatch pageTurnLatch_;
  BookReadingStats readingStats_;
  GlobalReadingStats globalReadingStats_;
  std::vector<BookmarkEntry> cachedBookmarks_;
  std::vector<FootnoteEntry> chapterFootnotes_;  // marker-like note links (# + 1/* /a), not TOC titles
  std::vector<FootnoteEntry> currentPageFootnotes_;
  int footnoteCacheSpine_ = -1;
  // Set by loadSpine; first ink of a new chapter skips the HTML note scan.
  bool footnoteScanDeferred_ = false;
  bool imgCacheDirReady_ = false;
  static constexpr int kMaxFootnoteDepth = 4;
  struct SavedPos {
    int spine = 0;
    int page = 0;
  };
  SavedPos footnoteStack_[kMaxFootnoteDepth]{};
  int footnoteDepth_ = 0;
  std::string stableId_;
  std::string crosspointBookDir_;  // /.crosspoint/book_<id>
  std::string irDir_;              // crosspointBookDir_/rivulet
  int spineIndex_ = 0;
  int marginX_ = 16;
  int marginY_ = 16;
  int marginR_ = 16;
  int marginB_ = 16;
  int pagesUntilFullRefresh_ = 0;
  unsigned long readingSessionStartMs_ = 0;
  unsigned long lastPageTurnTime_ = 0;
  // FIDX quiet clock. 0 until a real forward turn — onEnter used to stamp this
  // so opening a book and sitting still started a next-chapter IR swap.
  unsigned long lastForwardTurnMs_ = 0;
  unsigned long firstInkAtMs_ = 0;
  unsigned long lastConfirmReleaseMs_ = 0;
  unsigned long lastIdleMapMs_ = 0;
  mutable uint32_t smoothedBookTimeLeftSeconds_ = 0;
  bool ready_ = false;
  bool error_ = false;
  bool firstPaint_ = true;
  bool ignoreNextConfirmRelease_ = false;
  // After a side long-press shortcut fires while held, ignore that key's release
  // so it does not also page-turn (same pattern as Confirm).
  bool ignoreNextSideRelease_ = false;
  // Chapter land: next ink must stay FAST. A HALF after PageBack was 3.2s and
  // felt like a freeze (device: spine 21 p21 refresh=3257ms, then no exit).
  bool forceFastAfterChapterNav_ = false;
  bool pendingConfirmMenuOpen_ = false;
  bool pageMapDirty_ = false;  // map grew since last SD save
  // True while walking prev-chapter to true last page — render shows Loading only
  // (yield mid-walk must not paint intermediate pages onto the glass).
  bool chapterNavBusy_ = false;
  // Guard re-entry while warmPreviousSpinePageMap temporarily loads another spine.
  bool warmingAdjacent_ = false;
  // Set once the first page is on glass: only then may the idle tick spend time
  // indexing the adjacent chapter. Never index before first ink.
  bool firstInkDone_ = false;
  // Deferred/declined AA owes the current page a greyscale pass — see
  // tickAaCatchUp. Cleared as soon as AA actually runs for that page.
  bool aaCatchUpPending_ = false;
  // Set by the catch-up tick, consumed by the next render: overrides the
  // first-ink/scrub terms that declined AA, so the repaint it asked for is
  // actually allowed to anti-alias.
  bool forceAaThisRender_ = false;
  unsigned long aaCatchUpAtMs_ = 0;
  uint8_t aaCatchUpTries_ = 0;
  // Page the retry budget belongs to; moving to another page refills it.
  int aaCatchUpSpine_ = -1;
  int aaCatchUpPage_ = -1;
  // Long enough that the BW page is unambiguously on glass first (the point of
  // deferring), short enough to read as the same action.
  static constexpr unsigned long kAaCatchUpDelayMs = 350;
  // Heap may still refuse; retry a couple of times, then leave the page BW
  // rather than spin a full-screen greyscale attempt forever.
  static constexpr uint8_t kAaCatchUpMaxTries = 3;
  // storeBwBuffer needs the 48KB snapshot plus this so menus/turns still fit.
  static constexpr size_t kAaPaintHeadroom = 12 * 1024;
  // Future-chapter idle map. Engine holds that spine's IR; glass still shows
  // the reader's page. saveProgress / turns must use heldSpineForFuture_.
  bool futureIndexActive_ = false;
  int futureIndexSpine_ = -1;
  int heldSpineForFuture_ = 0;
  int heldPageForFuture_ = 0;
  bool heldAtChapterEndForFuture_ = false;
  unsigned long lastFutureWorkMs_ = 0;
  int futureStallTicks_ = 0;
  int futureIndexedThisSession_ = 0;
  // Tap restored the reading IR — do not StartForward again until spine changes.
  bool futureIndexAbortedThisSitting_ = false;
  // Partial / unloadable spine — do not retry this session.
  int futureSkipSpine_ = -1;
  int futurePartSaveAtKnown_ = 0;
  // Spine whose IR still needs to land after a cached first-page hop. -1 = none.
  int pendingChapterIrLoad_ = -1;
  bool currentPageBookmarked_ = false;
  bool clippingsLoaded_ = false;
  bool pendingScreenshot_ = false;
  bool pendingOpenStateSave_ = false;  // flush APP_STATE after first ink
  bool pendingRecentsTouch_ = false;   // RECENT_BOOKS.addBook after first ink
  bool pendingStatsLoad_ = false;      // CrossPointStats after first ink (QR open)
  // leaveReaderToHome already wrote progress/stats under "Saving stats" chrome.
  bool leaveExitFlushed_ = false;
  // True after releaseHeavyForUi() until restoreAfterUi() reloads the chapter.
  bool heavyReleasedForUi_ = false;
  int heldSpineForUi_ = 0;
  int heldPageForUi_ = 0;
  // Last position actually written by saveProgress(), so repeat calls for an
  // unchanged position are skipped. saveProgress() is invoked from ~20 sites
  // (menu open/close, sleep entry, orientation change, bookmark, KO sync, exit)
  // and each call is a ProgressFile::writeAtomic — several FAT operations for
  // six bytes. The classic reader guarded this the same way; the rewrite lost
  // it, which put redundant SD writes on the paths the user feels and burns
  // erase cycles for nothing (Resource Protocol rule 8).
  // -1 = nothing written yet this session.
  mutable int lastSavedSpine_ = -1;
  mutable int lastSavedPage_ = -1;
  mutable int lastSavedPageCount_ = -1;
  // In-chapter turns schedule a write; sleep / leave / chapter hop flush Now.
  mutable progressflush::State progressFlush_;
  std::string errorMsg_;
};
