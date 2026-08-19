#pragma once

#include <Epub.h>
#include <Epub/FootnoteEntry.h>
#include <RivuletEngine.h>

#include <memory>
#include <string>
#include <vector>

#include "BookReadingStats.h"
#include "BookmarkEntry.h"
#include "GlobalReadingStats.h"
#include "ReaderUtils.h"
#include "activities/Activity.h"
#include "activities/ActivityResult.h"

// Rivulet EPUB reader (CASPER_RIVULET_READER=1).
// Layout engine: lib/Rivulet. Ownership: /.crosspoint/book_<stableId>/ for progress,
// stats, and IR (path-independent id + ledger — see util/CasperBookStore).
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
  bool saveProgress() const;
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
  const std::string& casperDir() const { return casperBookDir_; }
  // Resolve img hrefs, probe dims, scale to viewport; rewrite IR paths.
  void prepareChapterImages(const std::string& spineHref);
  void paintPageImages();
  static bool extractEpubItem(void* ctx, const char* srcPath, const char* destPath);
  bool fireMenuShortcut(uint8_t function);
  bool tryLongPressShortcut(uint8_t function, bool& suppressRelease);
  // Side long-press: cycle Reading Orientation (Settings → Long-Press Buttons).
  void cycleReadingOrientation(bool nextTriggered);
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
  // Progress + session stats to SD (leave path and onExit).
  void flushExitProgressAndStats();
  void takeReaderScreenshot();
  // Persist current chapter page map when complete (idle or turn).
  void persistPageMapIfComplete();
  // Idle: extend current-spine page map a few pages (B). No full-book map (D).
  void tickIdlePageMap();
  // After open/spine land: index ~10 pages ahead (+ behind RAM) for fast turns.
  void warmOpenNavigationWindow();
  // Build + persist one spine's page map, restoring the reader's place after.
  // This is what makes PageBack into the previous chapter land on its real last
  // page (CrossInk section.bin feel).
  bool indexSpinePageMap(int spine);
  [[nodiscard]] bool spineHasPageMap(int spine) const;
  [[nodiscard]] int nearestSpineWithoutMap(bool preferBackward, bool adjacentOnly) const;
  // Idle: index the remaining chapters one at a time so the whole book ends up
  // mapped on SD (INX-style), without ever blocking a page turn or first ink.
  void tickBackgroundIndexer();

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
  static constexpr int kMaxFootnoteDepth = 4;
  struct SavedPos {
    int spine = 0;
    int page = 0;
  };
  SavedPos footnoteStack_[kMaxFootnoteDepth]{};
  int footnoteDepth_ = 0;
  std::string stableId_;
  std::string casperBookDir_;  // /.crosspoint/book_<id>
  std::string irDir_;          // casperBookDir_/rivulet
  int spineIndex_ = 0;
  int marginX_ = 16;
  int marginY_ = 16;
  int marginR_ = 16;
  int marginB_ = 16;
  int pagesUntilFullRefresh_ = 0;
  unsigned long readingSessionStartMs_ = 0;
  unsigned long lastPageTurnTime_ = 0;
  unsigned long lastConfirmReleaseMs_ = 0;
  unsigned long lastIdleMapMs_ = 0;
  mutable uint32_t smoothedBookTimeLeftSeconds_ = 0;
  bool ready_ = false;
  bool error_ = false;
  bool firstPaint_ = true;
  bool ignoreNextConfirmRelease_ = false;
  bool ignoreNextBackRelease_ = false;  // after long-press Back shortcut
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
  unsigned long aaCatchUpAtMs_ = 0;
  uint8_t aaCatchUpTries_ = 0;
  // Long enough that the BW page is unambiguously on glass first (the point of
  // deferring), short enough to read as the same action.
  static constexpr unsigned long kAaCatchUpDelayMs = 350;
  // Heap may still refuse; retry a couple of times, then leave the page BW
  // rather than spin a full-screen greyscale attempt forever.
  static constexpr uint8_t kAaCatchUpMaxTries = 3;
  // Every readable spine has a .rvpm — background indexer can stop.
  bool bookIndexComplete_ = false;
  unsigned long lastIndexPassMs_ = 0;
  // Spine we last tried to index. A partial-IR chapter never yields a saved map,
  // so without this guard the idle tick re-indexes it forever (blocking, seconds
  // per attempt) and page turns appear frozen.
  int lastIndexAttemptSpine_ = -1;
  // Reader must be settled this long before the indexer may take the bus, and
  // this long between chapters so input stays responsive.
  static constexpr unsigned long kBackgroundIndexIdleMs = 6000;
  static constexpr unsigned long kBackgroundIndexGapMs = 1500;
  bool currentPageBookmarked_ = false;
  bool clippingsLoaded_ = false;
  bool pendingScreenshot_ = false;
  bool pendingOpenStateSave_ = false;   // flush APP_STATE after first ink
  bool pendingRecentsTouch_ = false;    // RECENT_BOOKS.addBook after first ink
  bool pendingStatsLoad_ = false;       // CasperStats after first ink (QR open)
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
  std::string errorMsg_;
};
