#include "SettingsActivity.h"

#include <BoardConfig.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalGPIO.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "BackupStatsActivity.h"
#include "ButtonRemapActivity.h"
#include "ClearCacheActivity.h"
#include "ClockSettingsActivity.h"
#include "SystemStatusBarSettingsActivity.h"
#include "CrossPointSettings.h"
#include "DictionarySelectActivity.h"
#include "FontDownloadActivity.h"
#include "KOReaderSettingsActivity.h"
#include "StatsSettingsActivity.h"
#include "LanguageSelectActivity.h"
#include "MappedInputManager.h"
#include "OpdsServerListActivity.h"
#include "OtaUpdateActivity.h"
#include "SdCardFontSystem.h"
#include "SdFirmwareUpdateActivity.h"
#include "SettingsList.h"
#include "StatusBarSettingsActivity.h"
#include "TextSettingsActivity.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/IntervalSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/NestedMenuLabel.h"

const StrId SettingsActivity::categoryNames[categoryCount] = {StrId::STR_CAT_DISPLAY, StrId::STR_CAT_READER,
                                                              StrId::STR_CAT_CONTROLS, StrId::STR_CAT_SYSTEM};

void SettingsActivity::rebuildSettingsLists() {
  displaySettings.clear();
  readerSettings.clear();
  controlsSettings.clear();
  systemSettings.clear();

  // Pick up any fonts uploaded/deleted over the web server since the last
  // reader activity ran — otherwise the font-family picker shows stale list.
  sdFontSystem.refreshIfDirty();

  // Rescan only needed for web/list metadata; on-device dictionary picker is a page.
  std::vector<DictionaryEntry> dictionaries;
  DictionaryRegistry::discover(dictionaries);

  // System fields pulled from the shared list (reordered explicitly below).
  SettingInfo timeToSleep{};
  SettingInfo sessionTime{};
  SettingInfo showHidden{};
  SettingInfo removeRecents{};
  SettingInfo moveFinished{};
  bool haveTimeToSleep = false;
  bool haveSessionTime = false;
  bool haveShowHidden = false;
  bool haveRemoveRecents = false;
  bool haveMoveFinished = false;

  const bool spectralTheme =
      static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme) == CrossPointSettings::UI_THEME::SPECTRAL;

  for (auto& setting : getSettingsList(&sdFontSystem.registry(), &dictionaries)) {
    if (setting.category == StrId::STR_NONE_OPT) continue;
    // Spectral side-button map only when Spectral is the active home theme.
    if (setting.key && (strcmp(setting.key, "spectralSideLeft") == 0 || strcmp(setting.key, "spectralSideRight") == 0) &&
        !spectralTheme) {
      continue;
    }
    if (setting.category == StrId::STR_CAT_DISPLAY) {
      displaySettings.push_back(setting);
    } else if (setting.category == StrId::STR_CAT_READER) {
      // Settings merged into Manage Fonts (TextSettingsActivity)
      if (setting.inTextSettings) continue;
      readerSettings.push_back(setting);
    } else if (setting.category == StrId::STR_CAT_CONTROLS) {
      if (setting.valuePtr == &CrossPointSettings::pwrBtnFootnoteBack &&
          SETTINGS.shortPwrBtn != CrossPointSettings::SHORT_PWRBTN::FOOTNOTES &&
          SETTINGS.longPwrBtn != CrossPointSettings::SHORT_PWRBTN::FOOTNOTES) {
        continue;
      }
      controlsSettings.push_back(setting);
    } else if (setting.category == StrId::STR_CAT_SYSTEM) {
      // Capture for ordered System list (do not push yet).
      if (setting.nameId == StrId::STR_TIME_TO_SLEEP) {
        timeToSleep = setting;
        haveTimeToSleep = true;
      } else if (setting.nameId == StrId::STR_SESSION_TIME) {
        sessionTime = setting;
        haveSessionTime = true;
      } else if (setting.nameId == StrId::STR_SHOW_HIDDEN_FILES) {
        showHidden = setting;
        haveShowHidden = true;
      } else if (setting.nameId == StrId::STR_REMOVE_READ_FROM_RECENTS) {
        removeRecents = setting;
        haveRemoveRecents = true;
      } else if (setting.nameId == StrId::STR_MOVE_FINISHED_TO_READ) {
        moveFinished = setting;
        haveMoveFinished = true;
      }
    }
  }

  // Controls order: … → Remap Front Buttons → Orient Front Buttons → Tilt → …
  // Insert Remap immediately before Orient Front Buttons; nest Orient under Remap.
  if (!BoardConfig::hasTouch()) {
    auto insertAt = controlsSettings.end();
    for (auto it = controlsSettings.begin(); it != controlsSettings.end(); ++it) {
      if (it->nameId == StrId::STR_FRONT_BTN_FOLLOW_ORIENTATION) {
        it->nestedUnderParent = true;
        insertAt = it;
        break;
      }
    }
    controlsSettings.insert(insertAt,
                            SettingInfo::Action(StrId::STR_REMAP_FRONT_BUTTONS, SettingAction::RemapFrontButtons));
  }

  // Display order: Status Bar (system top chrome) → Theme → …
  displaySettings.insert(displaySettings.begin(),
                         SettingInfo::Action(StrId::STR_STATUS_BAR, SettingAction::SystemStatusBar));

  // System order: Wi‑Fi → Stats (folder) → Session Time → …
  // … → Language → KOReader Sync → OPDS → SD firmware → Check for Updates (last).
  // Backup / Auto Backup / Enable Tracking live inside Stats — not duplicated here.
  systemSettings.push_back(SettingInfo::Action(StrId::STR_WIFI_NETWORKS, SettingAction::Network));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_STATS, SettingAction::Stats));
  if (haveSessionTime) {
    systemSettings.push_back(sessionTime);
  } else {
    systemSettings.push_back(SettingInfo::Value(
        StrId::STR_SESSION_TIME, &CrossPointSettings::readingSessionIdleMinutes,
        {CrossPointSettings::MIN_SESSION_IDLE_MINUTES, CrossPointSettings::MAX_SESSION_IDLE_MINUTES, 1},
        "readingSessionIdleMinutes"));
  }
  if (haveTimeToSleep) systemSettings.push_back(timeToSleep);
  if (haveShowHidden) systemSettings.push_back(showHidden);
  if (haveRemoveRecents) systemSettings.push_back(removeRecents);
  if (haveMoveFinished) systemSettings.push_back(moveFinished);
  systemSettings.push_back(SettingInfo::Action(StrId::STR_LANGUAGE, SettingAction::Language));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_KOREADER_SYNC, SettingAction::KOReaderSync));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_OPDS_SERVERS, SettingAction::OPDSBrowser));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_SD_FIRMWARE_UPDATE, SettingAction::SdFirmwareUpdate));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_CHECK_UPDATES, SettingAction::CheckForUpdates));

  // Reader: Status Bar, Manage Fonts (includes Download tab), Dictionary.
  // Download Fonts is no longer a separate top-level Reader row.
  readerSettings.insert(readerSettings.begin(),
                        SettingInfo::Action(StrId::STR_CUSTOMISE_STATUS_BAR, SettingAction::CustomiseStatusBar));
  readerSettings.insert(readerSettings.begin() + 1,
                        SettingInfo::Action(StrId::STR_TEXT_SETTINGS, SettingAction::TextSettings));
  if (!dictionaries.empty()) {
    readerSettings.insert(readerSettings.begin() + 2,
                          SettingInfo::Action(StrId::STR_DICTIONARY, SettingAction::Dictionary));
  }

  // Update currentSettings pointer and count for the active category
  switch (selectedCategoryIndex) {
    case 0:
      currentSettings = &displaySettings;
      break;
    case 1:
      currentSettings = &readerSettings;
      break;
    case 2:
      currentSettings = &controlsSettings;
      break;
    case 3:
      currentSettings = &systemSettings;
      break;
  }
  settingsCount = static_cast<int>(currentSettings->size());
}

void SettingsActivity::onEnter() {
  Activity::onEnter();

  // Reset selection to first category
  selectedCategoryIndex = 0;
  selectedSettingIndex = 0;
  preserveQuickResumeTimeoutOn =
      SETTINGS.quickResumeSleepScreen == CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT;
  quickResumeTimeoutAutoEnabled = false;
  syncQuickResumeTimeoutForSleepScreen(/*sleepScreenChanged=*/true, /*quickResumeTimeoutChanged=*/false);

  rebuildSettingsLists();

  // Trigger first update
  requestUpdate();
}

void SettingsActivity::flushSettingsIfDirty() {
  if (!settingsDirty) {
    return;
  }
  SETTINGS.saveToFile();
  settingsDirty = false;
}

void SettingsActivity::onExit() {
  Activity::onExit();

  flushSettingsIfDirty();
  UITheme::getInstance().reload();  // Re-apply theme in case it was changed
}

void SettingsActivity::loop() {
  if (optionPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return;

  bool hasChangedCategory = false;

  auto applyCategorySelection = [this] {
    switch (selectedCategoryIndex) {
      case 0:
        currentSettings = &displaySettings;
        break;
      case 1:
        currentSettings = &readerSettings;
        break;
      case 2:
        currentSettings = &controlsSettings;
        break;
      case 3:
        currentSettings = &systemSettings;
        break;
    }
    settingsCount = static_cast<int>(currentSettings->size());
  };

  // Handle actions with early return
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    if (selectedSettingIndex == 0) {
      selectedCategoryIndex = (selectedCategoryIndex < categoryCount - 1) ? (selectedCategoryIndex + 1) : 0;
      hasChangedCategory = true;
      requestUpdate();
    } else {
      toggleCurrentSetting();
      requestUpdate();
      return;
    }
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    // One Back always leaves Settings (option popups consume Back first).
    // Previously, focus on a row required a second press to actually exit.
    // Prefer finish()/pop so stacked Home resumes (goToSettings pushes Home).
    // onGoHome() still works via empty-stack goHome, but would also force
    // initialMenuItem=SETTINGS_MENU when Home was replaced — landing classic
    // themes on the Settings row and confusing residual-Back handling.
    // onExit() flushes dirty settings; save once, not on every toggle.
    finish();
    return;
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  int tx = 0;
  int ty = 0;
  const int tabTop = metrics.topPadding + metrics.headerHeight;
  const int listTop = metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing;
  // System tab reserves a band for the firmware version under the last row.
  const int systemVersionBand =
      (selectedCategoryIndex == categoryCount - 1) ? (renderer.getLineHeight(UI_10_FONT_ID) + 10) : 0;
  const int listHeight =
      renderer.getScreenHeight() - (metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight +
                                    metrics.buttonHintsHeight + metrics.verticalSpacing * 2 + systemVersionBand);
  auto buildTabs = [&]() {
    std::vector<TabInfo> tabs;
    tabs.reserve(categoryCount);
    for (int i = 0; i < categoryCount; i++) {
      tabs.push_back({I18N.get(categoryNames[i]), selectedCategoryIndex == i});
    }
    return tabs;
  };
  auto settingIndexFromPoint = [&](const int x, const int y, int& settingIndex) {
    (void)x;
    if (settingsCount <= 0 || y < listTop || y >= listTop + listHeight) return false;
    const int rowStep = GUI.getListRowStep(false);
    if (rowStep <= 0) return false;
    const int pageItems = GUI.getListPageItems(listHeight, false);
    const int selectedRow = std::max(0, selectedSettingIndex - 1);
    const int pageStart = selectedRow / pageItems * pageItems;
    const int row = (y - listTop) / rowStep;
    const int touched = pageStart + row;
    if (row < 0 || row >= pageItems || touched < 0 || touched >= settingsCount) return false;
    settingIndex = touched + 1;
    return true;
  };

  if (mappedInput.wasScreenTouchDown(tx, ty)) {
    int touchedCategory = -1;
    const auto tabs = buildTabs();
    if (GUI.tabIndexFromPoint(renderer, Rect{0, tabTop, renderer.getScreenWidth(), metrics.tabBarHeight}, tabs, tx, ty,
                              touchedCategory)) {
      if (selectedCategoryIndex != touchedCategory || selectedSettingIndex != 0) {
        selectedCategoryIndex = touchedCategory;
        selectedSettingIndex = 0;
        applyCategorySelection();
        requestUpdate();
      }
      return;
    }

    int touchedSetting = -1;
    if (settingIndexFromPoint(tx, ty, touchedSetting)) {
      if (selectedSettingIndex != touchedSetting) {
        selectedSettingIndex = touchedSetting;
        requestUpdate();
      }
      return;
    }
  }

  if (mappedInput.wasScreenTapped(tx, ty)) {
    int tappedCategory = -1;
    const auto tabs = buildTabs();
    if (GUI.tabIndexFromPoint(renderer, Rect{0, tabTop, renderer.getScreenWidth(), metrics.tabBarHeight}, tabs, tx, ty,
                              tappedCategory)) {
      selectedCategoryIndex = tappedCategory;
      selectedSettingIndex = 0;
      applyCategorySelection();
      requestUpdate();
      return;
    }

    int tappedSetting = -1;
    if (settingIndexFromPoint(tx, ty, tappedSetting)) {
      selectedSettingIndex = tappedSetting;
      toggleCurrentSetting();
      requestUpdate();
      return;
    }
  }

  // Handle navigation
  const auto& navMetrics = UITheme::getInstance().getMetrics();
  const int navSystemVersionBand =
      (selectedCategoryIndex == categoryCount - 1) ? (renderer.getLineHeight(UI_10_FONT_ID) + 10) : 0;
  const int settingsListHeight =
      renderer.getScreenHeight() - (navMetrics.topPadding + navMetrics.headerHeight + navMetrics.tabBarHeight +
                                    navMetrics.buttonHintsHeight + navMetrics.verticalSpacing * 2 + navSystemVersionBand);
  const int settingsPageItems = GUI.getListPageItems(settingsListHeight, false);
  // Front Up/Down: within-category list ring only
  //   0 = this tab's label, 1..N = list rows (Down past last → tab label).
  // Side Up/Down: always previous/next tab, whether focus is on the tab bar or a list row.
  const int ringSize = settingsCount + 1;
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up) {
    selectedSettingIndex = selectedSettingIndex == 0
                               ? 1
                               : ButtonNavigator::nextPageIndex(selectedSettingIndex, ringSize, settingsPageItems);
    requestUpdate();
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Down) {
    selectedSettingIndex = ButtonNavigator::previousPageIndex(selectedSettingIndex, ringSize, settingsPageItems);
    requestUpdate();
    return;
  }

  auto moveListNext = [this, ringSize] {
    selectedSettingIndex = ButtonNavigator::nextIndex(selectedSettingIndex, ringSize);
    requestUpdate();
  };
  auto moveListPrev = [this, ringSize] {
    selectedSettingIndex = ButtonNavigator::previousIndex(selectedSettingIndex, ringSize);
    requestUpdate();
  };
  // Tab changes (side or Confirm on tab bar) preserve focus depth:
  // tab-bar focus stays on the tab bar so Select can cycle tabs without interruption.
  auto moveTabNext = [this, &hasChangedCategory] {
    hasChangedCategory = true;
    selectedCategoryIndex = ButtonNavigator::nextIndex(selectedCategoryIndex, categoryCount);
    requestUpdate();
  };
  auto moveTabPrev = [this, &hasChangedCategory] {
    hasChangedCategory = true;
    selectedCategoryIndex = ButtonNavigator::previousIndex(selectedCategoryIndex, categoryCount);
    requestUpdate();
  };

  buttonNavigator.onRelease(ButtonNavigator::getFrontNextButtons(), moveListNext);
  buttonNavigator.onRelease(ButtonNavigator::getFrontPreviousButtons(), moveListPrev);
  buttonNavigator.onContinuous(ButtonNavigator::getFrontNextButtons(), moveListNext);
  buttonNavigator.onContinuous(ButtonNavigator::getFrontPreviousButtons(), moveListPrev);

  buttonNavigator.onRelease(ButtonNavigator::getSideNextButtons(), moveTabNext);
  buttonNavigator.onRelease(ButtonNavigator::getSidePreviousButtons(), moveTabPrev);
  buttonNavigator.onContinuous(ButtonNavigator::getSideNextButtons(), moveTabNext);
  buttonNavigator.onContinuous(ButtonNavigator::getSidePreviousButtons(), moveTabPrev);

  if (hasChangedCategory) {
    const int priorFocus = selectedSettingIndex;
    applyCategorySelection();
    // Keep tab-bar vs list depth; clamp list index into the new tab.
    if (priorFocus <= 0) {
      selectedSettingIndex = 0;
    } else if (settingsCount <= 0) {
      selectedSettingIndex = 0;
    } else if (priorFocus > settingsCount) {
      selectedSettingIndex = settingsCount;
    } else {
      selectedSettingIndex = priorFocus;
    }
  }
}

void SettingsActivity::toggleCurrentSetting() {
  int selectedSetting = selectedSettingIndex - 1;
  if (selectedSetting < 0 || selectedSetting >= settingsCount) {
    return;
  }

  const auto& setting = (*currentSettings)[selectedSetting];
  const bool sleepScreenChanged = setting.valuePtr == &CrossPointSettings::sleepScreen;
  const bool quickResumeTimeoutChanged = setting.valuePtr == &CrossPointSettings::quickResumeSleepScreen;

  if (setting.nameId == StrId::STR_TIME_TO_SLEEP) {
    openSleepTimeoutPicker();
    return;
  }
  if (setting.nameId == StrId::STR_SESSION_TIME ||
      setting.valuePtr == &CrossPointSettings::readingSessionIdleMinutes) {
    openSessionTimePicker();
    return;
  }

  if (setting.type == SettingType::TOGGLE && setting.valuePtr != nullptr) {
    // Toggle the boolean value using the member pointer
    const bool currentValue = SETTINGS.*(setting.valuePtr);
    SETTINGS.*(setting.valuePtr) = !currentValue;
  } else if (setting.type == SettingType::ENUM && setting.valuePtr != nullptr) {
    // Always open a popup for multi-choice enums (including 2 options, e.g. Theme).
    // Only true TOGGLE settings cycle on Confirm without a list.
    const uint8_t currentValue = SETTINGS.*(setting.valuePtr);
    if (setting.enumValues.size() >= 2) {
      const auto valuePtr = setting.valuePtr;
      optionPopup.show(setting.nameId, setting.enumValues.data(), static_cast<int>(setting.enumValues.size()),
                       currentValue, [this, valuePtr, sleepScreenChanged, quickResumeTimeoutChanged](int idx) {
                         SETTINGS.*valuePtr = idx;
                         syncQuickResumeTimeoutForSleepScreen(sleepScreenChanged, quickResumeTimeoutChanged);
                         markSettingsDirty();
                         rebuildSettingsLists();
                       });
      requestUpdate();
      return;
    }
    SETTINGS.*(setting.valuePtr) = (currentValue + 1) % static_cast<uint8_t>(setting.enumValues.size());
  } else if (setting.type == SettingType::ENUM && setting.valueGetter && setting.valueSetter) {
    const uint8_t totalValues = setting.enumStringValues.empty()
                                    ? static_cast<uint8_t>(setting.enumValues.size())
                                    : static_cast<uint8_t>(setting.enumStringValues.size());
    const uint8_t cur = setting.valueGetter();
    if (totalValues >= 2) {
      const auto valueSetter = setting.valueSetter;
      const bool isUiTheme = setting.nameId == StrId::STR_UI_THEME;
      auto onSelect = [this, valueSetter, sleepScreenChanged, quickResumeTimeoutChanged, isUiTheme](int idx) {
        valueSetter(idx);
        if (isUiTheme) {
          UITheme::getInstance().reload();
        }
        syncQuickResumeTimeoutForSleepScreen(sleepScreenChanged, quickResumeTimeoutChanged);
        markSettingsDirty();
        rebuildSettingsLists();
      };
      if (!setting.enumStringValues.empty()) {
        optionPopup.show(setting.nameId, setting.enumStringValues, cur, std::move(onSelect));
      } else {
        optionPopup.show(setting.nameId, setting.enumValues.data(), static_cast<int>(setting.enumValues.size()), cur,
                         std::move(onSelect));
      }
      requestUpdate();
      return;
    }
    setting.valueSetter((cur + 1) % totalValues);
    if (setting.nameId == StrId::STR_UI_THEME) {
      UITheme::getInstance().reload();
    }
  } else if (setting.type == SettingType::VALUE && setting.valuePtr != nullptr) {
    const int8_t currentValue = SETTINGS.*(setting.valuePtr);
    if (currentValue + setting.valueRange.step > setting.valueRange.max) {
      SETTINGS.*(setting.valuePtr) = setting.valueRange.min;
    } else {
      SETTINGS.*(setting.valuePtr) = currentValue + setting.valueRange.step;
    }
  } else if (setting.type == SettingType::ACTION) {
    // Persist parent toggles before pushing a child (power-loss safety); children
    // that mutate SETTINGS should also mark dirty or save themselves.
    flushSettingsIfDirty();
    auto resultHandler = [this](const ActivityResult&) {
      // One SD write when returning from a child (children often mutate SETTINGS).
      SETTINGS.saveToFile();
      settingsDirty = false;
    };

    switch (setting.action) {
      case SettingAction::RemapFrontButtons:
        startActivityForResult(std::make_unique<ButtonRemapActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::CustomiseStatusBar:
        startActivityForResult(std::make_unique<StatusBarSettingsActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::SystemStatusBar:
        startActivityForResult(std::make_unique<SystemStatusBarSettingsActivity>(renderer, mappedInput),
                               resultHandler);
        break;
      case SettingAction::ClockSettings:
        // Legacy entry; clock options now live under Display → Status Bar.
        startActivityForResult(std::make_unique<ClockSettingsActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::KOReaderSync:
        startActivityForResult(std::make_unique<KOReaderSettingsActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::OPDSBrowser:
        startActivityForResult(std::make_unique<OpdsServerListActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::Network:
        startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput, false), resultHandler);
        break;
      case SettingAction::ClearCache:
        startActivityForResult(std::make_unique<ClearCacheActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::BackupStats:
        startActivityForResult(std::make_unique<BackupStatsActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::Stats:
        startActivityForResult(std::make_unique<StatsSettingsActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::CheckForUpdates:
        startActivityForResult(std::make_unique<OtaUpdateActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::SdFirmwareUpdate:
        startActivityForResult(std::make_unique<SdFirmwareUpdateActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::DownloadFonts:
        startActivityForResult(std::make_unique<FontDownloadActivity>(renderer, mappedInput),
                               [this](const ActivityResult&) {
                                 SETTINGS.saveToFile();
                                 settingsDirty = false;
                                 rebuildSettingsLists();
                               });
        break;
      case SettingAction::TextSettings:
        startActivityForResult(std::make_unique<TextSettingsActivity>(renderer, mappedInput, &sdFontSystem.registry(),
                                                                      TextSettingsActivity::Tab::Family),
                               [this](const ActivityResult&) {
                                 SETTINGS.saveToFile();
                                 settingsDirty = false;
                                 rebuildSettingsLists();
                               });
        break;
      case SettingAction::Language:
        startActivityForResult(std::make_unique<LanguageSelectActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::Dictionary:
        startActivityForResult(std::make_unique<DictionarySelectActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::None:
        // Do nothing
        break;
    }
    return;  // Results will be handled in the result handler, so we can return early here
  } else {
    return;
  }

  syncQuickResumeTimeoutForSleepScreen(sleepScreenChanged, quickResumeTimeoutChanged);
  markSettingsDirty();
  rebuildSettingsLists();
  selectedSettingIndex = std::min(selectedSettingIndex, settingsCount);
}

void SettingsActivity::syncQuickResumeTimeoutForSleepScreen(bool sleepScreenChanged, bool quickResumeTimeoutChanged) {
  if (quickResumeTimeoutChanged) {
    preserveQuickResumeTimeoutOn =
        SETTINGS.quickResumeSleepScreen == CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT;
    quickResumeTimeoutAutoEnabled = false;
  }

  if (SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::QUICK_RESUME) {
    if (SETTINGS.quickResumeSleepScreen != CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT) {
      SETTINGS.quickResumeSleepScreen = CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT;
      quickResumeTimeoutAutoEnabled = !preserveQuickResumeTimeoutOn;
    } else if (sleepScreenChanged && !preserveQuickResumeTimeoutOn) {
      quickResumeTimeoutAutoEnabled = true;
    }
    return;
  }

  if (sleepScreenChanged && quickResumeTimeoutAutoEnabled && !preserveQuickResumeTimeoutOn) {
    SETTINGS.quickResumeSleepScreen = CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_NEVER;
    quickResumeTimeoutAutoEnabled = false;
  }
}

void SettingsActivity::openSleepTimeoutPicker() {
  startActivityForResult(
      std::make_unique<IntervalSelectionActivity>(
          renderer, mappedInput, "SleepTimeoutInterval", StrId::STR_TIME_TO_SLEEP, SETTINGS.sleepTimeoutMinutes,
          CrossPointSettings::MIN_SLEEP_TIMEOUT_MINUTES, CrossPointSettings::MAX_SLEEP_TIMEOUT_MINUTES, 1, 5,
          StrId::STR_SLEEP_TIMER_VALUE_FORMAT, false, true, StrId::STR_SLEEP_NEVER),
      [this](const ActivityResult& result) {
        if (!result.isCancelled) {
          SETTINGS.sleepTimeoutMinutes = static_cast<uint8_t>(std::get<IntervalResult>(result.data).value);
          markSettingsDirty();
        }
        requestUpdate();
      });
}

void SettingsActivity::openSessionTimePicker() {
  // Idle gap before a reading stretch stops counting toward stats (minutes).
  startActivityForResult(
      std::make_unique<IntervalSelectionActivity>(
          renderer, mappedInput, "SessionTimeInterval", StrId::STR_SESSION_TIME,
          static_cast<int>(SETTINGS.readingSessionIdleMinutes),
          static_cast<int>(CrossPointSettings::MIN_SESSION_IDLE_MINUTES),
          static_cast<int>(CrossPointSettings::MAX_SESSION_IDLE_MINUTES), 1, 5, StrId::STR_SLEEP_TIMER_VALUE_FORMAT,
          false, true),
      [this](const ActivityResult& result) {
        if (!result.isCancelled) {
          SETTINGS.readingSessionIdleMinutes =
              static_cast<uint8_t>(std::get<IntervalResult>(result.data).value);
          markSettingsDirty();
        }
        requestUpdate();
      });
}

void SettingsActivity::render(RenderLock&&) {
  if (optionPopup.processRender(renderer, mappedInput)) return;

  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  const auto& metrics = UITheme::getInstance().getMetrics();

  // Version lives under System (centered above button hints), not in the header.
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_SETTINGS_TITLE));

  std::vector<TabInfo> tabs;
  tabs.reserve(categoryCount);
  for (int i = 0; i < categoryCount; i++) {
    tabs.push_back({I18N.get(categoryNames[i]), selectedCategoryIndex == i});
  }
  GUI.drawTabBar(renderer, Rect{0, metrics.topPadding + metrics.headerHeight, pageWidth, metrics.tabBarHeight}, tabs,
                 selectedSettingIndex == 0);

  const bool systemTab = selectedCategoryIndex == categoryCount - 1;
  const int versionBand = systemTab ? (renderer.getLineHeight(UI_10_FONT_ID) + 10) : 0;
  const int listTop = metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing;
  const int listHeight = pageHeight - (metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight +
                                       metrics.buttonHintsHeight + metrics.verticalSpacing * 2 + versionBand);

  const auto& settings = *currentSettings;
  GUI.drawList(
      renderer, Rect{0, listTop, pageWidth, listHeight},
      settingsCount, selectedSettingIndex - 1,
      [&settings](int index) {
        return NestedMenuLabel::format(I18N.get(settings[index].nameId), settings[index].nestedUnderParent);
      },
      nullptr, nullptr,
      [&settings](int i) {
        const auto& setting = settings[i];
        std::string valueText = "";
        if (setting.type == SettingType::TOGGLE && setting.valuePtr != nullptr) {
          const bool value = SETTINGS.*(setting.valuePtr);
          valueText = value ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
        } else if (setting.type == SettingType::ENUM && setting.valuePtr != nullptr) {
          const uint8_t value = SETTINGS.*(setting.valuePtr);
          valueText = I18N.get(setting.enumValues[value]);
        } else if (setting.type == SettingType::ENUM && setting.valueGetter) {
          const uint8_t value = setting.valueGetter();
          if (!setting.enumStringValues.empty() && value < setting.enumStringValues.size()) {
            valueText = setting.enumStringValues[value];
          } else if (value < setting.enumValues.size()) {
            valueText = I18N.get(setting.enumValues[value]);
          }
        } else if (setting.type == SettingType::VALUE && setting.valuePtr != nullptr) {
          if (setting.nameId == StrId::STR_TIME_TO_SLEEP) {
            char valueBuffer[32];
            if (SETTINGS.sleepTimeoutMinutes >= CrossPointSettings::SLEEP_TIMEOUT_NEVER_MINUTES) {
              valueText = tr(STR_SLEEP_NEVER);
            } else {
              snprintf(valueBuffer, sizeof(valueBuffer), tr(STR_SLEEP_TIMER_VALUE_FORMAT),
                       static_cast<unsigned int>(SETTINGS.*(setting.valuePtr)));
              valueText = valueBuffer;
            }
          } else if (setting.nameId == StrId::STR_SESSION_TIME ||
                     setting.valuePtr == &CrossPointSettings::readingSessionIdleMinutes) {
            char valueBuffer[32];
            snprintf(valueBuffer, sizeof(valueBuffer), tr(STR_SLEEP_TIMER_VALUE_FORMAT),
                     static_cast<unsigned int>(SETTINGS.readingSessionIdleMinutes));
            valueText = valueBuffer;
          } else {
            valueText = std::to_string(SETTINGS.*(setting.valuePtr));
          }
        } else if (setting.type == SettingType::ACTION) {
          // Submenu / folder affordance: keeps focus chip on the right like On/Off rows.
          valueText = ">";
        }
        return valueText;
      },
      true);

  // Confirm hint: next tab name on the tab bar; Toggle only for true on/off
  // settings; Select for enums, actions, values, and multi-choice pickers.
  const char* confirmLabel = tr(STR_SELECT);
  if (selectedSettingIndex == 0) {
    confirmLabel = I18N.get(categoryNames[(selectedCategoryIndex + 1) % categoryCount]);
  } else if (selectedSettingIndex > 0) {
    const auto& setting = (*currentSettings)[selectedSettingIndex - 1];
    if (setting.type == SettingType::TOGGLE) {
      confirmLabel = tr(STR_TOGGLE);
    } else {
      confirmLabel = tr(STR_SELECT);
    }
  }

  // Front Up/Down: list. Side: switch tabs. Confirm on tab bar also advances tab.
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  // System: firmware version centered between the last row and the button hints.
  if (systemTab) {
    const int bandTop = listTop + listHeight;
    const int bandBottom = pageHeight - metrics.buttonHintsHeight;
    const int textH = renderer.getLineHeight(UI_10_FONT_ID);
    const int textY = bandTop + std::max(0, (bandBottom - bandTop - textH) / 2);
    renderer.drawCenteredText(UI_10_FONT_ID, textY, CROSSPOINT_VERSION, true);
  }

  // Always use standard refresh for settings screen
  renderer.displayBuffer();
}
