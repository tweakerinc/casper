#pragma once
#include <Epub.h>
#include <Epub/FootnoteEntry.h>
#include <Epub/Section.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <memory>
#include <optional>
#include <string>

#include "BookReadingStats.h"
#include "BookmarkStore.h"
#include "EndOfBookOptions.h"
#include "EpubReaderMenuActivity.h"
#include "GlobalReadingStats.h"
#include "activities/Activity.h"

struct ToastRect {
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;
};

class EpubReaderActivity final : public Activity {
 public:
  struct ReaderSettingsSnapshot {
    uint8_t fontFamily = 0;
    uint8_t fontSize = 0;
    uint8_t lineHeightPercent = 100;
    uint8_t wordSpacing = 0;
    uint8_t orientation = 0;
    uint8_t screenMargin = 5;
    uint8_t publisherPageNumbers = 0;
    uint8_t paragraphAlignment = 0;
    uint8_t embeddedStyle = 1;
    uint8_t hyphenationEnabled = 0;
    uint8_t textAntiAliasing = 1;
    uint8_t readerDarkMode = 0;
    uint8_t imageRendering = 0;
    uint8_t extraParagraphSpacing = 1;
    uint8_t forceParagraphIndents = 0;
    uint8_t bionicReadingEnabled = 0;
    uint8_t guideReadingEnabled = 0;
    uint8_t epubRenderMode = 0;
    char sdFontFamilyName[64] = "";
  };

 private:
  std::shared_ptr<Epub> epub;
  std::unique_ptr<Section> section = nullptr;
  int currentSpineIndex = 0;
  int nextPageNumber = 0;
  int activeSectionFontId = 0;
  std::optional<uint16_t> pendingPageJump;
  // Set when navigating to a footnote href with a fragment (e.g. #note1).
  // Cleared on the next render after the new section loads and resolves it to a page.
  std::string pendingAnchor;
  std::string pendingFootnotePreviewAnchor;
  bool activeFootnotePreview = false;
  int pagesUntilFullRefresh = 0;
  int cachedSpineIndex = 0;
  int cachedChapterPageNumber = 0;
  int cachedChapterTotalPageCount = 0;
  bool pendingRelayoutReposition = false;
  uint16_t cachedPageParagraphIndex = UINT16_MAX;
  uint16_t cachedPageParagraphOffset = 0;
  uint16_t cachedPageParagraphSpan = 0;
  unsigned long lastPageTurnTime = 0UL;
  unsigned long pageTurnDuration = 0UL;
  unsigned long pageShownAtMs = 0UL;
  bool paceSampleWarmupPending = true;
  uint32_t sessionPaceSampleSeconds = 0;
  uint16_t sessionPaceSampleCount = 0;
  uint32_t sessionReadingSeconds = 0;
  uint16_t lastAutoPageTurnIntervalSeconds = 0;
  bool bookHasCustomReaderSettings = false;
  bool bookHasAutoPageTurnInterval = false;
  bool bookHasRenderModeOverride = false;
  bool restoreGlobalReaderSettingsOnExit = false;
  ReaderSettingsSnapshot globalReaderSettingsBeforeBook;
  bool bookReaderSettingsSuspendedForGlobalEdit = false;
  ReaderSettingsSnapshot suspendedBookReaderSettings;
  BookReadingStats stats;
  GlobalReadingStats globalStats;
  ReadingStatsDateTime sessionStartLocalDateTime;
  bool hasSessionStartLocalDateTime = false;
  // Signals that the next render should reposition within the newly loaded section
  // based on a cross-book percentage jump.
  bool pendingPercentJump = false;
  // Normalized 0.0-1.0 progress within the target spine item, computed from book percentage.
  float pendingSpineProgress = 0.0f;
  uint16_t pendingParagraphIndex = UINT16_MAX;
  uint16_t pendingClippingIndex = UINT16_MAX;
  bool pendingScreenshot = false;
  bool pendingSyncSaveError = false;
  bool skipNextButtonCheck = false;  // Skip button processing for one frame after subactivity exit
  bool automaticPageTurnActive = false;
  bool longPressMenuHandled = false;
  bool longPressBackHandled = false;
  bool longPowerButtonHandled = false;
  bool sideButtonLongPressHandled = false;
  bool frontButtonLongPressHandled = false;
  int pageLoadRetryCount = 0;
  enum class BookmarkFeedbackType : uint8_t {
    Added,
    Removed,
    LimitReached,
  };
  bool pendingBookmarkFeedback = false;
  BookmarkFeedbackType bookmarkFeedbackType = BookmarkFeedbackType::Added;
  unsigned long bookmarkFeedbackShowTime = 0UL;
  bool pendingCompletedFeedback = false;
  bool completedFeedbackIsFinished = false;
  unsigned long completedFeedbackShowTime = 0UL;
  bool pendingTiltPageTurnFeedback = false;
  bool tiltPageTurnFeedbackEnabled = false;
  unsigned long tiltPageTurnFeedbackShowTime = 0UL;
  bool pendingRenderModeToast = false;
  bool renderModeToastShown = false;
  bool pendingSafeModeToast = false;
  bool safeModeToastShown = false;
  uint8_t renderModeToastMode = 0;
  unsigned long renderModeToastShowTime = 0UL;
  std::unique_ptr<uint8_t[]> renderModeToastRegionBuffer;
  size_t renderModeToastRegionBufferSize = 0;
  ToastRect renderModeToastRegion;
  bool renderModeToastRegionSaved = false;
  int completionTriggerSpineIndex = -1;
  float completionTriggerSpineProgress = 1.0f;
  bool completionPromptQueued = false;
  bool completionPromptShown = false;
  bool completionTriggerSeenBelow = false;
  bool completionTriggerCrossed = false;
  bool lastAtOrPastCompletionTrigger = false;

  // Tracks whether this book is currently removed from Recent Books by the
  // removeReadBooksFromRecents feature (set at End-of-Book, cleared if paged back in).
  bool recentsEntryRemoved = false;
  // Set when the reader is left at end-of-book and SETTINGS.moveFinishedToReadFolder is on.
  // Consumed in onExit() to relocate the finished book into /Read/.
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
  // builds with identical layout parameters to the pages already rendered.
  uint16_t buildViewportWidth = 0;
  uint16_t buildViewportHeight = 0;
  // Set when the lazy extension start failed, so loop() does not retry every tick.
  bool partialRebuildStartFailed = false;
  // Set when a background extension build aborted for low heap. startBuild() still succeeds in
  // that state -- it is the layout inside buildSomeMore() that runs out of memory -- so without
  // this flag loop() would restart the same doomed build every tick, and skipLoopDelay() would
  // hold the loop at full speed while it did. The reader keeps the pages already laid out; a
  // build is only re-attempted from render() if the reader actually pages past the watermark.
  bool partialRebuildAbortedForLowMemory = false;
  std::atomic<bool> sectionBuildCancelRequested{false};
  std::atomic<bool> goHomeAfterBuildCancel{false};

  // Last position successfully persisted by saveProgress, used to skip redundant
  // writeAtomic calls on no-op re-renders.
  int lastSavedSpineIndex = -1;
  int lastSavedPage = -1;
  int lastSavedPageCount = -1;

  void renderContents(std::unique_ptr<Page> page, int fontId, int orientedMarginTop, int orientedMarginRight,
                      int orientedMarginBottom, int orientedMarginLeft);
  void drawClippingHighlights(const Page& page, int fontId, int orientedMarginTop, int orientedMarginLeft) const;
  void renderStatusBar() const;
  bool shouldUseFootnotePreview(int targetSpineIndex, const std::string& anchor) const;
  std::string footnotePreviewCacheSuffix(EpubRenderMode renderMode, const std::string& anchor) const;
  void clearFootnotePreviewState();
  // Pages laid out per incremental-build pump: on the render path (catching up to the page
  // being shown) and per loop() tick (background build of a large chapter). Kept small so a
  // background build chunk never noticeably delays input or a pending render.
  static constexpr int BUILD_PAGES_PER_CHUNK = 8;
  static constexpr int BACKGROUND_BUILD_PAGES_PER_TICK = 2;
  // How many pages to keep laid out ahead of the reader for a still-building section. A page
  // turn is ~1s on e-ink and a page builds in ~30ms, so the reader can't out-click the builder
  // -- a tiny buffer is enough. The background build stops once the watermark is this far
  // ahead and resumes as the reader advances; building unbounded instead locked up input by
  // monopolizing the RenderLock. A giant single-spine book therefore never finalizes its .bin
  // in one sitting -- instant reopen comes from Section::suspendBuild() persisting the pages
  // already laid out as a partial file on exit/sleep.
  static constexpr int BUILD_WINDOW_AHEAD = 5;
  // Reopening a partial does not immediately restart its whole-chapter extension build.
  // Start it only when the reader is close enough to need pages past the watermark.
  static constexpr int PARTIAL_REBUILD_START_MARGIN = 15;
  // Show the indexing popup when an initial build must lay out more than this many pages up front
  // (a deep resume/jump into a not-yet-built section), so it isn't a silent wait. Kept independent
  // of the small look-ahead window so ordinary landings stay popup-free.
  static constexpr int BUILD_POPUP_PAGE_THRESHOLD = 20;
  // Also show the popup when first building a spine larger than this (uncompressed bytes): its
  // whole HTML must be inflated before page 1 can lay out (the giant single-spine case), which is
  // a multi-second wait. Normal chapters are well under this and stay popup-free.
  static constexpr size_t BUILD_POPUP_BYTE_THRESHOLD = 96 * 1024;
  // Remap the cached relative reading position once the section's real page count is known
  // (used after a settings change re-paginates a chapter). Returns true if currentPage moved.
  // No-op while the section is still building or when the pagination is unchanged (plain resume).
  bool applyDeferredReposition();
  bool saveProgress(int spineIndex, int currentPage, int pageCount);
  // If Auto Upload on Close is enabled, start KOReader upload then go home. Returns true if started.
  bool tryStartAutoKoUpload();
  void leaveReaderToHome();
  void cacheCurrentSectionPosition();
  void pauseReadingPaceTimer(const char* reason = "unknown");
  void resumeReadingPaceTimer(const char* reason = "unknown");
  void armReadingPaceWarmup(const char* reason = "unknown");
  bool forwardPageReadElapsed(uint32_t& seconds, const char* source) const;
  bool currentPageReadingSecondsForStats(uint32_t& seconds, const char* source) const;
  void recordCurrentPageReadingTime(const char* source = "unknown");
  void recordForwardPagePaceSample(uint32_t seconds, const char* source);
  bool getSessionAveragePaceSeconds(uint16_t& avgSeconds) const;
  void recoverStoredPaceFromSession(const char* reason = "unknown");
  bool getTimeLeftPaceSeconds(uint16_t& avgSeconds, const char*& source, uint16_t& sampleCount) const;
  bool estimateRemainingTimeLeftPages(bool bookEstimate, float& remainingPages) const;
  bool estimateProgressTimeLeftSeconds(uint32_t& seconds) const;
  bool estimateTimeLeftSeconds(bool bookEstimate, uint32_t& seconds) const;
  bool formatTimeLeftLabel(char* buf, size_t len) const;
  void refreshCachedTimeLeftEstimate();
  void applyBookStatsEditsFromDisk();
  void handleBookStatsReturn();
  void resetCurrentBookStatsAfterDelete();
  void openFileTransfer();
  void openAutoPageTurnIntervalPicker(bool ignoreInitialConfirmRelease = false);
  void startClipSelection();
  void startDictionarySelection();
  void resetReadingPaceData();
  void captureGlobalReaderSettings();
  void restoreGlobalReaderSettings();
  void loadBookReaderSettings();
  void saveCurrentBookReaderSettings();
  void saveGlobalSettingsPreservingBookOverrides();
  void beginGlobalSettingsEdit();
  void endGlobalSettingsEdit();
  static void saveReaderOptionsForBook(void* ctx);
  static void saveGlobalSettingsForBookReader(void* ctx);
  static void beginGlobalSettingsEditForBookReader(void* ctx);
  static void endGlobalSettingsEditForBookReader(void* ctx);
  // Jump to a percentage of the book (0-100), mapping it to spine and page.
  void jumpToPercent(int percent);
  void reindexCurrentSection();
  void executeReaderQuickAction(CrossPointSettings::LONG_PRESS_MENU_ACTION action);
  bool quickActionUsesConfirmRelease(CrossPointSettings::LONG_PRESS_MENU_ACTION action) const;
  bool quickActionUsesPowerRelease(CrossPointSettings::LONG_PRESS_MENU_ACTION action) const;
  void suppressConfirmShortcutRelease(CrossPointSettings::LONG_PRESS_MENU_ACTION action);
  void executeFootnoteQuickAction(bool suppressInitialPowerRelease = false);
  void suppressPowerShortcutRelease();
  bool consumeLongPowerButtonRelease();
  bool consumeLongPowerButtonHold();
  bool executeShortPowerButtonAction();
  bool executeLongPowerButtonAction();
  void handleClippingJump(const ClippingJumpResult& clipping);
  void onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action);
  // Opens the reader menu for the current position (short-press Confirm)
  void openReaderMenu();
  void applyOrientation(uint8_t orientation);
  void pageTurn(bool isForwardTurn, const char* source = "unknown");
  float getCurrentBookProgressPercent() const;
  void initializeCompletionPromptTrigger();
  bool isAtOrPastCompletionTrigger() const;
  bool shouldQueueCompletionPromptOnChapterExit() const;
  void queueCompletionPromptIfNeeded();
  void setBookCompleted(bool isCompleted);
  void showCompletedFeedback(bool isCompleted);
  void showTiltPageTurnFeedback(bool enabled);
  void showRenderModeToast(uint8_t renderMode);
  void showSafeModeToast();
  bool storeRenderModeToastRegion(const char* msg);
  void drawRenderModeToastBuffer(const char* msg);
  bool restoreRenderModeToastRegion();

  // Footnote navigation
  void navigateToHref(const std::string& href, bool savePosition = false);
  void restoreSavedPosition();

 public:
  explicit EpubReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Epub> epub)
      : Activity("EpubReader", renderer, mappedInput), epub(std::move(epub)) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&& lock) override;
  bool preventAutoSleep() override { return automaticPageTurnActive; }
  bool skipLoopDelay() override { return section && section->isBuilding(); }
  bool isReaderActivity() const override { return true; }
  bool canSnapshotForSleepOverlay() const override { return true; }
  std::string getCurrentBookPath() const override { return epub ? epub->getPath() : std::string{}; }
  void setAutoPageTurnIntervalSeconds(uint16_t seconds);
  uint16_t getAutoPageTurnIntervalSeconds() const;

  // Renders the last saved page to the frame buffer without flushing to display.
  // Used by SleepActivity to prepare the background for the overlay sleep mode.
  // Returns false if the page cannot be loaded (missing cache / file error).
  static bool drawCurrentPageToBuffer(const std::string& filePath, GfxRenderer& renderer);
  static uint8_t loadBookRenderMode(const std::string& filePath);
  static bool saveBookRenderMode(const std::string& filePath, uint8_t renderMode);
  static bool resetBookReaderSettings(const std::string& filePath);
  ScreenshotInfo getScreenshotInfo() const override;
};
