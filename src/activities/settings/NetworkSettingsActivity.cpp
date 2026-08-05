#include "NetworkSettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <WiFi.h>

#include <memory>
#include <string>

#include "CrossPointSettings.h"
#include "KOReaderSettingsActivity.h"
#include "MappedInputManager.h"
#include "OpdsServerListActivity.h"
#include "SilentRestart.h"
#include "activities/ActivityResult.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/UiGhostPolicy.h"

namespace {
enum MenuItem : uint8_t {
  ITEM_WIFI = 0,
  ITEM_KOREADER,
  ITEM_OPDS,
  ITEM_COUNT,
};

StrId nameForItem(const int item) {
  switch (item) {
    case ITEM_WIFI:
      return StrId::STR_WIFI_NETWORKS;
    case ITEM_KOREADER:
      return StrId::STR_KOREADER_SYNC;
    case ITEM_OPDS:
      return StrId::STR_OPDS_SERVERS;
    default:
      return StrId::STR_NETWORK;
  }
}
}  // namespace

void NetworkSettingsActivity::onEnter() {
  Activity::onEnter();
  selectedIndex = 0;
  itemCount = static_cast<int>(ITEM_COUNT);
  UiGhostPolicy::requestHardScrub();
  requestUpdate();
}

void NetworkSettingsActivity::onExit() { Activity::onExit(); }

void NetworkSettingsActivity::loop() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight =
      renderer.getScreenHeight() - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  switch (handleListTouch(selectedIndex, itemCount, contentTop, contentHeight, false)) {
    case ListTouchResult::Activated:
      handleSelection();
      return;
    case ListTouchResult::Consumed:
      requestUpdate();
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
    return;
  }

  buttonNavigator.onNextRelease([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, itemCount);
    requestUpdate();
  });
  buttonNavigator.onPreviousRelease([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, itemCount);
    requestUpdate();
  });
  buttonNavigator.onNextContinuous([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, itemCount);
    requestUpdate();
  });
  buttonNavigator.onPreviousContinuous([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, itemCount);
    requestUpdate();
  });
}

void NetworkSettingsActivity::handleSelection() {
  auto resultHandler = [this](const ActivityResult&) { requestUpdate(); };

  switch (selectedIndex) {
    case ITEM_WIFI:
      // Credentials only — tear STA on exit so Settings stays usable (no LWIP heap leak).
      startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput, false),
                             [this](const ActivityResult&) {
                               SETTINGS.saveToFile();
                               if (WiFi.getMode() != WIFI_MODE_NULL) {
                                 WiFi.disconnect(true);
                                 WiFi.mode(WIFI_OFF);
                                 delay(50);
                               }
                               if (ESP.getMaxAllocHeap() < 12288 || ESP.getFreeHeap() < 28000) {
                                 silentRestart();
                               }
                               requestUpdate();
                             });
      break;
    case ITEM_KOREADER:
      startActivityForResult(std::make_unique<KOReaderSettingsActivity>(renderer, mappedInput), resultHandler);
      break;
    case ITEM_OPDS:
      startActivityForResult(std::make_unique<OpdsServerListActivity>(renderer, mappedInput), resultHandler);
      break;
    default:
      break;
  }
}

void NetworkSettingsActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_NETWORK));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight =
      pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;

  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, itemCount, selectedIndex,
      [](int index) { return std::string(I18N.get(nameForItem(index))); }, nullptr, nullptr,
      [](int) -> std::string { return ">"; }, true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  UiGhostPolicy::displayMenuFrame(renderer);
}
