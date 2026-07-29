#include "DictionarySelectActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstring>
// std::fill used when pre-selecting all packs on first open

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
// Filled / empty selection indicators that render cleanly on e-ink UI fonts.
constexpr const char* kBubbleOn = "(*)";
constexpr const char* kBubbleOff = "( )";
}  // namespace

void DictionarySelectActivity::loadFromSettings() {
  selected.assign(dictionaries.size(), 0);
  for (size_t i = 0; i < dictionaries.size(); ++i) {
    if (SETTINGS.isDictionaryEnabled(dictionaries[i].name.c_str())) {
      selected[i] = 1;
    }
  }
  // Nothing enabled yet: pre-check every pack so EN + EN-ES + ES-EN cascade
  // is the default. Dirty vs initial empty list → Back saves them.
  bool any = false;
  for (const uint8_t s : selected) {
    if (s) {
      any = true;
      break;
    }
  }
  if (!any && !dictionaries.empty()) {
    std::fill(selected.begin(), selected.end(), 1);
    initialSelected.assign(dictionaries.size(), 0);  // mark dirty so Back persists
  } else {
    initialSelected = selected;
  }
}

void DictionarySelectActivity::onEnter() {
  Activity::onEnter();
  dictionaries.clear();
  DictionaryRegistry::discover(dictionaries);
  loadFromSettings();
  selectedIndex = 0;
  // Settings opens this page on Confirm release; that edge is still live on the
  // first loop() and would toggle dictionaries[0] without a new press.
  ignoreNextConfirmRelease = true;
  requestUpdate();
}

void DictionarySelectActivity::onExit() { Activity::onExit(); }

bool DictionarySelectActivity::selectionChanged() const {
  if (selected.size() != initialSelected.size()) {
    return true;
  }
  for (size_t i = 0; i < selected.size(); ++i) {
    if (selected[i] != initialSelected[i]) {
      return true;
    }
  }
  return false;
}

void DictionarySelectActivity::toggleSelected() {
  if (dictionaries.empty() || selectedIndex < 0 ||
      selectedIndex >= static_cast<int>(selected.size())) {
    return;
  }
  selected[static_cast<size_t>(selectedIndex)] = selected[static_cast<size_t>(selectedIndex)] ? 0 : 1;
  requestUpdate();
}

void DictionarySelectActivity::saveAndExit() {
  std::vector<std::string> names;
  names.reserve(dictionaries.size());
  for (size_t i = 0; i < dictionaries.size(); ++i) {
    if (selected[i]) {
      names.push_back(dictionaries[i].name);
    }
  }
  SETTINGS.setEnabledDictionaries(names);
  SETTINGS.saveToFile();
  finish();
}

void DictionarySelectActivity::loop() {
  const bool dirty = selectionChanged();

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (dirty) {
      saveAndExit();
    } else {
      finish();
    }
    return;
  }

  if (ignoreNextConfirmRelease) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      ignoreNextConfirmRelease = false;
      return;
    }
    // Edge already consumed by the parent, or user still holding Confirm: clear
    // once Confirm is idle so a later real press still works.
    if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
      ignoreNextConfirmRelease = false;
    }
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (!dictionaries.empty()) {
      toggleSelected();
    }
    return;
  }

  if (dictionaries.empty()) {
    return;
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight =
      renderer.getScreenHeight() - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const int count = static_cast<int>(dictionaries.size());

  switch (handleListTouch(selectedIndex, count, contentTop, contentHeight, false)) {
    case ListTouchResult::Activated:
      toggleSelected();
      return;
    case ListTouchResult::Consumed:
      return;
    case ListTouchResult::None:
      break;
  }

  const int pageItems = UITheme::getNumberOfItemsPerPage(renderer, true, false, true, false);
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up) {
    selectedIndex = ButtonNavigator::nextPageIndex(selectedIndex, count, pageItems);
    requestUpdate();
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Down) {
    selectedIndex = ButtonNavigator::previousPageIndex(selectedIndex, count, pageItems);
    requestUpdate();
    return;
  }

  buttonNavigator.onNextRelease([this, count] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, count);
    requestUpdate();
  });
  buttonNavigator.onPreviousRelease([this, count] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, count);
    requestUpdate();
  });
  buttonNavigator.onNextContinuous([this, count, pageItems] {
    selectedIndex = ButtonNavigator::nextPageIndex(selectedIndex, count, pageItems);
    requestUpdate();
  });
  buttonNavigator.onPreviousContinuous([this, count, pageItems] {
    selectedIndex = ButtonNavigator::previousPageIndex(selectedIndex, count, pageItems);
    requestUpdate();
  });
}

void DictionarySelectActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_DICTIONARY));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  if (dictionaries.empty()) {
    renderer.drawCenteredText(UI_12_FONT_ID, contentTop + contentHeight / 2, tr(STR_DICT_NO_DICT_SET), true);
  } else {
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(dictionaries.size()), selectedIndex,
        [this](int i) { return dictionaries[static_cast<size_t>(i)].name; },
        /*rowSubtitle=*/nullptr,
        /*rowIcon=*/nullptr,
        [this](int i) -> std::string {
          return selected[static_cast<size_t>(i)] ? kBubbleOn : kBubbleOff;
        },
        /*highlightValue=*/true);
  }

  const bool dirty = selectionChanged();
  const auto labels = mappedInput.mapLabels(dirty ? tr(STR_SAVE) : tr(STR_BACK),
                                            dictionaries.empty() ? "" : tr(STR_SELECT), tr(STR_DIR_UP),
                                            tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
