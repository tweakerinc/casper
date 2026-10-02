#include "SettingsActivity.h"

#include <BoardConfig.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalGPIO.h>
#include <Logging.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "BackupStatsActivity.h"
#include "ButtonRemapActivity.h"
#include "ClearCacheActivity.h"
#include "ClockSettingsActivity.h"
#include "CrossPointSettings.h"
#include "DictionarySelectActivity.h"
#include "FontDownloadActivity.h"
#include "KOReaderSettingsActivity.h"
#include "LanguageSelectActivity.h"
#include "MappedInputManager.h"
#include "NetworkSettingsActivity.h"
#include "OpdsServerListActivity.h"
#include "OtaUpdateActivity.h"
#include "SdCardFontSystem.h"
#include "SdFirmwareUpdateActivity.h"
#include "SettingsList.h"
#include "SilentRestart.h"
#include "StatsSettingsActivity.h"
#include "StatusBarSettingsActivity.h"
#include "SystemStatusBarSettingsActivity.h"
#include "TextSettingsActivity.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/IntervalSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/NestedMenuLabel.h"
#include "util/SystemLog.h"
#include "util/UiGhostPolicy.h"

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
  SettingInfo sessionTime{};
  SettingInfo showHidden{};
  SettingInfo removeRecents{};
  SettingInfo moveFinished{};
  SettingInfo enableLogging{};
  bool haveSessionTime = false;
  bool haveShowHidden = false;
  bool haveRemoveRecents = false;
  bool haveMoveFinished = false;
  bool haveEnableLogging = false;

  for (auto& setting : getSettingsList(&sdFontSystem.registry(), &dictionaries)) {
    if (setting.category == StrId::STR_NONE_OPT) continue;
    // Never show legacy Penumbra/Spectral side remap (removed; behavior is fixed per device).
    if (setting.key &&
        (strcmp(setting.key, "spectralSideLeft") == 0 || strcmp(setting.key, "spectralSideRight") == 0)) {
      continue;
    }
    // Also hide by name in case a stale list entry lacks the key.
    if (setting.nameId == StrId::STR_PENUMBRA_LEFT_BUTTON || setting.nameId == StrId::STR_PENUMBRA_RIGHT_BUTTON ||
        setting.nameId == StrId::STR_PENUMBRA_UP_BUTTON || setting.nameId == StrId::STR_PENUMBRA_DOWN_BUTTON) {
      continue;
    }
    if (setting.category == StrId::STR_CAT_DISPLAY) {
      // Cover crop/filter only apply to wallpaper Sleep Screens, not last-frame QR.
      if (SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::QUICK_RESUME &&
          (setting.nameId == StrId::STR_SLEEP_COVER_MODE || setting.nameId == StrId::STR_SLEEP_COVER_FILTER ||
           (setting.key && (strcmp(setting.key, "sleepScreenCoverMode") == 0 ||
                            strcmp(setting.key, "sleepScreenCoverFilter") == 0)))) {
        continue;
      }
      // Nested under Dark Mode — only list when Dark Mode is On.
      if ((setting.nameId == StrId::STR_DARK_MODE_READER_ONLY ||
           (setting.key && strcmp(setting.key, "darkModeReaderOnly") == 0)) &&
          !SETTINGS.readerDarkMode) {
        continue;
      }
      displaySettings.push_back(setting);
    } else if (setting.category == StrId::STR_CAT_READER) {
      // Settings merged into Manage Fonts (TextSettingsActivity)
      if (setting.inTextSettings) continue;
      // Device-specific labels for side long-press (same fields, X3 vs X4 names).
      const bool x3 = gpio.deviceIsX3();
      if ((setting.nameId == StrId::STR_LONG_PRESS_SIDE_A_X3 || setting.nameId == StrId::STR_LONG_PRESS_SIDE_B_X3) &&
          !x3) {
        continue;
      }
      if ((setting.nameId == StrId::STR_LONG_PRESS_SIDE_A_X4 || setting.nameId == StrId::STR_LONG_PRESS_SIDE_B_X4) &&
          x3) {
        continue;
      }
      // Nested under Flip Orientation — hide unless either side is Flip.
      if ((setting.nameId == StrId::STR_ORIENTATION_FLIP_WITH ||
           (setting.key && strcmp(setting.key, "orientationFlipWith") == 0)) &&
          SETTINGS.longPressSideA != CrossPointSettings::LP_MENU_ORIENTATION_FLIP &&
          SETTINGS.longPressSideB != CrossPointSettings::LP_MENU_ORIENTATION_FLIP) {
        continue;
      }
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
      // Time to Sleep is under Display (above Sleep Screen).
      if (setting.nameId == StrId::STR_SESSION_TIME) {
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
      } else if (setting.nameId == StrId::STR_ENABLE_LOGGING ||
                 (setting.key && strcmp(setting.key, "systemLogLevel") == 0)) {
        enableLogging = setting;
        haveEnableLogging = true;
      }
    }
  }

  // Controls order: … → Long-Press Power → Remap → Tilt → …
  if (!BoardConfig::hasTouch()) {
    auto insertAt = controlsSettings.end();
    for (auto it = controlsSettings.begin(); it != controlsSettings.end(); ++it) {
      if (it->nameId == StrId::STR_TILT_PAGE_TURN) {
        insertAt = it;
        break;
      }
      if (it->nameId == StrId::STR_LONG_PRESS_ACTION) {
        insertAt = it + 1;
      }
    }
    controlsSettings.insert(insertAt,
                            SettingInfo::Action(StrId::STR_REMAP_FRONT_BUTTONS, SettingAction::RemapFrontButtons));
  }

  // Display order: Status Bar (system top chrome) → Theme → …
  displaySettings.insert(displaySettings.begin(),
                         SettingInfo::Action(StrId::STR_STATUS_BAR, SettingAction::SystemStatusBar));

  // System order: Network → Stats (X3) → Move Finished
  //   [→ Clear Recents nested, only if Move On] → Language → Show Hidden →
  //   Enable Logging → SD firmware → Check for Updates.
  // Network folder: Wi‑Fi, KOReader Sync, OPDS. Session Time lives under Stats.
  // Time to Sleep is under Display (above Sleep Screen).
  systemSettings.push_back(SettingInfo::Action(StrId::STR_NETWORK, SettingAction::NetworkFolder));
  if (gpio.deviceIsX3()) {
    systemSettings.push_back(SettingInfo::Action(StrId::STR_STATS, SettingAction::Stats));
  }
  // Session Time is edited inside Stats (not listed here).
  (void)haveSessionTime;
  (void)sessionTime;
  if (haveMoveFinished) systemSettings.push_back(moveFinished);
  // Child of Move Finished — only visible when the parent is On.
  if (haveRemoveRecents && SETTINGS.moveFinishedToReadFolder) {
    removeRecents.nestedUnderParent = true;
    systemSettings.push_back(removeRecents);
  }
  systemSettings.push_back(SettingInfo::Action(StrId::STR_LANGUAGE, SettingAction::Language));
  if (haveShowHidden) systemSettings.push_back(showHidden);
  if (haveEnableLogging) {
    systemSettings.push_back(enableLogging);
  } else {
    // Fallback if the shared list entry is missing (should not happen).
    systemSettings.push_back(SettingInfo::DynamicEnum(
        StrId::STR_ENABLE_LOGGING, {StrId::STR_STATE_OFF, StrId::STR_STATE_ON},
        [] { return static_cast<uint8_t>(SETTINGS.systemLogLevel != CrossPointSettings::SYSTEM_LOG_OFF ? 1 : 0); },
        [](uint8_t on) {
          SETTINGS.systemLogLevel = on ? static_cast<uint8_t>(CrossPointSettings::SYSTEM_LOG_TIMING)
                                       : static_cast<uint8_t>(CrossPointSettings::SYSTEM_LOG_OFF);
        },
        "systemLogLevel"));
  }
  systemSettings.push_back(SettingInfo::Action(StrId::STR_SD_FIRMWARE_UPDATE, SettingAction::SdFirmwareUpdate));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_CHECK_UPDATES, SettingAction::CheckForUpdates));

  // Reader: Status Bar, Manage Reader Fonts (includes Download tab), Dictionary.
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
  // First paint: displaySoftOpen (X3 FAST+settle, X4 one HALF). Cursor FAST.

  // Reset selection to first category
  selectedCategoryIndex = 0;
  selectedSettingIndex = 0;

  rebuildSettingsLists();

  // Home long-press Menu opens Settings while the key is still held. If that
  // physical slot is remapped (e.g. Bottom1 → Left), continuous tab nav would
  // start scrolling immediately. Wait until every nav key is up.
  armAwaitOpenButtonRelease();

  // Open plate: X3 FAST+settle, X4 one HALF (SSD1677 has no mid bank). List nav
  // stays FAST via displayMenuFrame.
  UiGhostPolicy::clearHardScrub();
  requestUpdate();
}

void SettingsActivity::onResume() {
  Activity::onResume();
  // Returning from a child (esp. Button Remap): Back was pressed under a locked
  // layout, then the new map applied — same physical key may now be Left/Up.
  // Without a quiet frame, Settings tab-nav would fire on that residual edge
  // (Remap Back → Controls also jumps to Reader).
  UiGhostPolicy::clearHardScrub();
  if (!gpio.deviceIsX3()) softOpenPending_ = true;
  armAwaitOpenButtonRelease(/*force=*/true);
}

void SettingsActivity::armAwaitOpenButtonRelease(const bool force) {
  using B = MappedInputManager::Button;
  const bool held =
      mappedInput.isPressed(B::Back) || mappedInput.isPressed(B::Confirm) || mappedInput.isPressed(B::Left) ||
      mappedInput.isPressed(B::Right) || mappedInput.isPressed(B::Up) || mappedInput.isPressed(B::Down) ||
      mappedInput.isFrontButtonPressed(HalGPIO::BTN_BACK) || mappedInput.isFrontButtonPressed(HalGPIO::BTN_CONFIRM) ||
      mappedInput.isFrontButtonPressed(HalGPIO::BTN_LEFT) || mappedInput.isFrontButtonPressed(HalGPIO::BTN_RIGHT);
  // force: always drain (child may have left a wasPressed/wasReleased edge with no hold).
  awaitOpenButtonRelease = force || held;
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
  if (awaitOpenButtonRelease) {
    using B = MappedInputManager::Button;
    const bool held =
        mappedInput.isPressed(B::Back) || mappedInput.isPressed(B::Confirm) || mappedInput.isPressed(B::Left) ||
        mappedInput.isPressed(B::Right) || mappedInput.isPressed(B::Up) || mappedInput.isPressed(B::Down) ||
        mappedInput.isFrontButtonPressed(HalGPIO::BTN_BACK) || mappedInput.isFrontButtonPressed(HalGPIO::BTN_CONFIRM) ||
        mappedInput.isFrontButtonPressed(HalGPIO::BTN_LEFT) || mappedInput.isFrontButtonPressed(HalGPIO::BTN_RIGHT);
    if (held) {
      return;
    }
    // Drain residual edges from the open gesture so they cannot exit or nav.
    (void)mappedInput.wasPressed(B::Back);
    (void)mappedInput.wasReleased(B::Back);
    (void)mappedInput.wasPressed(B::Confirm);
    (void)mappedInput.wasReleased(B::Confirm);
    (void)mappedInput.wasPressed(B::Left);
    (void)mappedInput.wasReleased(B::Left);
    (void)mappedInput.wasPressed(B::Right);
    (void)mappedInput.wasReleased(B::Right);
    (void)mappedInput.wasPressed(B::Up);
    (void)mappedInput.wasReleased(B::Up);
    (void)mappedInput.wasPressed(B::Down);
    (void)mappedInput.wasReleased(B::Down);
    (void)mappedInput.getReleasedFrontButton();
    (void)mappedInput.getPressedFrontButton();
    awaitOpenButtonRelease = false;
    return;
  }

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

  // Touch: tabs + list (BaseTheme hit testing).
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int tabTop = metrics.topPadding + metrics.headerHeight;
  const int listTop = tabTop + metrics.tabBarHeight + metrics.verticalSpacing;
  const int versionBand =
      (selectedCategoryIndex == categoryCount - 1) ? (renderer.getLineHeight(SMALL_FONT_ID) + 4) : 0;
  const int listHeight = renderer.getScreenHeight() -
                         (metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight +
                          BaseTheme::frontButtonFooterLayoutH(renderer) + metrics.verticalSpacing * 2 + versionBand);
  int tx = 0, ty = 0;
  auto buildTabs = [this]() {
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
      renderer.getScreenHeight() -
      (navMetrics.topPadding + navMetrics.headerHeight + navMetrics.tabBarHeight +
       BaseTheme::frontButtonFooterLayoutH(renderer) + navMetrics.verticalSpacing * 2 + navSystemVersionBand);
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

  auto isHeaderFocus = [this](int focusIdx) -> bool {
    if (focusIdx <= 0 || focusIdx > settingsCount) return false;
    return (*currentSettings)[focusIdx - 1].type == SettingType::HEADER;
  };
  auto moveListNext = [this, ringSize, &isHeaderFocus] {
    for (int i = 0; i < ringSize; ++i) {
      selectedSettingIndex = ButtonNavigator::nextIndex(selectedSettingIndex, ringSize);
      if (!isHeaderFocus(selectedSettingIndex)) break;
    }
    requestUpdate();
  };
  auto moveListPrev = [this, ringSize, &isHeaderFocus] {
    for (int i = 0; i < ringSize; ++i) {
      selectedSettingIndex = ButtonNavigator::previousIndex(selectedSettingIndex, ringSize);
      if (!isHeaderFocus(selectedSettingIndex)) break;
    }
    requestUpdate();
  };
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
  if (setting.type == SettingType::HEADER) {
    return;
  }
  if (setting.nameId == StrId::STR_TIME_TO_SLEEP) {
    openSleepTimeoutPicker();
    return;
  }
  if (setting.nameId == StrId::STR_SESSION_TIME || setting.valuePtr == &CrossPointSettings::readingSessionIdleMinutes) {
    openSessionTimePicker();
    return;
  }

  if (setting.type == SettingType::TOGGLE && setting.valuePtr != nullptr) {
    // Toggle the boolean value using the member pointer
    const bool currentValue = SETTINGS.*(setting.valuePtr);
    SETTINGS.*(setting.valuePtr) = !currentValue;
    // Clear-from-recents is a child of Move Finished — hide + reset when parent turns off.
    if (setting.valuePtr == &CrossPointSettings::moveFinishedToReadFolder && !SETTINGS.moveFinishedToReadFolder) {
      SETTINGS.removeReadBooksFromRecents = 0;
    }
    // Apply whole-UI invert immediately so the Settings list flips with the toggle.
    if (setting.valuePtr == &CrossPointSettings::readerDarkMode ||
        setting.valuePtr == &CrossPointSettings::darkModeReaderOnly) {
      renderer.setInvertOnDisplay(SETTINGS.readerDarkMode != 0 && SETTINGS.darkModeReaderOnly == 0);
    }
  } else if (setting.type == SettingType::ENUM && setting.valuePtr != nullptr) {
    // Always open a popup for multi-choice enums (including 2 options, e.g. Theme).
    // Only true TOGGLE settings cycle on Confirm without a list.
    const uint8_t currentValue = SETTINGS.*(setting.valuePtr);
    if (setting.enumValues.size() >= 2) {
      const auto valuePtr = setting.valuePtr;
      const bool isEnableLogging = setting.nameId == StrId::STR_ENABLE_LOGGING ||
                                   setting.nameId == StrId::STR_SYSTEM_LOG ||
                                   (setting.key && strcmp(setting.key, "systemLogLevel") == 0);
      optionPopup.show(
          setting.nameId, setting.enumValues.data(), static_cast<int>(setting.enumValues.size()), currentValue,
          [this, valuePtr, isEnableLogging](int idx) {
            SETTINGS.*valuePtr = idx;
            // Larger menu fonts: turn Text Wrapping on by default (user can still toggle off).
            if (valuePtr == &CrossPointSettings::menuFontSize &&
                (idx == CrossPointSettings::MENU_FONT_MEDIUM || idx == CrossPointSettings::MENU_FONT_LARGE)) {
              SETTINGS.splitBookTitleLines = 1;
            }
            // Reading Orientation: seed Orient Front Buttons for that layout.
            // Portrait / Landscape CW → Off; Portrait 180° / Landscape CCW → On.
            if (valuePtr == &CrossPointSettings::orientation) {
              SETTINGS.frontButtonFollowOrientation =
                  CrossPointSettings::defaultFrontButtonFollowForOrientation(static_cast<uint8_t>(idx));
            }
            if (isEnableLogging) {
              SystemLog::reloadLevel();
            }
            markSettingsDirty();
            rebuildSettingsLists();
          });
      requestUpdate();
      return;
    }
    SETTINGS.*(setting.valuePtr) = (currentValue + 1) % static_cast<uint8_t>(setting.enumValues.size());
    if (setting.valuePtr == &CrossPointSettings::menuFontSize) {
      const uint8_t size = SETTINGS.*(setting.valuePtr);
      if (size == CrossPointSettings::MENU_FONT_MEDIUM || size == CrossPointSettings::MENU_FONT_LARGE) {
        SETTINGS.splitBookTitleLines = 1;
      }
    }
    if (setting.nameId == StrId::STR_ENABLE_LOGGING || setting.nameId == StrId::STR_SYSTEM_LOG ||
        (setting.key && strcmp(setting.key, "systemLogLevel") == 0)) {
      SystemLog::reloadLevel();
    }
  } else if (setting.type == SettingType::ENUM && setting.valueGetter && setting.valueSetter) {
    const uint8_t totalValues = setting.enumStringValues.empty()
                                    ? static_cast<uint8_t>(setting.enumValues.size())
                                    : static_cast<uint8_t>(setting.enumStringValues.size());
    const uint8_t cur = setting.valueGetter();
    const bool isEnableLogging = setting.nameId == StrId::STR_ENABLE_LOGGING ||
                                 setting.nameId == StrId::STR_SYSTEM_LOG ||
                                 (setting.key && strcmp(setting.key, "systemLogLevel") == 0);
    // Off/On switches: one Confirm flips (same as Toggle). No Off/On popup.
    if (isEnableLogging && totalValues == 2) {
      setting.valueSetter(cur ? 0 : 1);
      SystemLog::reloadLevel();
    } else if (totalValues >= 2) {
      const auto valueSetter = setting.valueSetter;
      const bool isUiTheme = setting.nameId == StrId::STR_UI_THEME;
      auto onSelect = [this, valueSetter, isUiTheme](int idx) {
        const uint8_t prevTheme = isUiTheme ? SETTINGS.uiTheme : 0;
        valueSetter(idx);
        if (isUiTheme) {
          const uint32_t tReload = millis();
          UITheme::getInstance().reload();
          SystemLog::logThemeChange(prevTheme, SETTINGS.uiTheme, millis() - tReload);
        }
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
    {
      const bool isUiTheme = setting.nameId == StrId::STR_UI_THEME;
      const uint8_t prevTheme = isUiTheme ? SETTINGS.uiTheme : 0;
      setting.valueSetter((cur + 1) % totalValues);
      if (isUiTheme) {
        const uint32_t tReload = millis();
        UITheme::getInstance().reload();
        SystemLog::logThemeChange(prevTheme, SETTINGS.uiTheme, millis() - tReload);
      }
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
        startActivityForResult(std::make_unique<SystemStatusBarSettingsActivity>(renderer, mappedInput), resultHandler);
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
      case SettingAction::NetworkFolder:
        startActivityForResult(std::make_unique<NetworkSettingsActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::Network:
        // Settings only manages credentials — it does not keep STA for a transfer.
        // Leaving LWIP up after connect drops free heap to ~17KB and re-entry OOMs.
        // Tear the radio fully so we stay in Settings (no silentRestart → Home bounce).
        startActivityForResult(
            std::make_unique<WifiSelectionActivity>(renderer, mappedInput, false), [this](const ActivityResult&) {
              SETTINGS.saveToFile();
              settingsDirty = false;
              if (WiFi.getMode() != WIFI_MODE_NULL) {
                WiFi.disconnect(true);
                WiFi.mode(WIFI_OFF);
                delay(50);
              }
              LOG_DBG("SET", "WiFi Networks exit heap=%u maxAlloc=%u", static_cast<unsigned>(ESP.getFreeHeap()),
                      static_cast<unsigned>(ESP.getMaxAllocHeap()));
              // Last-resort defrag only if teardown left the heap unusable.
              if (ESP.getMaxAllocHeap() < 12288 || ESP.getFreeHeap() < 28000) {
                silentRestart();
              }
            });
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
        startActivityForResult(std::make_unique<FontDownloadActivity>(renderer, mappedInput), resultHandler);
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

  markSettingsDirty();
  rebuildSettingsLists();
  selectedSettingIndex = std::min(selectedSettingIndex, settingsCount);
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
      std::make_unique<IntervalSelectionActivity>(renderer, mappedInput, "SessionTimeInterval", StrId::STR_SESSION_TIME,
                                                  static_cast<int>(SETTINGS.readingSessionIdleMinutes),
                                                  static_cast<int>(CrossPointSettings::MIN_SESSION_IDLE_MINUTES),
                                                  static_cast<int>(CrossPointSettings::MAX_SESSION_IDLE_MINUTES), 1, 5,
                                                  StrId::STR_SLEEP_TIMER_VALUE_FORMAT, false, true),
      [this](const ActivityResult& result) {
        if (!result.isCancelled) {
          SETTINGS.readingSessionIdleMinutes = static_cast<uint8_t>(std::get<IntervalResult>(result.data).value);
          markSettingsDirty();
        }
        requestUpdate();
      });
}

void SettingsActivity::applyCategorySelection() {
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

void SettingsActivity::render(RenderLock&&) {
  if (optionPopup.processRender(renderer, mappedInput)) return;

  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_SETTINGS_TITLE));

  std::vector<TabInfo> tabs;
  tabs.reserve(categoryCount);
  for (int i = 0; i < categoryCount; i++) {
    tabs.push_back({I18N.get(categoryNames[i]), selectedCategoryIndex == i});
  }
  GUI.drawTabBar(renderer, Rect{0, metrics.topPadding + metrics.headerHeight, pageWidth, metrics.tabBarHeight}, tabs,
                 selectedSettingIndex == 0);

  const bool systemTab = selectedCategoryIndex == categoryCount - 1;
  const int versionBand = systemTab ? (renderer.getLineHeight(SMALL_FONT_ID) + 4) : 0;
  const int listTop = metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing;
  const int listHeight =
      pageHeight - (metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight +
                    BaseTheme::frontButtonFooterLayoutH(renderer) + metrics.verticalSpacing * 2 + versionBand);

  const auto& settings = *currentSettings;
  GUI.drawList(
      renderer, Rect{0, listTop, pageWidth, listHeight}, settingsCount, selectedSettingIndex - 1,
      [&settings](int index) {
        return NestedMenuLabel::format(I18N.get(settings[index].nameId), settings[index].nestedUnderParent);
      },
      nullptr, nullptr,
      [&settings](int i) {
        const auto& setting = settings[i];
        if (setting.type == SettingType::HEADER) return std::string{};
        std::string valueText;
        if (setting.type == SettingType::TOGGLE && setting.valuePtr != nullptr) {
          valueText = SETTINGS.*(setting.valuePtr) ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
        } else if (setting.type == SettingType::ENUM && setting.valuePtr != nullptr) {
          const uint8_t value = SETTINGS.*(setting.valuePtr);
          if (value < setting.enumValues.size()) valueText = I18N.get(setting.enumValues[value]);
        } else if (setting.type == SettingType::ENUM && setting.valueGetter) {
          const uint8_t value = setting.valueGetter();
          if (!setting.enumStringValues.empty() && value < setting.enumStringValues.size()) {
            valueText = setting.enumStringValues[value];
          } else if (value < setting.enumValues.size()) {
            valueText = I18N.get(setting.enumValues[value]);
          }
        } else if (setting.type == SettingType::VALUE && setting.valuePtr != nullptr) {
          if (setting.nameId == StrId::STR_TIME_TO_SLEEP) {
            if (SETTINGS.sleepTimeoutMinutes >= CrossPointSettings::SLEEP_TIMEOUT_NEVER_MINUTES) {
              valueText = tr(STR_SLEEP_NEVER);
            } else {
              char valueBuffer[32];
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
          valueText = ">";
        }
        return valueText;
      },
      true, nullptr, nullptr, [&settings](int i) { return settings[i].type == SettingType::HEADER; });

  const char* confirmLabel = tr(STR_SELECT);
  if (selectedSettingIndex == 0) {
    confirmLabel = I18N.get(categoryNames[(selectedCategoryIndex + 1) % categoryCount]);
  } else if (selectedSettingIndex > 0) {
    const auto& setting = (*currentSettings)[selectedSettingIndex - 1];
    confirmLabel = (setting.type == SettingType::TOGGLE) ? tr(STR_TOGGLE) : tr(STR_SELECT);
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (systemTab) {
    const int bandTop = listTop + listHeight;
    const int bandBottom = pageHeight - BaseTheme::frontButtonFooterLayoutH(renderer);
    const int textH = renderer.getLineHeight(SMALL_FONT_ID);
    const int textY = bandTop + std::max(0, (bandBottom - bandTop - textH) / 2);
#ifndef CROSSPOINT_VERSION
#define CROSSPOINT_VERSION "dev"
#endif
    renderer.drawCenteredText(SMALL_FONT_ID, textY, CROSSPOINT_VERSION, true);
  }

  // Open: displaySoftOpen (X3 FAST+settle, X4 HALF). Up/Down: plain FAST.
  if (softOpenPending_) {
    softOpenPending_ = false;
    UiGhostPolicy::displaySoftOpen(renderer, /*softCount=*/1);
  } else {
    UiGhostPolicy::displayMenuFrame(renderer);
  }
}
