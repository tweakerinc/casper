#pragma once
#include <Epub.h>
#include <I18n.h>

#include <array>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "components/OptionPopup.h"
#include "util/ButtonNavigator.h"

struct Rect;

class EpubReaderMenuActivity final : public Activity {
 public:
  bool isReaderMenuActivity() const override { return true; }

  // Menu actions available from the reader menu.
  enum class MenuAction {
    DICTIONARY,
    SELECT_CHAPTER,
    FOOTNOTES,
    GO_TO_PERCENT,
    MANAGE_READER_UI,  // Status bar / reader chrome; above fonts in menu
    MANAGE_FONTS,      // Text settings (family/size/layout/style); stays in reader stack
    AUTO_PAGE_TURN,
    ROTATE_SCREEN,
    ORIENT_FRONT_BUTTONS,  // Nested under Reading Orientation (same as Settings)
    TOGGLE_DARK_MODE,      // Master Dark Mode (Display parity)
    TOGGLE_DARK_MODE_READER_ONLY,  // Nested under Dark Mode when On
    READING_STATS,
    TOGGLE_COMPLETED,
    GO_HOME,
    SYNC,
    TOGGLE_BOOKMARK,
    BOOKMARKS,  // View bookmarks list
    DELETE_BOOKMARKS,
    SAVE_CLIPPING,
    VIEW_CLIPPINGS,
    SCREENSHOT,
    DISPLAY_QR,
    DELETE_STATS,
    DELETE_CACHE,
    RESET_READING_PACE,
  };

  explicit EpubReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& title,
                                  const int currentPage, const int totalPages, const int bookProgressPercent,
                                  const uint8_t currentOrientation, const bool hasFootnotes, bool hasBookmarks,
                                  bool hasClippings = false, bool isCurrentPageBookmarked = false,
                                  bool isBookCompleted = false);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool handleHomeGesture() override;

 private:
  struct MenuItem {
    MenuAction action;
    StrId labelId;
  };

  enum class MenuTab : uint8_t { Main = 0, Bookmarks = 1, Settings = 2 };
  static constexpr size_t MAIN_TAB_INDEX = 0;
  static constexpr size_t BOOKMARKS_TAB_INDEX = 1;
  static constexpr size_t SETTINGS_TAB_INDEX = 2;
  static constexpr size_t MENU_TAB_COUNT = 3;
  using TabMenuItems = std::array<std::vector<MenuItem>, MENU_TAB_COUNT>;

  static TabMenuItems buildMenuItems(bool hasFootnotes, bool hasBookmarks, bool hasClippings,
                                     bool isCurrentPageBookmarked, bool isBookCompleted);
  [[nodiscard]] const std::vector<MenuItem>& activeMenuItems() const;
  [[nodiscard]] size_t activeTabIndex() const { return static_cast<size_t>(activeTab); }
  void cycleActiveTab(int direction = 1);
  void focusTabRow();
  void closeCancelled();
  void activateSelected();
  void drawIconTabBar(Rect rect) const;

  const TabMenuItems menuItems;
  // Filtered view of Main when Dark Mode is Off (hides nested Reader Only).
  mutable std::vector<MenuItem> filteredMainItems_;

  // -1 = tab bar focused; 0..n-1 = list item
  int selectedIndex = -1;
  MenuTab activeTab = MenuTab::Main;

  ButtonNavigator buttonNavigator;
  OptionPopup optionPopup;
  // True while the button press that closed the popup is still held; its release
  // must not fall through to the menu's own Back/Confirm handlers.
  bool popupClosing = false;
  std::string title = "Reader Menu";
  uint8_t pendingOrientation = 0;
  uint8_t pendingFrontButtonFollow = 0;
  uint8_t selectedPageTurnOption = 0;
  // Match Settings → Reading Orientation labels (STR_INVERTED is color invert, not screen rotate).
  const std::vector<StrId> orientationLabels = {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW,
                                                StrId::STR_ORIENTATION_INVERTED, StrId::STR_LANDSCAPE_CCW};
  const std::vector<const char*> pageTurnLabels = {I18N.get(StrId::STR_STATE_OFF), "1", "3", "6", "12"};
  int currentPage = 0;
  int totalPages = 0;
  int bookProgressPercent = 0;

  [[nodiscard]] ::MenuResult makeMenuResult(int action) const;
};
