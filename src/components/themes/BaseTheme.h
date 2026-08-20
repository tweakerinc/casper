#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class GfxRenderer;
struct RecentBook;
struct BookReadingStats;
struct GlobalReadingStats;

struct Rect {
  int x;
  int y;
  int width;
  int height;

  explicit Rect(int x = 0, int y = 0, int width = 0, int height = 0) : x(x), y(y), width(width), height(height) {}
};

struct TabInfo {
  const char* label;
  bool selected;
};

struct ThemeMetrics {
  int batteryWidth;
  int batteryHeight;

  int topPadding;
  int batteryBarHeight;
  int headerHeight;
  int verticalSpacing;

  int previewPadding;
  int previewHeightPercent;

  int contentSidePadding;
  int listRowHeight;
  int listWithSubtitleRowHeight;
  int menuRowHeight;
  int menuSpacing;

  int tabSpacing;
  int tabBarHeight;

  int scrollBarWidth;
  int scrollBarRightOffset;

  int homeTopPadding;
  int homeCoverHeight;
  int homeCoverTileHeight;
  int homeRecentBooksCount;
  bool homeContinueReadingInMenu;
  int homeMenuTopOffset;

  int buttonHintsHeight;
  int sideButtonHintsWidth;

  int progressBarHeight;
  int progressBarMarginTop;
  int statusBarHorizontalMargin;
  int statusBarVerticalMargin;
  int keyboardKeyHeight;
  int keyboardKeySpacing;
  bool keyboardCenteredText;
  int keyboardVerticalOffset;
  int keyboardTextFieldWidthPercent;
  int keyboardWidthPercent;

  float popupTopOffsetRatio;
  int popupMarginX;
  int popupMarginY;
  int popupFrameThickness;
  int popupCornerRadius;
  bool popupTextBold;
  bool popupTextInverted;
  int popupTextBaselineOffsetY;
  int popupProgressBarHeight;
  bool popupProgressDrawOutline;
  bool popupProgressClampPercent;
  bool popupProgressFillInverted;
  bool popupProgressOutlineInverted;

  int optionPopupItemSpacing;
  int optionPopupInnerPadding;
  int optionPopupSelectionHPadding;
  int optionPopupSelectionVPadding;
  int optionPopupTitleGap;
  bool optionPopupUseSmallFont;
  bool optionPopupOptionFontBold;
  int optionPopupSelectionRadius;
  bool optionPopupSelectionLight;
  bool optionPopupDrawAllRows;
  int optionPopupDialogSideMargin;
  bool optionPopupTitleSeparator;

  int textFieldHorizontalPadding;
  int textFieldNormalThickness;
  int textFieldCursorThickness;
  int textFieldLineEndOffset;
};

enum UIIcon {
  None = 0,
  Folder,
  Text,
  Image,
  Book,
  File,
  Recent,
  Settings,
  Transfer,
  Library,
  Wifi,
  Hotspot,
  Bookmark,
  Bluetooth
};

// Default theme implementation (Classic Theme)
// Additional themes can inherit from this and override methods as needed

namespace BaseMetrics {
constexpr ThemeMetrics values = {.batteryWidth = 15,
                                 .batteryHeight = 12,
                                 .topPadding = 5,
                                 .batteryBarHeight = 20,
                                 // Room for 14 pt header + battery/clock row (was tight at 45).
                                 .headerHeight = 54,
                                 .verticalSpacing = 12,
                                 .previewPadding = 12,
                                 .previewHeightPercent = 30,
                                 .contentSidePadding = 20,
                                 // 12 pt list text with comfortable padding (not cramped).
                                 .listRowHeight = 36,
                                 .listWithSubtitleRowHeight = 54,
                                 .menuRowHeight = 48,
                                 .menuSpacing = 10,
                                 .tabSpacing = 12,
                                 .tabBarHeight = 54,
                                 .scrollBarWidth = 4,
                                 .scrollBarRightOffset = 5,
                                 .homeTopPadding = 40,
                                 .homeCoverHeight = 400,
                                 .homeCoverTileHeight = 400,
                                 .homeRecentBooksCount = 1,
                                 .homeContinueReadingInMenu = false,
                                 .homeMenuTopOffset = 10,
                                 .buttonHintsHeight = 40,
                                 .sideButtonHintsWidth = 30,
                                 .progressBarHeight = 16,
                                 .progressBarMarginTop = 1,
                                 .statusBarHorizontalMargin = 5,
                                 .statusBarVerticalMargin = 19,
                                 .keyboardKeyHeight = 48,
                                 .keyboardKeySpacing = 0,
                                 .keyboardCenteredText = false,
                                 .keyboardVerticalOffset = -13,
                                 .keyboardTextFieldWidthPercent = 85,
                                 .keyboardWidthPercent = 94,
                                 .popupTopOffsetRatio = 0.075f,
                                 .popupMarginX = 15,
                                 .popupMarginY = 15,
                                 .popupFrameThickness = 2,
                                 .popupCornerRadius = 0,
                                 .popupTextBold = true,
                                 .popupTextInverted = true,
                                 .popupTextBaselineOffsetY = -2,
                                 .popupProgressBarHeight = 4,
                                 .popupProgressDrawOutline = false,
                                 .popupProgressClampPercent = false,
                                 .popupProgressFillInverted = true,
                                 .popupProgressOutlineInverted = true,
                                 .optionPopupItemSpacing = 6,
                                 .optionPopupInnerPadding = 16,
                                 .optionPopupSelectionHPadding = 8,
                                 .optionPopupSelectionVPadding = 4,
                                 .optionPopupTitleGap = 10,
                                 .optionPopupUseSmallFont = true,
                                 .optionPopupOptionFontBold = true,
                                 .optionPopupSelectionRadius = 0,
                                 .optionPopupSelectionLight = false,
                                 .optionPopupDrawAllRows = false,
                                 .optionPopupDialogSideMargin = 20,
                                 .optionPopupTitleSeparator = true,
                                 .textFieldHorizontalPadding = 6,
                                 .textFieldNormalThickness = 1,
                                 .textFieldCursorThickness = 3,
                                 .textFieldLineEndOffset = 0};
}

class BaseTheme {
 public:
  virtual ~BaseTheme() = default;

  // Component drawing methods
  void drawProgressBar(const GfxRenderer& renderer, Rect rect, size_t current, size_t total) const;
  // displayMode: CasperSettings::BATTERY_DISPLAY_MODE (Icon / Percent / Icon + Percent).
  void drawBatteryLeft(const GfxRenderer& renderer, Rect rect,
                       uint8_t displayMode = 2) const;  // Left aligned (reader mode)
  void drawBatteryRight(const GfxRenderer& renderer, Rect rect,
                        uint8_t displayMode = 2) const;  // Right aligned (UI headers)
  // Icon size tracks status-bar font line height (aspect of base metrics).
  static void batteryIconSizeForStatusFont(const GfxRenderer& renderer, int& outW, int& outH);
  // Width of icon (+ gap + "100%") for layout / reserves using current status font.
  static int batteryGroupWidth(const GfxRenderer& renderer, uint8_t displayMode);
  virtual void fillBatteryIcon(const GfxRenderer& renderer, Rect rect, uint16_t percentage) const;
  virtual void drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                               const char* btn4) const;
  virtual void drawSideButtonHints(const GfxRenderer& renderer, const char* topBtn, const char* bottomBtn) const;
  // Portrait footer height, or thinner landscape side strip for front-key chrome.
  static int frontButtonHintReserve(const GfxRenderer& renderer);
  virtual int getListRowStep(bool hasSubtitle) const;
  virtual int getListPageItems(int contentHeight, bool hasSubtitle) const;
  // rowApplied: when true, draws a filled radio circle on the right (current
  // setting). Focus highlight stays on selectedIndex only — it does not jump
  // to the applied row.
  virtual void drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                        const std::function<std::string(int index)>& rowTitle,
                        const std::function<std::string(int index)>& rowSubtitle = nullptr,
                        const std::function<UIIcon(int index)>& rowIcon = nullptr,
                        const std::function<std::string(int index)>& rowValue = nullptr, bool highlightValue = false,
                        const std::function<bool(int index)>& rowDimmed = nullptr,
                        const std::function<bool(int index)>& rowApplied = nullptr,
                        const std::function<bool(int index)>& rowCentered = nullptr) const;
  // In-list section title (Settings → Reader → "Reader Controls"):
  // UI_12 bold like the Settings / Reader chrome, two 1px rules, then the same
  // air the first list row has under the tab bar (verticalSpacing).
  static int listSectionHeaderHeight(const GfxRenderer& renderer);
  static void drawListSectionHeader(const GfxRenderer& renderer, int x, int width, int y, const char* title);
  virtual void drawHeader(const GfxRenderer& renderer, Rect rect, const char* title,
                          const char* subtitle = nullptr) const;
  virtual void drawSubHeader(const GfxRenderer& renderer, Rect rect, const char* label,
                             const char* rightLabel = nullptr) const;
  virtual void drawTabBar(const GfxRenderer& renderer, Rect rect, const std::vector<TabInfo>& tabs,
                          bool selected) const;
  virtual bool tabIndexFromPoint(const GfxRenderer& renderer, Rect rect, const std::vector<TabInfo>& tabs, int x, int y,
                                 int& index) const;
  // storeCoverBuffer(x,y,w,h): snapshot only the drawn cover region (not the full home tile).
  using StoreCoverBufferFn = std::function<bool(int x, int y, int w, int h)>;
  virtual void drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                   const int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                   bool& bufferRestored, StoreCoverBufferFn storeCoverBuffer,
                                   const BookReadingStats* stats = nullptr, float progressPercent = -1.0f,
                                   const GlobalReadingStats* globalStats = nullptr,
                                   const char* currentChapterTitle = nullptr) const;
  virtual void drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                              const std::function<std::string(int index)>& buttonLabel,
                              const std::function<UIIcon(int index)>& rowIcon) const;
  // topOffsetRatio: <0 = metrics default; >=0 = fraction of screen height from top.
  // Special: kPopupCenterY centers the dialog vertically (legacy full-screen dialogs).
  // refresh=false draws into the framebuffer only (caller displays once).
  static constexpr float kPopupCenterY = -2.0f;
  virtual Rect drawPopup(const GfxRenderer& renderer, const char* message, float topOffsetRatio = -1.0f,
                         bool refresh = true) const;
  // Lightweight busy cue: small text in the upper-left status band (no center
  // pill). Prefer this over drawPopup for open/close/chapter waits — less ghosting.
  // refresh=false paints FB only (caller HALF/FAST later); true uses FAST only.
  virtual Rect drawTopLeftStatus(const GfxRenderer& renderer, const char* message, bool refresh = true) const;
  virtual void drawOptionPopup(const GfxRenderer& renderer, const char* title, const std::vector<std::string>& options,
                               int selectedIndex) const;
  virtual void fillPopupProgress(const GfxRenderer& renderer, const Rect& layout, const int progress,
                                 bool refresh = true) const;
  // currentPage/pageCount = chapter pages ("Pg. n/m"). bookPage/bookPageCount = whole-book
  // pages (same "Pg." form; pass <=0 to reuse chapter numbers). chapterIndex/chapterTotal
  // = TOC position ("Ch. 5/40"; pass <=0 to hide). bookTitle / chapterTitle for title slots.
  // previewIgnoreBatteryMasterHide: Customize Reader UI preview still draws a
  // Battery slot when Display → Battery is Hide, so the layout can be configured.
  // previewClockTime: when non-null, Clock slots use this sample string (settings
  // preview on devices without RTC, e.g. X4).
  void drawStatusBar(GfxRenderer& renderer, const float bookProgress, const int currentPage, const int pageCount,
                     std::string bookTitle, const int paddingBottom = 0, const int textYOffset = 0,
                     const bool fillMargin = true, const bool isPageBookmarked = false,
                     const bool pageCountEstimated = false, const char* timeLeftBookLabel = nullptr,
                     const char* timeLeftChapterLabel = nullptr, const bool drawTopBattery = true,
                     const int bookPage = 0, const int bookPageCount = 0, const bool bookPageCountEstimated = false,
                     const int chapterIndex = 0, const int chapterTotal = 0, std::string chapterTitle = {},
                     bool previewIgnoreBatteryMasterHide = false, const char* previewClockTime = nullptr) const;
  // Top-center clock for reader chrome (X3 RTC). No-op when clock hidden/unavailable.
  void drawTopStatusBarClock(const GfxRenderer& renderer, int topY = -1, const char* previewTime = nullptr) const;
  // System top chrome (Display → Status Bar): Left / Middle / Right Battery|Clock|Hide.
  // previewTime forces a fixed clock string (settings preview).
  // forceBatteryWarningPreview: always show the center Battery Warning sample (Status Bar settings).
  void drawSystemStatusBar(const GfxRenderer& renderer, int topY = -1, const char* previewTime = nullptr,
                           bool forceBatteryWarningPreview = false) const;
  // Width reserved on one side for title truncation (battery+percent or clock text).
  int systemStatusSideReserve(const GfxRenderer& renderer) const;
  void drawHelpText(const GfxRenderer& renderer, Rect rect, const char* label) const;
  virtual void drawTextField(const GfxRenderer& renderer, Rect rect, const int textWidth, bool cursorMode = false,
                             int contentStartX = 0, int contentWidth = 0) const;
  virtual bool showsFileIcons() const { return false; }

  // Shared constants and helpers for battery drawing (used by all themes)
  static constexpr int batteryPercentSpacing = 4;
  // Shared top chrome geometry — MUST match reader status bar and home/dashboard headers.
  static constexpr int kTopChromeInsetX = 12;
  static constexpr int kTopChromeBatteryY = 5;
  static void drawBatteryOutline(const GfxRenderer& renderer, int x, int y, int battWidth, int rectHeight);
  static void drawBatteryLightningBolt(const GfxRenderer& renderer, int boltX, int boltY);
};
