#pragma once
#include <functional>
#include <vector>

#include "./FileBrowserActivity.h"
#include "activities/Activity.h"
#include "activities/reader/BookReadingStats.h"
#include "activities/reader/GlobalReadingStats.h"
#include "util/ButtonNavigator.h"

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
  // Long-press Read (BTN_RIGHT) opens the same book-action menu as Recent Books.
  bool readLongPressFired = false;
  // Bare: long-press Library (BTN_CONFIRM) opens Recent Books.
  bool libraryLongPressFired = false;
  // Bare: long-press Menu (BTN_BACK) opens Settings.
  bool menuLongPressFired = false;
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

  int getMenuItemCount() const;
  bool storeCoverBuffer();    // Store frame buffer for cover image
  bool restoreCoverBuffer();  // Restore frame buffer from stored cover
  void freeCoverBuffer();     // Free the stored cover buffer
  // 2-bit hero covers: BW base + LSB/MSB gray planes (sleep-screen style).
  // Without this, midtones paint solid black on a single BW half-refresh.
  void multipassHomeCoverGrayscale();
  void loadRecentBooks(int maxBooks);
  void loadRecentCovers(int coverHeight);

 public:
  explicit HomeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                        HomeMenuItem initialMenuItemValue = HomeMenuItem::NONE)
      : Activity("Home", renderer, mappedInput), initialMenuItem(initialMenuItemValue) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isHomeActivity() const override { return true; }
  // Power long-press FORCE_REFRESH: re-run cover grayscale multipass (plain HALF
  // leaves 2-bit midtones black and the settled-skip would never repaint greys).
  bool handleForcedRefresh() override;
};
