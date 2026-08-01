#include "SystemStatusBarSettingsActivity.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "ClockOffsetActivity.h"
#include "ClockSyncActivity.h"
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
  ITEM_LEFT = 0,
  ITEM_MIDDLE,
  ITEM_RIGHT,
  ITEM_BATTERY_DISPLAY,  // nested under the slot that has Battery
  ITEM_BATTERY_WARNING,  // threshold nested under the slot that has Battery Warning
  ITEM_CLOCK_FORMAT,
  ITEM_CLOCK_UTC_OFFSET,
  ITEM_CLOCK_SYNC,
  ITEM_ID_COUNT
};

// Slot content order for popup: Hide, Battery, Clock, Battery Warning.
constexpr uint8_t kSlotContentOrder[] = {
    CrossPointSettings::SYS_SLOT_HIDE,
    CrossPointSettings::SYS_SLOT_BATTERY,
    CrossPointSettings::SYS_SLOT_CLOCK,
    CrossPointSettings::SYS_SLOT_BATTERY_WARNING,
};
constexpr int kSlotContentOrderCount = static_cast<int>(sizeof(kSlotContentOrder) / sizeof(kSlotContentOrder[0]));

const StrId kSlotLabelByEnum[CrossPointSettings::SYSTEM_STATUS_SLOT_COUNT] = {
    StrId::STR_HIDE,
    StrId::STR_BATTERY,
    StrId::STR_CLOCK,
    StrId::STR_BATTERY_WARNING,
};

constexpr int CLOCK_FORMAT_ITEMS = 2;
const StrId clockFormatNames[CLOCK_FORMAT_ITEMS] = {StrId::STR_CLOCK_FORMAT_24H, StrId::STR_CLOCK_FORMAT_12H};

// Order matches BATTERY_DISPLAY_MODE: Icon, Percent, Icon + Percent.
constexpr int BATTERY_DISPLAY_ITEMS = CrossPointSettings::BATTERY_DISPLAY_MODE_COUNT;
const StrId batteryDisplayNames[BATTERY_DISPLAY_ITEMS] = {
    StrId::STR_ICON, StrId::STR_PERCENT, StrId::STR_ICON_PLUS_PERCENT};

// Battery Warning thresholds (matches BATTERY_WARNING enum).
constexpr int BATTERY_WARNING_ITEMS = CrossPointSettings::BATTERY_WARNING_COUNT;
const char* batteryWarningValueLabel(uint8_t mode) {
  switch (mode) {
    case CrossPointSettings::BATTERY_WARNING_5:
      return "5%";
    case CrossPointSettings::BATTERY_WARNING_10:
      return "10%";
    case CrossPointSettings::BATTERY_WARNING_15:
      return "15%";
    case CrossPointSettings::BATTERY_WARNING_20:
      return "20%";
    case CrossPointSettings::BATTERY_WARNING_25:
      return "25%";
    case CrossPointSettings::BATTERY_WARNING_OFF:
    default:
      return I18N.get(StrId::STR_STATE_OFF);
  }
}

// Dynamic visible menu: map list index → MenuItem.
// Order: Left, [battery % / clock under Left], Middle, …, Right, …
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

std::string formatUtcOffset(uint8_t biasedQ) {
  if (biasedQ > 104) biasedQ = 48;
  int totalMinutes = (static_cast<int>(biasedQ) - 48) * 15;
  bool neg = totalMinutes < 0;
  int absMinutes = neg ? -totalMinutes : totalMinutes;
  int hours = absMinutes / 60;
  int mins = absMinutes % 60;
  char buf[16];
  snprintf(buf, sizeof(buf), "UTC%c%d:%02d", neg ? '-' : '+', hours, mins);
  return buf;
}

const char* slotContentLabel(uint8_t content) {
  if (content >= CrossPointSettings::SYSTEM_STATUS_SLOT_COUNT) {
    content = CrossPointSettings::SYS_SLOT_HIDE;
  }
  return I18N.get(kSlotLabelByEnum[content]);
}

int displayIndexForContent(uint8_t content) {
  for (int i = 0; i < kSlotContentOrderCount; i++) {
    if (kSlotContentOrder[i] == content) return i;
  }
  return 0;
}

std::vector<std::string> slotOptionLabels(bool includeClock) {
  std::vector<std::string> labels;
  labels.reserve(kSlotContentOrderCount);
  for (int i = 0; i < kSlotContentOrderCount; i++) {
    if (!includeClock && kSlotContentOrder[i] == CrossPointSettings::SYS_SLOT_CLOCK) continue;
    labels.emplace_back(slotContentLabel(kSlotContentOrder[i]));
  }
  return labels;
}

uint8_t contentFromFilteredIndex(int idx, bool includeClock) {
  int seen = 0;
  for (int i = 0; i < kSlotContentOrderCount; i++) {
    if (!includeClock && kSlotContentOrder[i] == CrossPointSettings::SYS_SLOT_CLOCK) continue;
    if (seen == idx) return kSlotContentOrder[i];
    seen++;
  }
  return CrossPointSettings::SYS_SLOT_HIDE;
}

int filteredDisplayIndex(uint8_t content, bool includeClock) {
  int seen = 0;
  for (int i = 0; i < kSlotContentOrderCount; i++) {
    if (!includeClock && kSlotContentOrder[i] == CrossPointSettings::SYS_SLOT_CLOCK) continue;
    if (kSlotContentOrder[i] == content) return seen;
    seen++;
  }
  return 0;
}

void pushClockItems(const bool nested) {
  pushVisible(ITEM_CLOCK_FORMAT, nested);
  pushVisible(ITEM_CLOCK_UTC_OFFSET, nested);
  pushVisible(ITEM_CLOCK_SYNC, nested);
}

void pushNestedForSlot(uint8_t slotContent) {
  if (slotContent == CrossPointSettings::SYS_SLOT_BATTERY) {
    pushVisible(ITEM_BATTERY_DISPLAY, true);
  }
  if (slotContent == CrossPointSettings::SYS_SLOT_BATTERY_WARNING) {
    // Threshold nests under whichever slot holds Battery Warning (L / M / R).
    pushVisible(ITEM_BATTERY_WARNING, true);
  }
  if (slotContent == CrossPointSettings::SYS_SLOT_CLOCK && halClock.isAvailable()) {
    pushClockItems(/*nested=*/true);
  }
}

void rebuildVisibleMenu() {
  gVisibleCount = 0;

  pushVisible(ITEM_LEFT);
  pushNestedForSlot(SETTINGS.systemStatusBarLeft);
  pushVisible(ITEM_MIDDLE);
  pushNestedForSlot(SETTINGS.systemStatusBarMiddle);
  pushVisible(ITEM_RIGHT);
  pushNestedForSlot(SETTINGS.systemStatusBarRight);

  // Penumbra has no status-bar clock slot, but still needs format / offset / sync
  // for the large home clock. Top-level when clock is not on the bar.
  if (halClock.isAvailable() && !SETTINGS.systemStatusBarAllowsClock() &&
      !SETTINGS.systemStatusBarHas(CrossPointSettings::SYS_SLOT_CLOCK)) {
    pushClockItems(/*nested=*/false);
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

bool isSlotItem(int item) { return item == ITEM_LEFT || item == ITEM_MIDDLE || item == ITEM_RIGHT; }

uint8_t& slotFieldForItem(int item) {
  switch (item) {
    case ITEM_LEFT:
      return SETTINGS.systemStatusBarLeft;
    case ITEM_MIDDLE:
      return SETTINGS.systemStatusBarMiddle;
    case ITEM_RIGHT:
    default:
      return SETTINGS.systemStatusBarRight;
  }
}

StrId slotNameForItem(int item) {
  switch (item) {
    case ITEM_LEFT:
      return StrId::STR_LEFT;
    case ITEM_MIDDLE:
      return StrId::STR_MIDDLE;
    case ITEM_RIGHT:
    default:
      return StrId::STR_RIGHT;
  }
}

StrId menuNameForItem(int item) {
  switch (item) {
    case ITEM_LEFT:
      return StrId::STR_LEFT;
    case ITEM_MIDDLE:
      return StrId::STR_MIDDLE;
    case ITEM_RIGHT:
      return StrId::STR_RIGHT;
    case ITEM_BATTERY_DISPLAY:
      return StrId::STR_BATTERY_DISPLAY;
    case ITEM_BATTERY_WARNING:
      return StrId::STR_BATTERY_WARNING;
    case ITEM_CLOCK_FORMAT:
      return StrId::STR_CLOCK_FORMAT;
    case ITEM_CLOCK_UTC_OFFSET:
      return StrId::STR_CLOCK_UTC_OFFSET;
    case ITEM_CLOCK_SYNC:
      return StrId::STR_CLOCK_SYNC_NOW;
    default:
      return StrId::STR_STATUS_BAR;
  }
}
}  // namespace

void SystemStatusBarSettingsActivity::onEnter() {
  Activity::onEnter();
  selectedIndex = 0;

  if (SETTINGS.clockUtcOffsetQ > 104) {
    SETTINGS.clockUtcOffsetQ = 48;
  }
  if (SETTINGS.clockFormat >= CLOCK_FORMAT_ITEMS) {
    SETTINGS.clockFormat = 0;
  }
  // Clamp slots and enforce exclusivity via assign (no-op assign of current value).
  auto clampSlot = [](uint8_t& s) {
    if (s >= CrossPointSettings::SYSTEM_STATUS_SLOT_COUNT) s = CrossPointSettings::SYS_SLOT_HIDE;
  };
  clampSlot(SETTINGS.systemStatusBarLeft);
  clampSlot(SETTINGS.systemStatusBarMiddle);
  clampSlot(SETTINGS.systemStatusBarRight);
  SETTINGS.syncSystemStatusLegacyFromSlots();

  rebuildVisibleMenu();
  visibleItemCount = gVisibleCount;
  if (selectedIndex >= visibleItemCount) selectedIndex = 0;
  requestUpdate();
}

void SystemStatusBarSettingsActivity::onExit() { Activity::onExit(); }

void SystemStatusBarSettingsActivity::loop() {
  if (optionPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return;

  rebuildVisibleMenu();
  visibleItemCount = gVisibleCount;
  if (selectedIndex >= visibleItemCount) selectedIndex = std::max(0, visibleItemCount - 1);

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight =
      renderer.getScreenHeight() - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
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

void SystemStatusBarSettingsActivity::handleSelection() {
  const int item = itemAt(selectedIndex);
  if (isSlotItem(item)) {
    const int slotItem = item;
    uint8_t& field = slotFieldForItem(slotItem);
    // Penumbra theme: large home clock owns the time — Clock is not a slot option.
    const bool includeClock = halClock.isAvailable() && SETTINGS.systemStatusBarAllowsClock();
    const int currentDisplay = filteredDisplayIndex(field, includeClock);
    optionPopup.show(slotNameForItem(slotItem), slotOptionLabels(includeClock), currentDisplay,
                     [this, slotItem, includeClock](int idx) {
                       SETTINGS.assignSystemStatusBarSlot(slotFieldForItem(slotItem),
                                                          contentFromFilteredIndex(idx, includeClock));
                       SETTINGS.saveToFile();
                       rebuildVisibleMenu();
                       visibleItemCount = gVisibleCount;
                       if (selectedIndex >= visibleItemCount) selectedIndex = std::max(0, visibleItemCount - 1);
                     });
    return;
  }

  switch (item) {
    case ITEM_BATTERY_DISPLAY: {
      const uint8_t cur =
          SETTINGS.systemBatteryDisplay < BATTERY_DISPLAY_ITEMS ? SETTINGS.systemBatteryDisplay : 0;
      optionPopup.show(StrId::STR_BATTERY_DISPLAY, batteryDisplayNames, BATTERY_DISPLAY_ITEMS, cur,
                       [this](int idx) {
                         if (idx < 0 || idx >= BATTERY_DISPLAY_ITEMS) return;
                         SETTINGS.systemBatteryDisplay = static_cast<uint8_t>(idx);
                         SETTINGS.saveToFile();
                       });
      return;
    }
    case ITEM_BATTERY_WARNING: {
      std::vector<std::string> labels;
      labels.reserve(BATTERY_WARNING_ITEMS);
      for (int i = 0; i < BATTERY_WARNING_ITEMS; i++) {
        labels.emplace_back(batteryWarningValueLabel(static_cast<uint8_t>(i)));
      }
      const uint8_t cur =
          SETTINGS.batteryWarning < BATTERY_WARNING_ITEMS ? SETTINGS.batteryWarning : 0;
      optionPopup.show(StrId::STR_BATTERY_WARNING, labels, cur, [this](int idx) {
        if (idx < 0 || idx >= BATTERY_WARNING_ITEMS) return;
        SETTINGS.batteryWarning = static_cast<uint8_t>(idx);
        SETTINGS.saveToFile();
      });
      return;
    }
    case ITEM_CLOCK_FORMAT:
      SETTINGS.clockFormat = (SETTINGS.clockFormat + 1) % CLOCK_FORMAT_ITEMS;
      SETTINGS.saveToFile();
      return;
    case ITEM_CLOCK_UTC_OFFSET:
      startActivityForResult(std::make_unique<ClockOffsetActivity>(renderer, mappedInput), nullptr);
      return;
    case ITEM_CLOCK_SYNC:
      startActivityForResult(std::make_unique<ClockSyncActivity>(renderer, mappedInput), nullptr);
      return;
    default:
      return;
  }
}

void SystemStatusBarSettingsActivity::render(RenderLock&&) {
  if (optionPopup.processRender(renderer, mappedInput)) return;

  rebuildVisibleMenu();
  visibleItemCount = gVisibleCount;
  if (selectedIndex >= visibleItemCount) selectedIndex = std::max(0, visibleItemCount - 1);

  renderer.clearScreen();

  auto metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  // Live system status bar preview at the top (same chrome as real headers).
  // Do not use drawHeader — it would also draw a page title rule under the preview.
  char previewTime[16] = "12:34 PM";
  if (halClock.isAvailable()) {
    if (!halClock.formatTime(previewTime, sizeof(previewTime), SETTINGS.clockUtcOffsetQ, SETTINGS.clockFormat == 1)) {
      snprintf(previewTime, sizeof(previewTime), SETTINGS.clockFormat == 1 ? "12:34 PM" : "12:34");
    }
  } else {
    snprintf(previewTime, sizeof(previewTime), SETTINGS.clockFormat == 1 ? "12:34 PM" : "12:34");
  }
  // Force Battery Warning sample in the preview so the user can see the center message.
  GUI.drawSystemStatusBar(renderer, metrics.topPadding, previewTime, /*forceBatteryWarningPreview=*/true);

  // Title centered in the band between top chrome and the list (like Customize Reader UI).
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  {
    const char* headerTitle = tr(STR_STATUS_BAR);
    const int topChromeY = metrics.topPadding + BaseTheme::kTopChromeBatteryY;
    const int topChromeBottom = topChromeY + 6 + metrics.batteryHeight;
    const int titleLineH = renderer.getLineHeight(UI_12_FONT_ID);
    const int gap = contentTop - topChromeBottom;
    const int titleY = topChromeBottom + std::max(0, (gap - titleLineH) / 2);
    renderer.drawCenteredText(UI_12_FONT_ID, titleY, headerTitle, true, EpdFontFamily::BOLD);
  }

  const int hintsTop = pageHeight - metrics.buttonHintsHeight;
  const int contentHeight = std::max(40, hintsTop - contentTop - metrics.verticalSpacing * 2);

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
          case ITEM_LEFT:
            return slotContentLabel(SETTINGS.systemStatusBarLeft);
          case ITEM_MIDDLE:
            return slotContentLabel(SETTINGS.systemStatusBarMiddle);
          case ITEM_RIGHT:
            return slotContentLabel(SETTINGS.systemStatusBarRight);
          case ITEM_BATTERY_DISPLAY: {
            const uint8_t mode =
                SETTINGS.systemBatteryDisplay < BATTERY_DISPLAY_ITEMS ? SETTINGS.systemBatteryDisplay : 0;
            return std::string(I18N.get(batteryDisplayNames[mode]));
          }
          case ITEM_BATTERY_WARNING: {
            const uint8_t mode =
                SETTINGS.batteryWarning < BATTERY_WARNING_ITEMS ? SETTINGS.batteryWarning : 0;
            return std::string(batteryWarningValueLabel(mode));
          }
          case ITEM_CLOCK_FORMAT: {
            const uint8_t fmt = SETTINGS.clockFormat < CLOCK_FORMAT_ITEMS ? SETTINGS.clockFormat : 0;
            return std::string(I18N.get(clockFormatNames[fmt]));
          }
          case ITEM_CLOCK_UTC_OFFSET:
            return formatUtcOffset(SETTINGS.clockUtcOffsetQ);
          case ITEM_CLOCK_SYNC:
            return SETTINGS.clockHasBeenSynced ? tr(STR_CLOCK_SYNCED) : tr(STR_NOT_SET);
          default:
            return "";
        }
      },
      true);

  const int item = itemAt(selectedIndex);
  const char* confirmHint = (item == ITEM_CLOCK_FORMAT) ? tr(STR_TOGGLE) : tr(STR_SELECT);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmHint, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  // "Preview" label in the empty band below the list (mirrors Customize Reader UI).
  {
    const int rowStep = GUI.getListRowStep(false);
    const int pageItems = GUI.getListPageItems(contentHeight, false);
    const int rowsOnPage = std::min(visibleItemCount, pageItems);
    const int listBottom = contentTop + rowsOnPage * rowStep;
    const int bandTop = listBottom;
    const int bandBottom = hintsTop;
    const char* previewLabel = tr(STR_PREVIEW);
    const int textW = renderer.getTextWidth(UI_10_FONT_ID, previewLabel);
    const int textH = renderer.getLineHeight(UI_10_FONT_ID);
    const int bandH = bandBottom - bandTop;
    if (bandH > textH + 4) {
      const int textX = (pageWidth - textW) / 2;
      const int textY = bandTop + (bandH - textH) / 2;
      renderer.drawText(UI_10_FONT_ID, textX, textY, previewLabel);
    }
  }

  UiGhostPolicy::displayMenuFrame(renderer);
}
