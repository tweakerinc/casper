#include "ClockSettingsActivity.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <I18n.h>

#include <cstdio>
#include <memory>
#include <string>

#include "ClockOffsetActivity.h"
#include "ClockSyncActivity.h"
#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/NestedMenuLabel.h"
#include "util/UiGhostPolicy.h"

namespace {
enum MenuItem {
  ITEM_SHOW = 0,  // System-wide Show / Hide
  ITEM_FORMAT,
  ITEM_UTC_OFFSET,
  ITEM_SYNC,
  ITEM_COUNT
};

const StrId menuNames[ITEM_COUNT] = {
    StrId::STR_CLOCK,
    StrId::STR_CLOCK_FORMAT,
    StrId::STR_CLOCK_UTC_OFFSET,
    StrId::STR_CLOCK_SYNC_NOW,
};

constexpr int CLOCK_FORMAT_ITEMS = 2;
const StrId clockFormatNames[CLOCK_FORMAT_ITEMS] = {StrId::STR_CLOCK_FORMAT_24H, StrId::STR_CLOCK_FORMAT_12H};

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
}  // namespace

void ClockSettingsActivity::onEnter() {
  Activity::onEnter();
  selectedIndex = 0;
  visibleItemCount = ITEM_COUNT;

  if (SETTINGS.clockUtcOffsetQ > 104) {
    SETTINGS.clockUtcOffsetQ = 48;
  }
  if (SETTINGS.clockFormat >= CLOCK_FORMAT_ITEMS) {
    SETTINGS.clockFormat = 0;
  }
  if (SETTINGS.systemClock != CrossPointSettings::STATUS_BAR_CLOCK_MODE::STATUS_BAR_CLOCK_HIDE &&
      SETTINGS.systemClock != CrossPointSettings::STATUS_BAR_CLOCK_MODE::STATUS_BAR_CLOCK_SHOW) {
    SETTINGS.systemClock = CrossPointSettings::STATUS_BAR_CLOCK_MODE::STATUS_BAR_CLOCK_SHOW;
    SETTINGS.saveToFile();
  }
  requestUpdate();
}

void ClockSettingsActivity::onExit() { Activity::onExit(); }

void ClockSettingsActivity::loop() {
  if (optionPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return;

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

void ClockSettingsActivity::handleSelection() {
  switch (selectedIndex) {
    case ITEM_SHOW:
      SETTINGS.systemClock =
          (SETTINGS.systemClock == CrossPointSettings::STATUS_BAR_CLOCK_MODE::STATUS_BAR_CLOCK_HIDE)
              ? CrossPointSettings::STATUS_BAR_CLOCK_MODE::STATUS_BAR_CLOCK_SHOW
              : CrossPointSettings::STATUS_BAR_CLOCK_MODE::STATUS_BAR_CLOCK_HIDE;
      SETTINGS.saveToFile();
      return;
    case ITEM_FORMAT:
      SETTINGS.clockFormat = (SETTINGS.clockFormat + 1) % CLOCK_FORMAT_ITEMS;
      SETTINGS.saveToFile();
      return;
    case ITEM_UTC_OFFSET:
      startActivityForResult(std::make_unique<ClockOffsetActivity>(renderer, mappedInput), nullptr);
      return;
    case ITEM_SYNC:
      startActivityForResult(std::make_unique<ClockSyncActivity>(renderer, mappedInput), nullptr);
      return;
    default:
      return;
  }
}

void ClockSettingsActivity::render(RenderLock&&) {
  if (optionPopup.processRender(renderer, mappedInput)) return;

  renderer.clearScreen();
  auto metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_CLOCK));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, visibleItemCount, static_cast<int>(selectedIndex),
      [](int index) {
        return NestedMenuLabel::format(I18N.get(menuNames[index]), index != ITEM_SHOW);
      },
      nullptr, nullptr,
      [](int index) -> std::string {
        switch (index) {
          case ITEM_SHOW:
            return SETTINGS.systemClock == CrossPointSettings::STATUS_BAR_CLOCK_MODE::STATUS_BAR_CLOCK_HIDE
                       ? tr(STR_HIDE)
                       : tr(STR_SHOW);
          case ITEM_FORMAT: {
            const uint8_t fmt = SETTINGS.clockFormat < CLOCK_FORMAT_ITEMS ? SETTINGS.clockFormat : 0;
            return std::string(I18N.get(clockFormatNames[fmt]));
          }
          case ITEM_UTC_OFFSET:
            return formatUtcOffset(SETTINGS.clockUtcOffsetQ);
          case ITEM_SYNC:
            return SETTINGS.clockHasBeenSynced ? tr(STR_CLOCK_SYNCED) : tr(STR_NOT_SET);
          default:
            return "";
        }
      },
      true);

  const char* confirmHint =
      (selectedIndex == ITEM_SHOW || selectedIndex == ITEM_FORMAT) ? tr(STR_TOGGLE) : tr(STR_SELECT);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmHint, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  UiGhostPolicy::displayMenuFrame(renderer);
}
