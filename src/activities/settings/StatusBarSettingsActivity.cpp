#include "StatusBarSettingsActivity.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <I18n.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "components/themes/BaseTheme.h"
#include "fontIds.h"
#include "util/NestedMenuLabel.h"
#include "util/UiGhostPolicy.h"

namespace {
// Logical menu rows (visible set is rebuilt dynamically).
enum MenuItem {
  ITEM_FONT_SIZE = 0,  // top of list — scales clock / % / pages / ETAs / titles
  ITEM_UPPER_LEFT,
  ITEM_UPPER_MIDDLE,
  ITEM_UPPER_RIGHT,
  ITEM_LOWER_LEFT,
  ITEM_LOWER_MIDDLE,
  ITEM_LOWER_RIGHT,
  ITEM_BATTERY_DISPLAY,  // nested under the slot that has Battery
  ITEM_PROGRESS_BAR,
  ITEM_PROGRESS_BAR_THICKNESS,
  ITEM_ID_COUNT
};

StrId menuNameForItem(int item) {
  switch (item) {
    case ITEM_FONT_SIZE:
      return StrId::STR_STATUS_BAR_FONT_SIZE;
    case ITEM_UPPER_LEFT:
      return StrId::STR_UPPER_LEFT;
    case ITEM_UPPER_MIDDLE:
      return StrId::STR_UPPER_MIDDLE;
    case ITEM_UPPER_RIGHT:
      return StrId::STR_UPPER_RIGHT;
    case ITEM_LOWER_LEFT:
      return StrId::STR_LOWER_LEFT;
    case ITEM_LOWER_MIDDLE:
      return StrId::STR_LOWER_MIDDLE;
    case ITEM_LOWER_RIGHT:
      return StrId::STR_LOWER_RIGHT;
    case ITEM_BATTERY_DISPLAY:
      return StrId::STR_BATTERY_DISPLAY;
    case ITEM_PROGRESS_BAR:
      return StrId::STR_PROGRESS_BAR;
    case ITEM_PROGRESS_BAR_THICKNESS:
      return StrId::STR_PROGRESS_BAR_THICKNESS;
    default:
      return StrId::STR_CUSTOMISE_STATUS_BAR;
  }
}

// Menu display order (not enum order). Values are STATUS_BAR_CORNER_CONTENT.
// Hide, Battery, Clock, Book Pg. Count, Chapter Pg. Count, Chapter Counter,
// Progress %, Time Left in Book, Time Left in Chapter, Book Title, Chapter Title,
// XTC Status Bar (last — placement sets XTC top/bottom overlay).
constexpr uint8_t kSlotContentOrder[] = {
    CrossPointSettings::CORNER_HIDE,
    CrossPointSettings::CORNER_BATTERY,
    CrossPointSettings::CORNER_CLOCK,
    CrossPointSettings::CORNER_BOOK_PAGE_COUNTER,
    CrossPointSettings::CORNER_CHAPTER_PAGE_COUNTER,
    CrossPointSettings::CORNER_CHAPTER_COUNTER,
    CrossPointSettings::CORNER_PROGRESS_PERCENT,
    CrossPointSettings::CORNER_TIME_LEFT_BOOK,
    CrossPointSettings::CORNER_TIME_LEFT_CHAPTER,
    CrossPointSettings::CORNER_BOOK_TITLE,
    CrossPointSettings::CORNER_CHAPTER_TITLE,
    CrossPointSettings::CORNER_XTC_STATUS_BAR,
};
constexpr int kSlotContentOrderCount = static_cast<int>(sizeof(kSlotContentOrder) / sizeof(kSlotContentOrder[0]));

// Labels for enum values (index = STATUS_BAR_CORNER_CONTENT).
const StrId kContentLabelByEnum[CrossPointSettings::STATUS_BAR_CORNER_CONTENT_COUNT] = {
    StrId::STR_HIDE,                      // 0
    StrId::STR_BATTERY,                   // 1
    StrId::STR_CHAPTER_PAGE_COUNTER,      // 2
    StrId::STR_PROGRESS_PERCENTAGE,       // 3
    StrId::STR_TIME_LEFT_BOOK_OPTION,     // 4
    StrId::STR_TIME_LEFT_CHAPTER_OPTION,  // 5
    StrId::STR_CLOCK,                     // 6
    StrId::STR_BOOK_TITLE,                // 7
    StrId::STR_BOOK_PAGE_COUNTER,         // 8
    StrId::STR_CHAPTER_COUNTER,           // 9
    StrId::STR_CHAPTER_TITLE,             // 10
    StrId::STR_XTC_STATUS_BAR,            // 11
};

// Order matches STATUS_BAR_PROGRESS_BAR: Hide, Book, Chapter.
constexpr int PROGRESS_BAR_ITEMS = 3;
const StrId progressBarNames[PROGRESS_BAR_ITEMS] = {StrId::STR_HIDE, StrId::STR_BOOK, StrId::STR_CHAPTER};

constexpr int PROGRESS_BAR_THICKNESS_ITEMS = 3;
const StrId progressBarThicknessNames[PROGRESS_BAR_THICKNESS_ITEMS] = {
    StrId::STR_PROGRESS_BAR_THIN, StrId::STR_PROGRESS_BAR_MEDIUM, StrId::STR_PROGRESS_BAR_THICK};

// Order matches BATTERY_DISPLAY_MODE: Icon, Percent, Icon + Percent.
constexpr int BATTERY_DISPLAY_ITEMS = CrossPointSettings::BATTERY_DISPLAY_MODE_COUNT;
const StrId batteryDisplayNames[BATTERY_DISPLAY_ITEMS] = {StrId::STR_ICON, StrId::STR_PERCENT,
                                                          StrId::STR_ICON_PLUS_PERCENT};

// Order matches STATUS_BAR_FONT_SIZE: 8 / 10 / 12 pt.
constexpr int STATUS_BAR_FONT_ITEMS = CrossPointSettings::STATUS_BAR_FONT_SIZE_COUNT;
const StrId statusBarFontNames[STATUS_BAR_FONT_ITEMS] = {StrId::STR_STATUS_BAR_FONT_8, StrId::STR_STATUS_BAR_FONT_10,
                                                         StrId::STR_STATUS_BAR_FONT_12};

// Air between list and preview footer (label band for "Preview").
const int kPreviewLabelBand = 22;
// Extra lift above button hints so progress bar / titles do not sit on the footer.
const int kPreviewAboveHints = 14;

// paddingBottom for drawStatusBar: clear the button-hints strip + a small gap.
int previewPaddingBottom(const ThemeMetrics& metrics) { return metrics.buttonHintsHeight + kPreviewAboveHints; }

int previewReserveHeight(const ThemeMetrics& metrics) {
  return UITheme::getInstance().getStatusBarHeight() + previewPaddingBottom(metrics) + kPreviewLabelBand;
}

// Dynamic visible menu: map list index → MenuItem.
constexpr int kMaxVisible = 16;
uint8_t gVisibleItems[kMaxVisible];
bool gVisibleNested[kMaxVisible];
int gVisibleCount = 0;

void pushVisible(const uint8_t id, const bool nested = false) {
  if (gVisibleCount >= kMaxVisible) return;
  gVisibleItems[gVisibleCount] = id;
  gVisibleNested[gVisibleCount] = nested;
  ++gVisibleCount;
}

void pushNestedForSlot(uint8_t slotContent) {
  if (slotContent == CrossPointSettings::CORNER_BATTERY) {
    pushVisible(ITEM_BATTERY_DISPLAY, true);
  }
}

void rebuildVisibleMenu() {
  gVisibleCount = 0;
  pushVisible(ITEM_FONT_SIZE);
  pushVisible(ITEM_UPPER_LEFT);
  pushNestedForSlot(SETTINGS.statusBarUpperLeft);
  pushVisible(ITEM_UPPER_MIDDLE);
  pushNestedForSlot(SETTINGS.statusBarUpperMiddle);
  pushVisible(ITEM_UPPER_RIGHT);
  pushNestedForSlot(SETTINGS.statusBarUpperRight);
  pushVisible(ITEM_LOWER_LEFT);
  pushNestedForSlot(SETTINGS.statusBarLowerLeft);
  pushVisible(ITEM_LOWER_MIDDLE);
  pushNestedForSlot(SETTINGS.statusBarLowerMiddle);
  pushVisible(ITEM_LOWER_RIGHT);
  pushNestedForSlot(SETTINGS.statusBarLowerRight);
  pushVisible(ITEM_PROGRESS_BAR);
  if (SETTINGS.statusBarProgressBar != CrossPointSettings::STATUS_BAR_PROGRESS_BAR::HIDE_PROGRESS) {
    pushVisible(ITEM_PROGRESS_BAR_THICKNESS, true);
  }
}

int itemAt(int listIndex) {
  if (listIndex < 0 || listIndex >= gVisibleCount) return -1;
  return gVisibleItems[listIndex];
}

bool nestedAt(int listIndex) {
  if (listIndex < 0 || listIndex >= gVisibleCount) return false;
  return gVisibleNested[listIndex];
}

bool isSlotItem(int item) {
  return item == ITEM_UPPER_LEFT || item == ITEM_UPPER_MIDDLE || item == ITEM_UPPER_RIGHT || item == ITEM_LOWER_LEFT ||
         item == ITEM_LOWER_MIDDLE || item == ITEM_LOWER_RIGHT;
}

uint8_t& cornerFieldForItem(int item) {
  switch (item) {
    case ITEM_UPPER_LEFT:
      return SETTINGS.statusBarUpperLeft;
    case ITEM_UPPER_MIDDLE:
      return SETTINGS.statusBarUpperMiddle;
    case ITEM_UPPER_RIGHT:
      return SETTINGS.statusBarUpperRight;
    case ITEM_LOWER_LEFT:
      return SETTINGS.statusBarLowerLeft;
    case ITEM_LOWER_MIDDLE:
      return SETTINGS.statusBarLowerMiddle;
    case ITEM_LOWER_RIGHT:
    default:
      return SETTINGS.statusBarLowerRight;
  }
}

const char* cornerContentLabel(uint8_t content) {
  if (content >= CrossPointSettings::STATUS_BAR_CORNER_CONTENT_COUNT) {
    content = CrossPointSettings::CORNER_HIDE;
  }
  return I18N.get(kContentLabelByEnum[content]);
}

// Time-left slots need pace samples from stat tracking — hide them when tracking is off.
bool slotContentAvailable(uint8_t content) {
  if (!SETTINGS.readingStatsTrackingEnabled()) {
    if (content == CrossPointSettings::CORNER_TIME_LEFT_BOOK ||
        content == CrossPointSettings::CORNER_TIME_LEFT_CHAPTER) {
      return false;
    }
  }
  return true;
}

void appendAvailableSlotContents(std::vector<uint8_t>& out) {
  out.clear();
  out.reserve(kSlotContentOrderCount);
  for (int i = 0; i < kSlotContentOrderCount; i++) {
    if (slotContentAvailable(kSlotContentOrder[i])) {
      out.push_back(kSlotContentOrder[i]);
    }
  }
}

int displayIndexForContent(uint8_t content) {
  std::vector<uint8_t> opts;
  appendAvailableSlotContents(opts);
  for (int i = 0; i < static_cast<int>(opts.size()); i++) {
    if (opts[i] == content) return i;
  }
  // Current value unavailable (e.g. time-left while tracking is off) → select Hide.
  return 0;
}

std::vector<std::string> slotOptionLabels() {
  std::vector<uint8_t> opts;
  appendAvailableSlotContents(opts);
  std::vector<std::string> labels;
  labels.reserve(opts.size());
  for (uint8_t c : opts) {
    labels.emplace_back(cornerContentLabel(c));
  }
  return labels;
}
}  // namespace

void StatusBarSettingsActivity::onEnter() {
  Activity::onEnter();

  selectedIndex = 0;

  if (SETTINGS.statusBarProgressBar >= PROGRESS_BAR_ITEMS) {
    SETTINGS.statusBarProgressBar = CrossPointSettings::STATUS_BAR_PROGRESS_BAR::HIDE_PROGRESS;
  }
  if (SETTINGS.statusBarProgressBarThickness >= PROGRESS_BAR_THICKNESS_ITEMS) {
    SETTINGS.statusBarProgressBarThickness = CrossPointSettings::PROGRESS_BAR_THIN;
  }
  if (SETTINGS.statusBarFontSize >= STATUS_BAR_FONT_ITEMS) {
    SETTINGS.statusBarFontSize = CrossPointSettings::STATUS_BAR_FONT_8;
  }
  rebuildVisibleMenu();
  visibleItemCount = gVisibleCount;
  if (selectedIndex >= visibleItemCount) selectedIndex = 0;
  requestUpdate();
}

void StatusBarSettingsActivity::onExit() { Activity::onExit(); }

void StatusBarSettingsActivity::loop() {
  if (optionPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return;

  rebuildVisibleMenu();
  visibleItemCount = gVisibleCount;
  if (selectedIndex >= visibleItemCount) selectedIndex = std::max(0, visibleItemCount - 1);

  const auto& metrics = UITheme::getInstance().getMetrics();
  // Compact title band (not full Settings headerHeight — that left too little list room on X3).
  const int topChromeBottom = metrics.topPadding + BaseTheme::kTopChromeBatteryY + 6 + metrics.batteryHeight;
  const int titleLineH = renderer.getLineHeight(UI_12_FONT_ID);
  const int contentTop = topChromeBottom + titleLineH + 12;
  const int hintsTop = renderer.getScreenHeight() - metrics.buttonHintsHeight;
  const int contentHeight = std::max(40, hintsTop - contentTop - previewReserveHeight(metrics));
  switch (handleListTouch(selectedIndex, visibleItemCount, contentTop, contentHeight, false)) {
    case ListTouchResult::Activated:
      handleSelection();
      requestUpdate();
      return;
    case ListTouchResult::Consumed:
      return;
    case ListTouchResult::None:
      break;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    handleSelection();
    requestUpdate();
    return;
  }

  buttonNavigator.onNextRelease([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, visibleItemCount);
    requestUpdate();
  });
  buttonNavigator.onPreviousRelease([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, visibleItemCount);
    requestUpdate();
  });
  buttonNavigator.onNextContinuous([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, visibleItemCount);
    requestUpdate();
  });
  buttonNavigator.onPreviousContinuous([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, visibleItemCount);
    requestUpdate();
  });
}

void StatusBarSettingsActivity::handleSelection() {
  const int item = itemAt(selectedIndex);
  if (isSlotItem(item)) {
    const int cornerItem = item;
    uint8_t& field = cornerFieldForItem(cornerItem);
    const int currentDisplay = displayIndexForContent(field);
    optionPopup.show(menuNameForItem(cornerItem), slotOptionLabels(), currentDisplay, [this, cornerItem](int idx) {
      std::vector<uint8_t> opts;
      appendAvailableSlotContents(opts);
      if (idx < 0 || idx >= static_cast<int>(opts.size())) return;
      SETTINGS.assignStatusBarCorner(cornerFieldForItem(cornerItem), opts[static_cast<size_t>(idx)]);
      SETTINGS.saveToFile();
      rebuildVisibleMenu();
      visibleItemCount = gVisibleCount;
      if (selectedIndex >= visibleItemCount) selectedIndex = std::max(0, visibleItemCount - 1);
    });
    return;
  }

  switch (item) {
    case ITEM_FONT_SIZE: {
      const uint8_t cur = SETTINGS.statusBarFontSize < STATUS_BAR_FONT_ITEMS ? SETTINGS.statusBarFontSize : 0;
      optionPopup.show(StrId::STR_STATUS_BAR_FONT_SIZE, statusBarFontNames, STATUS_BAR_FONT_ITEMS, cur,
                       [this](int idx) {
                         if (idx < 0 || idx >= STATUS_BAR_FONT_ITEMS) return;
                         SETTINGS.statusBarFontSize = static_cast<uint8_t>(idx);
                         SETTINGS.saveToFile();
                       });
      return;
    }
    case ITEM_BATTERY_DISPLAY: {
      const uint8_t cur = SETTINGS.readerBatteryDisplay < BATTERY_DISPLAY_ITEMS ? SETTINGS.readerBatteryDisplay : 0;
      optionPopup.show(StrId::STR_BATTERY_DISPLAY, batteryDisplayNames, BATTERY_DISPLAY_ITEMS, cur, [this](int idx) {
        if (idx < 0 || idx >= BATTERY_DISPLAY_ITEMS) return;
        SETTINGS.readerBatteryDisplay = static_cast<uint8_t>(idx);
        SETTINGS.saveToFile();
      });
      return;
    }
    case ITEM_PROGRESS_BAR:
      optionPopup.show(StrId::STR_PROGRESS_BAR, progressBarNames, PROGRESS_BAR_ITEMS, SETTINGS.statusBarProgressBar,
                       [this](int idx) {
                         SETTINGS.statusBarProgressBar = idx;
                         SETTINGS.saveToFile();
                         rebuildVisibleMenu();
                         visibleItemCount = gVisibleCount;
                         if (selectedIndex >= visibleItemCount) selectedIndex = visibleItemCount - 1;
                       });
      return;
    case ITEM_PROGRESS_BAR_THICKNESS:
      optionPopup.show(StrId::STR_PROGRESS_BAR_THICKNESS, progressBarThicknessNames, PROGRESS_BAR_THICKNESS_ITEMS,
                       SETTINGS.statusBarProgressBarThickness, [this](int idx) {
                         SETTINGS.statusBarProgressBarThickness = idx;
                         SETTINGS.saveToFile();
                       });
      return;
    default:
      return;
  }
}

void StatusBarSettingsActivity::render(RenderLock&&) {
  if (optionPopup.processRender(renderer, mappedInput)) return;

  rebuildVisibleMenu();
  visibleItemCount = gVisibleCount;
  if (selectedIndex >= visibleItemCount) selectedIndex = std::max(0, visibleItemCount - 1);

  renderer.clearScreen();

  auto metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  // Title only — do not use drawHeader (system chrome would ghost under the
  // six-slot preview when Upper Left is Hide). Compact band under top chrome.
  const int topChromeBottom = metrics.topPadding + BaseTheme::kTopChromeBatteryY + 6 + metrics.batteryHeight;
  const int titleLineH = renderer.getLineHeight(UI_12_FONT_ID);
  const int contentTop = topChromeBottom + titleLineH + 12;
  {
    const char* headerTitle = tr(STR_CUSTOMISE_STATUS_BAR);
    const int titleY = topChromeBottom + 4;
    renderer.drawCenteredText(UI_12_FONT_ID, titleY, headerTitle, true, EpdFontFamily::BOLD);
  }
  const int hintsTop = pageHeight - metrics.buttonHintsHeight;
  // List stays above the bottom status-bar preview + "Preview" label.
  const int verticalPreviewPad = previewPaddingBottom(metrics);
  const int contentHeight = std::max(40, hintsTop - contentTop - previewReserveHeight(metrics));

  // drawList pages when itemCount > rows that fit; Lyra draws a scroll bar.
  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, visibleItemCount, static_cast<int>(selectedIndex),
      [](int index) {
        const int item = itemAt(index);
        if (item < 0) return std::string();
        return NestedMenuLabel::format(I18N.get(menuNameForItem(item)), nestedAt(index));
      },
      nullptr, nullptr,
      [](int index) -> std::string {
        switch (itemAt(index)) {
          case ITEM_FONT_SIZE: {
            const uint8_t fs = SETTINGS.statusBarFontSize < STATUS_BAR_FONT_ITEMS ? SETTINGS.statusBarFontSize : 0;
            return std::string(I18N.get(statusBarFontNames[fs]));
          }
          case ITEM_UPPER_LEFT:
            return cornerContentLabel(SETTINGS.statusBarUpperLeft);
          case ITEM_UPPER_MIDDLE:
            return cornerContentLabel(SETTINGS.statusBarUpperMiddle);
          case ITEM_UPPER_RIGHT:
            return cornerContentLabel(SETTINGS.statusBarUpperRight);
          case ITEM_LOWER_LEFT:
            return cornerContentLabel(SETTINGS.statusBarLowerLeft);
          case ITEM_LOWER_MIDDLE:
            return cornerContentLabel(SETTINGS.statusBarLowerMiddle);
          case ITEM_LOWER_RIGHT:
            return cornerContentLabel(SETTINGS.statusBarLowerRight);
          case ITEM_BATTERY_DISPLAY: {
            const uint8_t mode =
                SETTINGS.readerBatteryDisplay < BATTERY_DISPLAY_ITEMS ? SETTINGS.readerBatteryDisplay : 0;
            return std::string(I18N.get(batteryDisplayNames[mode]));
          }
          case ITEM_PROGRESS_BAR:
            return I18N.get(
                progressBarNames[SETTINGS.statusBarProgressBar < PROGRESS_BAR_ITEMS ? SETTINGS.statusBarProgressBar
                                                                                    : 0]);
          case ITEM_PROGRESS_BAR_THICKNESS:
            return I18N.get(progressBarThicknessNames[SETTINGS.statusBarProgressBarThickness]);
          default:
            return "";
        }
      },
      true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  const auto sb = SETTINGS.statusBarSpec();
  std::string bookTitle = sb.wantsBookTitle ? tr(STR_EXAMPLE_BOOK) : "";
  std::string chapterTitle = sb.wantsChapterTitle ? tr(STR_EXAMPLE_CHAPTER) : "";

  char bookTl[32];
  char chapTl[32];
  snprintf(bookTl, sizeof(bookTl), "12m %s", tr(STR_TIME_LEFT_IN_BOOK));
  snprintf(chapTl, sizeof(chapTl), "5m %s", tr(STR_TIME_LEFT_IN_CHAPTER));
  const char* bookPtr = sb.wantsTimeLeftBook ? bookTl : nullptr;
  const char* chapPtr = sb.wantsTimeLeftChapter ? chapTl : nullptr;

  // Sample clock for preview even when RTC is missing (X4) or not ready — otherwise
  // Upper Middle "Clock" draws nothing and users think the slot is broken.
  char clockSample[16];
  if (SETTINGS.clockFormat == 1) {
    snprintf(clockSample, sizeof(clockSample), "12:34 PM");
  } else {
    snprintf(clockSample, sizeof(clockSample), "12:34");
  }

  // Preview ignores Display → Battery Hide so corner slots set to Battery still
  // render (real reader chrome continues to honor the master Battery setting).
  // verticalPreviewPad lifts chrome above the button-hint strip.
  GUI.drawStatusBar(renderer, 75, 8, 32, bookTitle, verticalPreviewPad, 0, false, false, false, bookPtr, chapPtr,
                    /*drawTopBattery=*/true, /*bookPage=*/120, /*bookPageCount=*/480, /*bookPageCountEstimated=*/true,
                    /*chapterIndex=*/5, /*chapterTotal=*/40, chapterTitle,
                    /*previewIgnoreBatteryMasterHide=*/true, clockSample);

  // "Preview" label in the reserved band between list and footer chrome.
  {
    const int bandTop = contentTop + contentHeight;
    const int bandBottom = hintsTop - UITheme::getInstance().getStatusBarHeight() - verticalPreviewPad;
    const char* previewLabel = tr(STR_PREVIEW);
    const int textW = renderer.getTextWidth(UI_10_FONT_ID, previewLabel);
    const int textH = renderer.getLineHeight(UI_10_FONT_ID);
    const int bandH = bandBottom - bandTop;
    if (bandH > textH + 2) {
      const int textX = (pageWidth - textW) / 2;
      const int textY = bandTop + (bandH - textH) / 2;
      renderer.drawText(UI_10_FONT_ID, textX, textY, previewLabel);
    }
  }

  UiGhostPolicy::displayMenuFrame(renderer);
}
