#include "StatsSettingsActivity.h"

#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <I18n.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "BackupStatsActivity.h"
#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/ActivityResult.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
// Dynamic rows: Enable, [Auto Backup if tracking on + X3], [Backup Now if tracking on].
enum MenuItem : uint8_t {
  ITEM_ENABLE = 0,
  ITEM_AUTO_BACKUP,
  ITEM_BACKUP_NOW,
};

constexpr int kMaxVisible = 4;
uint8_t gVisible[kMaxVisible];
int gVisibleCount = 0;

void rebuildVisible() {
  gVisibleCount = 0;
  auto push = [](uint8_t id) {
    if (gVisibleCount < kMaxVisible) gVisible[gVisibleCount++] = id;
  };
  push(ITEM_ENABLE);
  if (SETTINGS.readingStatsTrackingEnabled()) {
    // Auto backup needs RTC date (X3).
    if (gpio.deviceIsX3()) {
      push(ITEM_AUTO_BACKUP);
    }
    push(ITEM_BACKUP_NOW);
  }
}

int itemAt(int listIndex) {
  if (listIndex < 0 || listIndex >= gVisibleCount) return -1;
  return gVisible[listIndex];
}

StrId nameForItem(int item) {
  switch (item) {
    case ITEM_ENABLE:
      return StrId::STR_ENABLE_STAT_TRACKING;
    case ITEM_AUTO_BACKUP:
      return StrId::STR_AUTO_BACKUP_STATS;
    case ITEM_BACKUP_NOW:
      return StrId::STR_BACKUP_NOW;
    default:
      return StrId::STR_STATS;
  }
}
}  // namespace

void StatsSettingsActivity::rebuildMenu() {
  rebuildVisible();
  visibleItemCount = gVisibleCount;
  if (selectedIndex >= visibleItemCount) {
    selectedIndex = std::max(0, visibleItemCount - 1);
  }
}

void StatsSettingsActivity::onEnter() {
  Activity::onEnter();
  selectedIndex = 0;
  rebuildMenu();
  requestUpdate();
}

void StatsSettingsActivity::onExit() { Activity::onExit(); }

void StatsSettingsActivity::loop() {
  rebuildMenu();

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

void StatsSettingsActivity::handleSelection() {
  switch (itemAt(selectedIndex)) {
    case ITEM_ENABLE:
      SETTINGS.readingStatsEnabled = SETTINGS.readingStatsEnabled != 0 ? 0 : 1;
      SETTINGS.saveToFile();
      rebuildMenu();
      return;
    case ITEM_AUTO_BACKUP:
      SETTINGS.autoBackupStats = SETTINGS.autoBackupStats != 0 ? 0 : 1;
      SETTINGS.saveToFile();
      return;
    case ITEM_BACKUP_NOW:
      startActivityForResult(std::make_unique<BackupStatsActivity>(renderer, mappedInput),
                             [this](const ActivityResult&) { requestUpdate(); });
      return;
    default:
      return;
  }
}

void StatsSettingsActivity::render(RenderLock&&) {
  rebuildMenu();

  renderer.clearScreen();
  auto metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_STATS));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight =
      pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;

  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, visibleItemCount, selectedIndex,
      [](int index) {
        const int item = itemAt(index);
        if (item < 0) return std::string();
        return std::string(I18N.get(nameForItem(item)));
      },
      nullptr, nullptr,
      [](int index) -> std::string {
        switch (itemAt(index)) {
          case ITEM_ENABLE:
            return SETTINGS.readingStatsEnabled != 0 ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
          case ITEM_AUTO_BACKUP:
            return SETTINGS.autoBackupStats != 0 ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
          case ITEM_BACKUP_NOW:
            return ">";
          default:
            return "";
        }
      },
      true);

  const int item = itemAt(selectedIndex);
  const char* confirmHint =
      (item == ITEM_ENABLE || item == ITEM_AUTO_BACKUP) ? tr(STR_TOGGLE) : tr(STR_SELECT);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmHint, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
