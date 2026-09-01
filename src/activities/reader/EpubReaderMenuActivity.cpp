#include "EpubReaderMenuActivity.h"

#include <BoardConfig.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "components/icons/settings2.h"
#include "fontIds.h"
#include "util/NestedMenuLabel.h"
#include "util/UiGhostPolicy.h"

namespace {

// Hamburger / list icon for Main tab (24x24, 1-bit, MSB-left, black=0).
constexpr uint8_t MenuIcon24[] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xf3, 0xe7, 0xcf, 0xf3, 0xe7, 0xcf, 0xf3, 0xe7, 0xcf,
    0xf3, 0xe7, 0xcf, 0xf3, 0xe7, 0xcf, 0xf3, 0xe7, 0xcf, 0xf3, 0xe7, 0xcf, 0xf3, 0xe7, 0xcf, 0xf3, 0xe7, 0xcf,
    0xf3, 0xe7, 0xcf, 0xf3, 0xe7, 0xcf, 0xf3, 0xe7, 0xcf, 0xf3, 0xe7, 0xcf, 0xf3, 0xe7, 0xcf, 0xf3, 0xe7, 0xcf,
    0xf3, 0xe7, 0xcf, 0xf3, 0xe7, 0xcf, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
static_assert(sizeof(MenuIcon24) == 24 * ((24 + 7) / 8), "MenuIcon24 must contain 24 rows of 1-bit icon data");

constexpr int tabIconSize = 24;
constexpr int selectedTabBoxWidth = 50;
constexpr int selectedTabBoxHeight = 34;
constexpr int selectedTabBoxRadius = 2;

void drawBookmarkTabIcon(const GfxRenderer& renderer, int x, int y, const bool foregroundBlack = true) {
  constexpr int ribbonWidth = 16;
  constexpr int ribbonHeight = 22;
  constexpr int notchSize = 6;
  const int iconX = x + (tabIconSize - ribbonWidth) / 2;
  const int iconY = y + 1;
  const int centerX = iconX + ribbonWidth / 2;

  const int polyX[5] = {iconX, iconX + ribbonWidth, iconX + ribbonWidth, centerX, iconX};
  const int polyY[5] = {iconY, iconY, iconY + ribbonHeight, iconY + ribbonHeight - notchSize, iconY + ribbonHeight};
  renderer.fillPolygon(polyX, polyY, 5, foregroundBlack);
}

void drawReaderMenuBitmapIcon(const GfxRenderer& renderer, const uint8_t bitmap[], const int x, const int y,
                              const int width, const int height, const bool foregroundBlack = true) {
  if (bitmap == nullptr || width <= 0 || height <= 0) {
    return;
  }

  const int stride = (width + 7) / 8;
  for (int row = 0; row < height; ++row) {
    const int srcOffset = row * stride;
    for (int col = 0; col < width; ++col) {
      const uint8_t mask = static_cast<uint8_t>(0x80 >> (col & 7));
      if ((bitmap[srcOffset + (col >> 3)] & mask) != 0) {
        continue;
      }

      // Icon assets are authored for the legacy portrait blitter. Keep that
      // source rotation, but route placement through logical coordinates so
      // landscape and inverted reader menus keep the tabs centered.
      renderer.drawPixel(x + width - 1 - row, y + col, foregroundBlack);
    }
  }
}

}  // namespace

EpubReaderMenuActivity::EpubReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                               const std::string& title, const int currentPage, const int totalPages,
                                               const int bookProgressPercent, const uint8_t currentOrientation,
                                               const bool hasFootnotes, const bool hasBookmarks,
                                               const bool hasClippings, const bool isCurrentPageBookmarked,
                                               const bool isBookCompleted, const bool hasRestorableBookStats)
    : Activity("EpubReaderMenu", renderer, mappedInput),
      menuItems(buildMenuItems(hasFootnotes, hasBookmarks, hasClippings, isCurrentPageBookmarked, isBookCompleted,
                               hasRestorableBookStats)),
      title(title),
      pendingOrientation(currentOrientation),
      pendingFrontButtonFollow(SETTINGS.frontButtonFollowOrientation ? 1 : 0),
      currentPage(currentPage),
      totalPages(totalPages),
      bookProgressPercent(bookProgressPercent) {}

EpubReaderMenuActivity::TabMenuItems EpubReaderMenuActivity::buildMenuItems(bool hasFootnotes, bool hasBookmarks,
                                                                            bool hasClippings,
                                                                            bool isCurrentPageBookmarked,
                                                                            bool isBookCompleted,
                                                                            bool hasRestorableBookStats) {
  TabMenuItems items;
  auto& mainItems = items[MAIN_TAB_INDEX];
  auto& bookmarkItems = items[BOOKMARKS_TAB_INDEX];
  auto& settingsItems = items[SETTINGS_TAB_INDEX];

  mainItems.reserve(14 + (hasFootnotes ? 1u : 0u));
  bookmarkItems.reserve(8 + (hasBookmarks ? 2u : 0u) + (hasClippings ? 1u : 0u));
  settingsItems.reserve(5 + (hasBookmarks ? 1u : 0u));

  // ---- Main ----
  mainItems.push_back({MenuAction::DICTIONARY, StrId::STR_DICTIONARY});
  mainItems.push_back({MenuAction::SELECT_CHAPTER, StrId::STR_SELECT_CHAPTER});
  if (hasFootnotes) {
    mainItems.push_back({MenuAction::FOOTNOTES, StrId::STR_FOOTNOTES});
  }
  mainItems.push_back({MenuAction::GO_TO_PERCENT, StrId::STR_GO_TO_PERCENT});
  // Under Go to %: reader chrome (status bar) then fonts — Back stays in the book.
  mainItems.push_back({MenuAction::MANAGE_READER_UI, StrId::STR_CUSTOMISE_STATUS_BAR});
  mainItems.push_back({MenuAction::MANAGE_FONTS, StrId::STR_TEXT_SETTINGS});
  // Auto page turn removed (product size / unused). Bluetooth / BLE page-turner removed.
  mainItems.push_back({MenuAction::ROTATE_SCREEN, StrId::STR_ORIENTATION});
  // Nested under Reading Orientation (same placement as Settings).
  mainItems.push_back({MenuAction::ORIENT_FRONT_BUTTONS, StrId::STR_FRONT_BTN_FOLLOW_ORIENTATION});
  // Book Dark Mode (mirrors Settings → Display). Nested Reader Only when On.
  mainItems.push_back({MenuAction::TOGGLE_DARK_MODE, StrId::STR_READER_DARK_MODE});
  mainItems.push_back({MenuAction::TOGGLE_DARK_MODE_READER_ONLY, StrId::STR_DARK_MODE_READER_ONLY});
  // Mark finished is not session tracking — drives finished folder / recents rules.
  mainItems.push_back(
      {MenuAction::TOGGLE_COMPLETED, isBookCompleted ? StrId::STR_MARK_UNFINISHED : StrId::STR_MARK_FINISHED});
  mainItems.push_back({MenuAction::GO_HOME, StrId::STR_GO_HOME_BUTTON});

  // ---- Bookmarks ----
  bookmarkItems.push_back({MenuAction::SYNC, StrId::STR_SYNC_PROGRESS});
  bookmarkItems.push_back(
      {MenuAction::TOGGLE_BOOKMARK, isCurrentPageBookmarked ? StrId::STR_REMOVE_BOOKMARK : StrId::STR_ADD_BOOKMARK});
  if (hasBookmarks) {
    bookmarkItems.push_back({MenuAction::BOOKMARKS, StrId::STR_VIEW_BOOKMARKS});
  }
  bookmarkItems.push_back({MenuAction::SAVE_CLIPPING, StrId::STR_SAVE_CLIPPING});
  if (hasClippings) {
    bookmarkItems.push_back({MenuAction::VIEW_CLIPPINGS, StrId::STR_VIEW_CLIPPINGS});
  }
  bookmarkItems.push_back({MenuAction::SCREENSHOT, StrId::STR_SCREENSHOT_BUTTON});
  bookmarkItems.push_back({MenuAction::DISPLAY_QR, StrId::STR_DISPLAY_QR});

  // ---- Settings (stats + book maintenance only here — not on Main) ----
  // When stat tracking is off, omit every stats-facing row so users never see them.
  if (SETTINGS.readingStatsTrackingEnabled()) {
    settingsItems.push_back({MenuAction::READING_STATS, StrId::STR_READING_STATS});
    settingsItems.push_back({MenuAction::RESET_READING_PACE, StrId::STR_RESET_READING_PACE});
    settingsItems.push_back({MenuAction::DELETE_STATS, StrId::STR_DELETE_BOOK_STATS});
    if (hasRestorableBookStats) {
      settingsItems.push_back({MenuAction::RESTORE_STATS, StrId::STR_RESTORE_BOOK_STATS});
    }
  }
  settingsItems.push_back({MenuAction::DELETE_CACHE, StrId::STR_DELETE_CACHE});
  if (hasBookmarks) {
    settingsItems.push_back({MenuAction::DELETE_BOOKMARKS, StrId::STR_DELETE_BOOKMARKS});
  }

  return items;
}

const std::vector<EpubReaderMenuActivity::MenuItem>& EpubReaderMenuActivity::activeMenuItems() const {
  if (activeTab != MenuTab::Main) {
    return menuItems[activeTabIndex()];
  }
  filteredMainItems_.clear();
  filteredMainItems_.reserve(menuItems[MAIN_TAB_INDEX].size());
  for (const auto& item : menuItems[MAIN_TAB_INDEX]) {
    if (item.action == MenuAction::TOGGLE_DARK_MODE_READER_ONLY && SETTINGS.readerDarkMode == 0) {
      continue;
    }
    filteredMainItems_.push_back(item);
  }
  return filteredMainItems_;
}

void EpubReaderMenuActivity::focusTabRow() { selectedIndex = -1; }

void EpubReaderMenuActivity::cycleActiveTab(const int direction) {
  const int count = static_cast<int>(MENU_TAB_COUNT);
  int idx = static_cast<int>(activeTabIndex()) + direction;
  idx = ((idx % count) + count) % count;
  activeTab = static_cast<MenuTab>(idx);
  focusTabRow();
  requestUpdate();
}

void EpubReaderMenuActivity::onEnter() {
  Activity::onEnter();
  // Snappy open: paint the menu with FAST and wait until it is on glass.
  // Hard HALF scrub used to land *after* prep work — users felt ~2s of dead air
  // then a flash. Mild page residual under the white plate is preferable.
  // Cursor moves still use FAST via displayMenuFrame (scrub not armed).
  UiGhostPolicy::clearHardScrub();
  const uint32_t t0 = millis();
  requestUpdateAndWait();
  LOG_INF("MENU", "open paint %lums", static_cast<unsigned long>(millis() - t0));
}

void EpubReaderMenuActivity::onExit() { Activity::onExit(); }

MenuResult EpubReaderMenuActivity::makeMenuResult(const int action) const {
  return MenuResult{action, pendingOrientation, selectedPageTurnOption, pendingFrontButtonFollow};
}

void EpubReaderMenuActivity::closeCancelled() {
  ActivityResult result;
  result.isCancelled = true;
  result.data = makeMenuResult(-1);
  setResult(std::move(result));
  finish();
}

bool EpubReaderMenuActivity::handleHomeGesture() {
  closeCancelled();
  return true;
}

void EpubReaderMenuActivity::drawIconTabBar(const Rect rect) const {
  renderer.drawLine(rect.x, rect.y, rect.x + rect.width - 1, rect.y, true);
  renderer.drawLine(rect.x, rect.y + rect.height - 1, rect.x + rect.width - 1, rect.y + rect.height - 1, true);

  for (size_t i = 0; i < MENU_TAB_COUNT; i++) {
    const int slotX = rect.x + static_cast<int>((i * rect.width) / MENU_TAB_COUNT);
    const int nextSlotX = rect.x + static_cast<int>(((i + 1) * rect.width) / MENU_TAB_COUNT);
    const int slotWidth = nextSlotX - slotX;
    const int centerX = slotX + slotWidth / 2;
    const bool selected = i == activeTabIndex();
    const bool tabFocused = selected && selectedIndex < 0;
    const int boxX = centerX - selectedTabBoxWidth / 2;
    const int boxY = rect.y + (rect.height - selectedTabBoxHeight) / 2;
    const int iconX = centerX - tabIconSize / 2;
    const int iconY = rect.y + (rect.height - tabIconSize) / 2;

    if (tabFocused) {
      renderer.fillRoundedRect(boxX, boxY, selectedTabBoxWidth, selectedTabBoxHeight, selectedTabBoxRadius,
                               Color::Black);
    } else if (selected) {
      renderer.drawRoundedRect(boxX, boxY, selectedTabBoxWidth, selectedTabBoxHeight, 1, selectedTabBoxRadius, true);
    }

    if (i == static_cast<size_t>(MenuTab::Main)) {
      drawReaderMenuBitmapIcon(renderer, MenuIcon24, iconX, iconY, tabIconSize, tabIconSize, !tabFocused);
    } else if (i == static_cast<size_t>(MenuTab::Bookmarks)) {
      drawBookmarkTabIcon(renderer, iconX, iconY, !tabFocused);
    } else {
      drawReaderMenuBitmapIcon(renderer, Settings2Icon24, iconX, iconY, tabIconSize, tabIconSize, !tabFocused);
    }
  }
}

void EpubReaderMenuActivity::activateSelected() {
  if (selectedIndex < 0) {
    cycleActiveTab();
    return;
  }

  const auto& items = activeMenuItems();
  if (selectedIndex >= static_cast<int>(items.size())) {
    focusTabRow();
    requestUpdate();
    return;
  }

  const auto selectedAction = items[static_cast<size_t>(selectedIndex)].action;
  if (selectedAction == MenuAction::ROTATE_SCREEN) {
    optionPopup.show(StrId::STR_ORIENTATION, orientationLabels.data(), static_cast<int>(orientationLabels.size()),
                     pendingOrientation, [this](int idx) {
                       pendingOrientation = static_cast<uint8_t>(idx);
                       // Same seed as Settings when Reading Orientation changes.
                       pendingFrontButtonFollow =
                           CrossPointSettings::defaultFrontButtonFollowForOrientation(pendingOrientation);
                       // Live so footer / list nav match before leaving the menu.
                       SETTINGS.frontButtonFollowOrientation = pendingFrontButtonFollow;
                       requestUpdate();
                     });
    requestUpdate();
    return;
  }

  if (selectedAction == MenuAction::ORIENT_FRONT_BUTTONS) {
    pendingFrontButtonFollow = pendingFrontButtonFollow ? 0 : 1;
    SETTINGS.frontButtonFollowOrientation = pendingFrontButtonFollow;
    requestUpdate();
    return;
  }

  // Book menu Dark Mode mirrors Settings → Display (master + nested Reader Only).
  if (selectedAction == MenuAction::TOGGLE_DARK_MODE) {
    SETTINGS.readerDarkMode = SETTINGS.readerDarkMode ? 0 : 1;
    SETTINGS.saveToFile();
    renderer.setInvertOnDisplay(SETTINGS.readerDarkMode != 0 && SETTINGS.darkModeReaderOnly == 0);
    const int n = static_cast<int>(activeMenuItems().size());
    if (selectedIndex >= n) selectedIndex = std::max(-1, n - 1);
    requestUpdate();
    return;
  }

  if (selectedAction == MenuAction::TOGGLE_DARK_MODE_READER_ONLY) {
    if (SETTINGS.readerDarkMode == 0) return;
    SETTINGS.darkModeReaderOnly = SETTINGS.darkModeReaderOnly ? 0 : 1;
    SETTINGS.saveToFile();
    renderer.setInvertOnDisplay(SETTINGS.readerDarkMode != 0 && SETTINGS.darkModeReaderOnly == 0);
    requestUpdate();
    return;
  }

  if (selectedAction == MenuAction::AUTO_PAGE_TURN) {
    optionPopup.show(I18N.get(StrId::STR_AUTO_TURN_PAGES_PER_MIN), pageTurnLabels.data(),
                     static_cast<int>(pageTurnLabels.size()), selectedPageTurnOption, [this](int idx) {
                       selectedPageTurnOption = idx;
                       requestUpdate();
                     });
    requestUpdate();
    return;
  }

  ActivityResult result;
  result.data = makeMenuResult(static_cast<int>(selectedAction));
  setResult(std::move(result));
  finish();
}

void EpubReaderMenuActivity::loop() {
  if (optionPopup.handleInput(mappedInput, [this] { requestUpdate(); })) {
    popupClosing = !optionPopup.isActive();
    return;
  }
  if (popupClosing) {
    if (mappedInput.isPressed(MappedInputManager::Button::Back) ||
        mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
      return;
    }
    popupClosing = false;
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      return;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (selectedIndex >= 0) {
      focusTabRow();
      requestUpdate();
      return;
    }
    closeCancelled();
    return;
  }

  auto metrics = UITheme::getInstance().getMetrics();
  Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int contentTop =
      screen.y + metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight * 2 + metrics.verticalSpacing;
  // Portrait: leave room for bottom front-key chrome. Landscape: chrome is on the
  // side (already removed from safe width) — don't also eat bottom height.
  const auto orient = renderer.getOrientation();
  const bool landscape = orient == GfxRenderer::Orientation::LandscapeClockwise ||
                         orient == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const int bottomHint = landscape ? 0 : metrics.buttonHintsHeight;
  const int contentHeight = screen.height - contentTop - metrics.verticalSpacing - bottomHint;
  const int tabTop = screen.y + metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight;

  // Touch: icon tab bar
  int tx = 0;
  int ty = 0;
  if (mappedInput.wasScreenTouchDown(tx, ty) || mappedInput.wasScreenTapped(tx, ty)) {
    if (ty >= tabTop && ty < tabTop + metrics.tabBarHeight && tx >= screen.x && tx < screen.x + screen.width) {
      const int slot = std::clamp((tx - screen.x) * static_cast<int>(MENU_TAB_COUNT) / std::max(1, screen.width), 0,
                                  static_cast<int>(MENU_TAB_COUNT) - 1);
      if (activeTab != static_cast<MenuTab>(slot) || selectedIndex != -1) {
        activeTab = static_cast<MenuTab>(slot);
        focusTabRow();
        requestUpdate();
      }
      return;
    }
  }

  // Touch: list rows
  int listSelected = std::max(0, selectedIndex);
  switch (handleListTouch(listSelected, static_cast<int>(activeMenuItems().size()), contentTop, contentHeight, false)) {
    case ListTouchResult::Activated:
      selectedIndex = listSelected;
      activateSelected();
      return;
    case ListTouchResult::Consumed:
      selectedIndex = listSelected;
      requestUpdate();
      return;
    case ListTouchResult::None:
      break;
  }

  // Front: within-tab list ring. Side: always previous/next tab (any focus).
  // selectedIndex: -1 = tab bar, 0..n-1 = list rows (ring via selectedIndex+1).
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up) {
    const int menuCount = static_cast<int>(activeMenuItems().size());
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex + 1, menuCount + 1) - 1;
    requestUpdate();
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Down) {
    const int menuCount = static_cast<int>(activeMenuItems().size());
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex + 1, menuCount + 1) - 1;
    requestUpdate();
    return;
  }

  auto moveListNext = [this] {
    const int menuCount = static_cast<int>(activeMenuItems().size());
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex + 1, menuCount + 1) - 1;
    requestUpdate();
  };
  auto moveListPrev = [this] {
    const int menuCount = static_cast<int>(activeMenuItems().size());
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex + 1, menuCount + 1) - 1;
    requestUpdate();
  };
  // Always switch tabs; keep relative focus (tab row stays tab row; list index clamped).
  auto moveTabNext = [this] {
    const bool onTabs = selectedIndex < 0;
    const int listPos = selectedIndex;
    cycleActiveTab(1);
    if (onTabs) {
      focusTabRow();
    } else {
      const int menuCount = static_cast<int>(activeMenuItems().size());
      if (menuCount <= 0) {
        selectedIndex = -1;
      } else if (listPos >= menuCount) {
        selectedIndex = menuCount - 1;
      } else {
        selectedIndex = listPos;
      }
      requestUpdate();
    }
  };
  auto moveTabPrev = [this] {
    const bool onTabs = selectedIndex < 0;
    const int listPos = selectedIndex;
    cycleActiveTab(-1);
    if (onTabs) {
      focusTabRow();
    } else {
      const int menuCount = static_cast<int>(activeMenuItems().size());
      if (menuCount <= 0) {
        selectedIndex = -1;
      } else if (listPos >= menuCount) {
        selectedIndex = menuCount - 1;
      } else {
        selectedIndex = listPos;
      }
      requestUpdate();
    }
  };

  buttonNavigator.onRelease(ButtonNavigator::getFrontNextButtons(), moveListNext);
  buttonNavigator.onRelease(ButtonNavigator::getFrontPreviousButtons(), moveListPrev);
  buttonNavigator.onContinuous(ButtonNavigator::getFrontNextButtons(), moveListNext);
  buttonNavigator.onContinuous(ButtonNavigator::getFrontPreviousButtons(), moveListPrev);

  buttonNavigator.onRelease(ButtonNavigator::getSideNextButtons(), moveTabNext);
  buttonNavigator.onRelease(ButtonNavigator::getSidePreviousButtons(), moveTabPrev);
  buttonNavigator.onContinuous(ButtonNavigator::getSideNextButtons(), moveTabNext);
  buttonNavigator.onContinuous(ButtonNavigator::getSidePreviousButtons(), moveTabPrev);

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateSelected();
    return;
  }
}

void EpubReaderMenuActivity::render(RenderLock&&) {
  if (optionPopup.processRender(renderer, mappedInput)) return;

  renderer.clearScreen();

  auto metrics = UITheme::getInstance().getMetrics();
  Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);

  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 title.c_str());

  // Progress summary under header
  std::string progressLine;
  if (totalPages > 0) {
    progressLine = std::string(tr(STR_CHAPTER_PREFIX)) + std::to_string(currentPage) + "/" +
                   std::to_string(totalPages) + std::string(tr(STR_PAGES_SEPARATOR));
  }
  progressLine += std::string(tr(STR_BOOK_PREFIX)) + std::to_string(bookProgressPercent) + "%";
  GUI.drawSubHeader(
      renderer,
      Rect{screen.x, screen.y + metrics.topPadding + metrics.headerHeight, screen.width, metrics.tabBarHeight},
      progressLine.c_str());

  const Rect tabRect{screen.x, screen.y + metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight,
                     screen.width, metrics.tabBarHeight};
  drawIconTabBar(tabRect);

  const int contentTop =
      screen.y + metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight * 2 + metrics.verticalSpacing;
  const auto orient = renderer.getOrientation();
  const bool landscape = orient == GfxRenderer::Orientation::LandscapeClockwise ||
                         orient == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const int bottomHint = landscape ? 0 : metrics.buttonHintsHeight;
  const int contentHeight = screen.height - contentTop - metrics.verticalSpacing - bottomHint;
  const auto& items = activeMenuItems();

  GUI.drawList(
      renderer, Rect{screen.x, contentTop, screen.width, contentHeight}, static_cast<int>(items.size()), selectedIndex,
      [&items](int index) {
        const bool nest = items[index].action == MenuAction::ORIENT_FRONT_BUTTONS ||
                          items[index].action == MenuAction::TOGGLE_DARK_MODE_READER_ONLY;
        // Orient nest prefix omitted (looks wrong in book menu landscape); Reader
        // Only keeps a light nest mark to match Settings → Display.
        if (items[index].action == MenuAction::ORIENT_FRONT_BUTTONS) {
          return std::string(I18N.get(items[index].labelId));
        }
        return NestedMenuLabel::format(I18N.get(items[index].labelId), nest);
      },
      nullptr, nullptr,
      [this](int index) -> std::string {
        const auto& list = activeMenuItems();
        if (index < 0 || index >= static_cast<int>(list.size())) {
          return "";
        }
        const auto value = list[static_cast<size_t>(index)].action;
        if (value == MenuAction::ROTATE_SCREEN) {
          return I18N.get(orientationLabels[pendingOrientation]);
        }
        if (value == MenuAction::ORIENT_FRONT_BUTTONS) {
          return pendingFrontButtonFollow ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
        }
        if (value == MenuAction::TOGGLE_DARK_MODE) {
          return SETTINGS.readerDarkMode ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
        }
        if (value == MenuAction::TOGGLE_DARK_MODE_READER_ONLY) {
          return SETTINGS.darkModeReaderOnly ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
        }
        if (value == MenuAction::AUTO_PAGE_TURN) {
          return pageTurnLabels[selectedPageTurnOption];
        }
        return "";
      },
      true);

  // Front stays Up/Down for the list; side switches tabs (no side-chrome labels).
  const char* confirmHint = selectedIndex < 0 ? tr(STR_NEXT) : tr(STR_SELECT);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmHint, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  // Open arms hard scrub once (onEnter). Cursor moves must stay FAST — displayFastFull
  // always HALF and was multi-second flashing every Down in the reader menu.
  UiGhostPolicy::displayMenuFrame(renderer);
}
