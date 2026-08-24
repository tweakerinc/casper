#pragma once

#include <BoardConfig.h>
#include <HalClock.h>
#include <HalGPIO.h>
#include <HalTiltSensor.h>
#include <I18n.h>
#include <SdCardFontRegistry.h>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <vector>

#include "CrossPointSettings.h"
#include "KOReaderCredentialStore.h"
#include "activities/settings/SettingsActivity.h"
#include "util/DictionaryRegistry.h"

// UI Theme picker. Stats (FocusTheme) is disabled / not in the binary.
// Bare · Penumbra on both X3 and X4 (Penumbra layout differs by hardware).
inline SettingInfo buildUiThemeSetting() {
  using T = CrossPointSettings::UI_THEME;

  std::vector<StrId> labels;
  labels.reserve(2);
  labels.push_back(StrId::STR_THEME_BARE);
  labels.push_back(StrId::STR_THEME_PENUMBRA);

  auto applyThemeDefaults = [](T theme) {
    if (theme == T::PENUMBRA) {
      // Clean top by default — battery/clock off; Battery Warning in Middle.
      // X3 clock / X4 progress live on the home face, never on the system bar.
      SETTINGS.systemStatusBarLeft = CrossPointSettings::SYS_SLOT_HIDE;
      SETTINGS.systemStatusBarMiddle = CrossPointSettings::SYS_SLOT_BATTERY_WARNING;
      SETTINGS.systemStatusBarRight = CrossPointSettings::SYS_SLOT_HIDE;
      SETTINGS.stripSystemStatusBarClock();
      SETTINGS.syncSystemStatusLegacyFromSlots();
      SETTINGS.batteryWarning = CrossPointSettings::BATTERY_WARNING_15;
    } else {
      // Bare (and any remapped legacy theme).
      SETTINGS.systemStatusBarLeft = CrossPointSettings::SYS_SLOT_HIDE;
      SETTINGS.systemStatusBarMiddle = CrossPointSettings::SYS_SLOT_BATTERY_WARNING;
      SETTINGS.systemStatusBarRight = CrossPointSettings::SYS_SLOT_HIDE;
      SETTINGS.syncSystemStatusLegacyFromSlots();
      SETTINGS.batteryWarning = CrossPointSettings::BATTERY_WARNING_15;
    }
  };

  return SettingInfo::DynamicEnum(
      StrId::STR_UI_THEME, std::move(labels),
      [] {
        const auto t = static_cast<T>(SETTINGS.uiTheme);
        if (t == T::PENUMBRA) return static_cast<uint8_t>(1);
        return static_cast<uint8_t>(0);  // Bare
      },
      [applyThemeDefaults](uint8_t displayIdx) {
        const T theme = (displayIdx == 1) ? T::PENUMBRA : T::BARE;
        SETTINGS.uiTheme = static_cast<uint8_t>(theme);
        applyThemeDefaults(theme);
      },
      "uiTheme", StrId::STR_CAT_DISPLAY);
}

// Sleep Screen picker — wallpaper styles only. Quick Resume is a power / timeout
// action, not a sleep-screen value. Stored enum stays append-only (incl. legacy QR=6).
// UI order: CrossPoint Dark, CrossPoint Light, Cover, Cover + Custom, Custom, None.
inline SettingInfo buildSleepScreenSetting() {
  using M = CrossPointSettings::SLEEP_SCREEN_MODE;
  static constexpr M kOrder[] = {
      M::DARK, M::LIGHT, M::COVER, M::COVER_CUSTOM, M::CUSTOM, M::BLANK,
  };
  static constexpr StrId kLabels[] = {
      StrId::STR_DARK,         StrId::STR_LIGHT,  StrId::STR_COVER,
      StrId::STR_COVER_CUSTOM, StrId::STR_CUSTOM, StrId::STR_NONE_OPT,
  };
  static constexpr uint8_t kCount = static_cast<uint8_t>(sizeof(kOrder) / sizeof(kOrder[0]));

  std::vector<StrId> labels;
  labels.reserve(kCount);
  for (uint8_t i = 0; i < kCount; ++i) {
    labels.push_back(kLabels[i]);
  }

  return SettingInfo::DynamicEnum(
             StrId::STR_SLEEP_SCREEN, std::move(labels),
             [] {
               const auto mode = static_cast<M>(SETTINGS.sleepScreen);
               // Legacy QUICK_RESUME (6) displays as Light until next explicit pick.
               if (mode == M::QUICK_RESUME) {
                 for (uint8_t i = 0; i < kCount; ++i) {
                   if (kOrder[i] == M::LIGHT) return i;
                 }
               }
               for (uint8_t i = 0; i < kCount; ++i) {
                 if (kOrder[i] == mode) return i;
               }
               return static_cast<uint8_t>(1);  // Light
             },
             [](uint8_t displayIdx) {
               if (displayIdx >= kCount) displayIdx = 1;
               SETTINGS.sleepScreen = static_cast<uint8_t>(kOrder[displayIdx]);
             },
             "sleepScreen", StrId::STR_CAT_DISPLAY)
      .withNestedUnderParent();  // under Quick Resume on Timeout when that row is Off
}

// Shared Reader Controls shortcut picker (side long-press + Confirm long/double).
// Storage is LONG_PRESS_MENU_FUNCTION (append-only). Display order is product priority.
inline SettingInfo buildReaderShortcutSetting(StrId nameId, uint8_t CrossPointSettings::* field, const char* key,
                                              uint8_t fallbackDisplayIdx = 0) {
  using A = CrossPointSettings::LONG_PRESS_MENU_FUNCTION;
  static constexpr A kOrder[] = {
      A::LP_MENU_DISABLED,      A::LP_MENU_DICTIONARY,         A::LP_MENU_BOOKMARK,         A::LP_MENU_SCREENSHOT,
      A::LP_MENU_FOOTNOTES,     A::LP_MENU_CLIPPINGS,          A::LP_MENU_KOSYNC,           A::LP_MENU_SLEEP,
      A::LP_MENU_FORCE_REFRESH, A::LP_MENU_FILE_BROWSER,       A::LP_MENU_FILE_TRANSFER,    A::LP_MENU_READING_STATS,
      A::LP_MENU_CHAPTER_SKIP,  A::LP_MENU_ORIENTATION_CHANGE, A::LP_MENU_ORIENTATION_FLIP, A::LP_MENU_DARK_MODE,
  };
  static constexpr StrId kLabels[] = {
      StrId::STR_LONG_PRESS_BEHAVIOR_OFF,
      StrId::STR_DICTIONARY,
      StrId::STR_BOOKMARK_OPTION,
      StrId::STR_SCREENSHOT_BUTTON,
      StrId::STR_FOOTNOTES,
      StrId::STR_CLIPPING_TOOL,
      StrId::STR_KOSYNC,
      StrId::STR_SLEEP,
      StrId::STR_FORCE_REFRESH,
      StrId::STR_BROWSE_FILES,
      StrId::STR_FILE_TRANSFER,
      StrId::STR_READING_STATS,
      StrId::STR_LONG_PRESS_BEHAVIOR_SKIP,
      StrId::STR_LONG_PRESS_BEHAVIOR_ORIENTATION,
      StrId::STR_LONG_PRESS_BEHAVIOR_FLIP,
      StrId::STR_READER_DARK_MODE,
  };
  static constexpr uint8_t kCount = static_cast<uint8_t>(sizeof(kOrder) / sizeof(kOrder[0]));
  static_assert(kCount == CrossPointSettings::LONG_PRESS_MENU_FUNCTION_COUNT);
  static_assert(sizeof(kLabels) / sizeof(kLabels[0]) == kCount);

  std::vector<StrId> labels;
  labels.reserve(kCount);
  for (uint8_t i = 0; i < kCount; ++i) labels.push_back(kLabels[i]);

  return SettingInfo::DynamicEnum(
      nameId, std::move(labels),
      [field, fallbackDisplayIdx] {
        const auto mode = static_cast<A>(SETTINGS.*field);
        for (uint8_t i = 0; i < kCount; ++i) {
          if (kOrder[i] == mode) return i;
        }
        return fallbackDisplayIdx < kCount ? fallbackDisplayIdx : static_cast<uint8_t>(0);
      },
      [field, fallbackDisplayIdx](uint8_t displayIdx) {
        if (displayIdx >= kCount) displayIdx = fallbackDisplayIdx < kCount ? fallbackDisplayIdx : 0;
        SETTINGS.*field = static_cast<uint8_t>(kOrder[displayIdx]);
      },
      key, StrId::STR_CAT_READER);
}

// Nested under Flip Orientation (either side long-press): Portrait ↔ this orientation.
inline SettingInfo buildOrientationFlipWithSetting() {
  using O = CrossPointSettings::ORIENTATION;
  static constexpr O kOrder[] = {O::LANDSCAPE_CW, O::INVERTED, O::LANDSCAPE_CCW};
  static constexpr StrId kLabels[] = {StrId::STR_LANDSCAPE_CW, StrId::STR_ORIENTATION_INVERTED,
                                      StrId::STR_LANDSCAPE_CCW};
  static constexpr uint8_t kCount = static_cast<uint8_t>(sizeof(kOrder) / sizeof(kOrder[0]));

  std::vector<StrId> labels;
  labels.reserve(kCount);
  for (uint8_t i = 0; i < kCount; ++i) labels.push_back(kLabels[i]);

  return SettingInfo::DynamicEnum(
             StrId::STR_ORIENTATION_FLIP_WITH, std::move(labels),
             [] {
               const auto v = static_cast<O>(SETTINGS.orientationFlipWith);
               for (uint8_t i = 0; i < kCount; ++i) {
                 if (kOrder[i] == v) return i;
               }
               for (uint8_t i = 0; i < kCount; ++i) {
                 if (kOrder[i] == O::LANDSCAPE_CCW) return i;
               }
               return static_cast<uint8_t>(0);
             },
             [](uint8_t displayIdx) {
               if (displayIdx >= kCount) displayIdx = 0;
               SETTINGS.orientationFlipWith = static_cast<uint8_t>(kOrder[displayIdx]);
             },
             "orientationFlipWith", StrId::STR_CAT_READER)
      .withNestedUnderParent();
}

// Legacy name kept for call sites that still build Confirm long/double (now aliases).
inline SettingInfo buildLongPressActionSetting(StrId nameId, uint8_t CrossPointSettings::* field, const char* key,
                                               uint8_t fallbackDisplayIdx = 0,
                                               StrId /*category*/ = StrId::STR_CAT_READER) {
  return buildReaderShortcutSetting(nameId, field, key, fallbackDisplayIdx);
}

// Short / Long power-button action picker.
// Storage enum SHORT_PWRBTN is append-only (settings.json indices stay valid).
// Display order (product): Off, Sleep, Quick Resume, Refresh Screen, Page Turn, Footnotes.
inline SettingInfo buildPwrBtnSetting(StrId nameId, uint8_t CrossPointSettings::* field, const char* key) {
  using A = CrossPointSettings::SHORT_PWRBTN;
  static constexpr A kOrder[] = {
      A::IGNORE, A::SLEEP, A::PWR_QUICK_RESUME, A::FORCE_REFRESH, A::PAGE_TURN, A::FOOTNOTES,
  };
  static constexpr StrId kLabels[] = {
      // IGNORE storage value; caption unified to "Off" with other control pickers.
      StrId::STR_LONG_PRESS_BEHAVIOR_OFF, StrId::STR_SLEEP,     StrId::STR_QUICK_RESUME,
      StrId::STR_FORCE_REFRESH,           StrId::STR_PAGE_TURN, StrId::STR_FOOTNOTES,
  };
  static constexpr uint8_t kCount = static_cast<uint8_t>(sizeof(kOrder) / sizeof(kOrder[0]));
  static_assert(kCount == CrossPointSettings::SHORT_PWRBTN_COUNT);
  static_assert(sizeof(kLabels) / sizeof(kLabels[0]) == kCount);

  std::vector<StrId> labels;
  labels.reserve(kCount);
  for (uint8_t i = 0; i < kCount; ++i) labels.push_back(kLabels[i]);

  return SettingInfo::DynamicEnum(
      nameId, std::move(labels),
      [field] {
        const auto mode = static_cast<A>(SETTINGS.*field);
        for (uint8_t i = 0; i < kCount; ++i) {
          if (kOrder[i] == mode) return i;
        }
        return static_cast<uint8_t>(0);  // Ignore
      },
      [field](uint8_t displayIdx) {
        if (displayIdx >= kCount) displayIdx = 0;
        SETTINGS.*field = static_cast<uint8_t>(kOrder[displayIdx]);
      },
      key, StrId::STR_CAT_CONTROLS);
}

// Build the font family setting dynamically. When registry is non-null, SD card fonts
// are appended after the built-in fonts. Otherwise only built-in fonts are listed.
inline SettingInfo buildFontFamilySetting(const SdCardFontRegistry* registry) {
  // Built-in font labels — order matches FONT_FAMILY enum.
  // Brand names are fixed product labels (not translated).
  std::vector<StrId> enumValues;  // unused when allStringValues is filled; keep for ENUM type
  // Runtime string labels for SD card fonts
  std::vector<std::string> enumStringValues;

  // Reserve: first CrossPointSettings::BUILTIN_FONT_COUNT entries use StrId, rest use strings
  if (registry) {
    const auto& families = registry->getFamilies();
    enumStringValues.reserve(families.size());
    std::transform(families.begin(), families.end(), std::back_inserter(enumStringValues),
                   [](const SdCardFontFamilyInfo& f) { return f.name; });
  }

  // Capture the SD font count for the lambdas
  const int sdFontCount = static_cast<int>(enumStringValues.size());

  // Always expose string labels so the menu never depends on i18n keys for brand names.
  // (enumStringValues is preferred over enumValues by the settings renderer when non-empty.)
  std::vector<std::string> allStringValues;
  allStringValues.push_back("Sourcerer");
  allStringValues.push_back("Literata");
  if (sdFontCount > 0) {
    allStringValues.insert(allStringValues.end(), enumStringValues.begin(), enumStringValues.end());
  }

  SettingInfo s;
  s.nameId = StrId::STR_FONT_FAMILY;
  s.type = SettingType::ENUM;
  s.enumValues = std::move(enumValues);
  s.enumStringValues = std::move(allStringValues);
  s.key = "fontFamily";
  s.category = StrId::STR_CAT_READER;
  s.inTextSettings = true;  // matches the static font-family entry it replaces

  // Capture registry families by copy for the lambdas
  std::vector<std::string> sdFamilyNames;
  if (registry) {
    const auto& families = registry->getFamilies();
    sdFamilyNames.reserve(families.size());
    std::transform(families.begin(), families.end(), std::back_inserter(sdFamilyNames),
                   [](const SdCardFontFamilyInfo& f) { return f.name; });
  }

  s.valueGetter = [sdFamilyNames]() -> uint8_t {
    // If an SD card font is selected, find its index
    if (SETTINGS.sdFontFamilyName[0] != '\0') {
      for (int i = 0; i < static_cast<int>(sdFamilyNames.size()); i++) {
        if (sdFamilyNames[i] == SETTINGS.sdFontFamilyName) {
          return static_cast<uint8_t>(CrossPointSettings::BUILTIN_FONT_COUNT + i);
        }
      }
      // SD font name not found in registry — fall through to built-in
    }
    return SETTINGS.fontFamily < CrossPointSettings::BUILTIN_FONT_COUNT ? SETTINGS.fontFamily : 0;
  };

  s.valueSetter = [sdFamilyNames](uint8_t v) {
    if (v < CrossPointSettings::BUILTIN_FONT_COUNT) {
      SETTINGS.fontFamily = v;
      SETTINGS.sdFontFamilyName[0] = '\0';
    } else {
      int sdIdx = v - CrossPointSettings::BUILTIN_FONT_COUNT;
      if (sdIdx < static_cast<int>(sdFamilyNames.size())) {
        strncpy(SETTINGS.sdFontFamilyName, sdFamilyNames[sdIdx].c_str(), sizeof(SETTINGS.sdFontFamilyName) - 1);
        SETTINGS.sdFontFamilyName[sizeof(SETTINGS.sdFontFamilyName) - 1] = '\0';
      }
    }
  };

  return s;
}

// Build the dictionary selection setting dynamically from the folders discovered
// under /dictionaries. "None" plus one option per dictionary; the selected folder
// name persists in SETTINGS.dictionaryName (saved/loaded manually in
// CrossPointSettings::toJson/fromJson — the generic loop skips dynamic entries).
inline SettingInfo buildDictionarySetting(const std::vector<DictionaryEntry>& dictionaries) {
  std::vector<std::string> folderNames;
  folderNames.reserve(dictionaries.size());
  std::transform(dictionaries.begin(), dictionaries.end(), std::back_inserter(folderNames),
                 [](const DictionaryEntry& d) { return d.name; });

  SettingInfo s;
  s.nameId = StrId::STR_DICTIONARY;
  s.type = SettingType::ENUM;
  s.enumStringValues.reserve(folderNames.size() + 1);
  s.enumStringValues.push_back(I18N.get(StrId::STR_NONE_OPT));
  s.enumStringValues.insert(s.enumStringValues.end(), folderNames.begin(), folderNames.end());
  s.category = StrId::STR_CAT_READER;

  s.valueGetter = [folderNames]() -> uint8_t {
    for (size_t i = 0; i < folderNames.size(); i++) {
      // Compare within the settings field capacity: an over-long folder name is
      // stored truncated, and must still match its list entry.
      if (strncmp(folderNames[i].c_str(), SETTINGS.dictionaryName, sizeof(SETTINGS.dictionaryName) - 1) == 0) {
        return static_cast<uint8_t>(i + 1);
      }
    }
    return 0;  // "None", also when the stored folder no longer exists
  };

  s.valueSetter = [folderNames](uint8_t v) {
    if (v == 0 || v > folderNames.size()) {
      SETTINGS.dictionaryName[0] = '\0';
      return;
    }
    strncpy(SETTINGS.dictionaryName, folderNames[v - 1].c_str(), sizeof(SETTINGS.dictionaryName) - 1);
    SETTINGS.dictionaryName[sizeof(SETTINGS.dictionaryName) - 1] = '\0';
  };

  return s;
}

// Shared settings list used by both the device settings UI and the web settings API.
// Each entry has a key (for JSON API) and category (for grouping).
// ACTION-type entries and entries without a key are device-only.
//
// The static list is constructed exactly once (master's optimization, #1086 +
// #1636) so the per-entry SettingInfo cost is paid once. When an
// SdCardFontRegistry is supplied AND has SD card fonts installed, the
// font-family entry is replaced in a per-call copy with a registry-aware
// version. Callers without SD fonts pay only a vector copy.
//
// getSettingsListBase(): no copy — for toJson/fromJson under low heap (copying
// SettingInfo vectors with nested enum labels was aborting on bad_alloc).
inline const std::vector<SettingInfo>& getSettingsListBase() {
  static const std::vector<SettingInfo> baseList = [] {
    std::vector<SettingInfo> v = {
        // --- Display (device order: Status Bar → Theme → Sleep → …) ---
        // Status Bar (system top chrome) is an Action row inserted by SettingsActivity.
        // Web/JSON still exposes hideBatteryPercentage + system slots below.
        // Theme (stored enum values stay append-only / stable).
        // Picker: Bare · Penumbra (X3 clock face / X4 title+progress).
        buildUiThemeSetting(),
        // No Penumbra side-button remap rows (X3 L/R cycle panels; X4 U/D scroll recents).
        // Idle timeout sits with sleep chrome: Time to Sleep → Quick Resume on Timeout → Sleep Screen.
        SettingInfo::Value(
            StrId::STR_TIME_TO_SLEEP, &CrossPointSettings::sleepTimeoutMinutes,
            {CrossPointSettings::MIN_SLEEP_TIMEOUT_MINUTES, CrossPointSettings::MAX_SLEEP_TIMEOUT_MINUTES, 1},
            "sleepTimeoutMinutes", StrId::STR_CAT_DISPLAY),
        SettingInfo::Enum(StrId::STR_QUICK_RESUME_TIMEOUT, &CrossPointSettings::quickResumeSleepScreen,
                          {StrId::STR_STATE_OFF, StrId::STR_STATE_ON}, "quickResumeSleepScreen",
                          StrId::STR_CAT_DISPLAY),
        buildSleepScreenSetting(),
        SettingInfo::Enum(StrId::STR_SLEEP_COVER_MODE, &CrossPointSettings::sleepScreenCoverMode,
                          {StrId::STR_FIT, StrId::STR_CROP}, "sleepScreenCoverMode", StrId::STR_CAT_DISPLAY)
            .withNestedUnderParent(),  // under Sleep Screen
        SettingInfo::Enum(StrId::STR_SLEEP_COVER_FILTER, &CrossPointSettings::sleepScreenCoverFilter,
                          {StrId::STR_NONE_OPT, StrId::STR_FILTER_CONTRAST, StrId::STR_INVERTED},
                          "sleepScreenCoverFilter", StrId::STR_CAT_DISPLAY)
            .withNestedUnderParent(),  // under Sleep Screen
        SettingInfo::Toggle(StrId::STR_SUNLIGHT_FADING_FIX, &CrossPointSettings::fadingFix, "fadingFix",
                            StrId::STR_CAT_DISPLAY),
        // Dark Mode: whole UI by default. Nested "Reader Only" scopes invert to books.
        // Both default Off. Nested row is hidden while Dark Mode is Off (SettingsActivity).
        SettingInfo::Toggle(StrId::STR_READER_DARK_MODE, &CrossPointSettings::readerDarkMode, "readerDarkMode",
                            StrId::STR_CAT_DISPLAY),
        SettingInfo::Toggle(StrId::STR_DARK_MODE_READER_ONLY, &CrossPointSettings::darkModeReaderOnly,
                            "darkModeReaderOnly", StrId::STR_CAT_DISPLAY)
            .withNestedUnderParent(),  // under Dark Mode

        // --- Reader ---
        // Built-in font-family entry. Replaced per-call with a registry-aware
        // version when SD fonts are installed.
        SettingInfo::Enum(StrId::STR_FONT_FAMILY, &CrossPointSettings::fontFamily,
                          {StrId::STR_NOTO_SERIF, StrId::STR_NOTO_SANS}, "fontFamily", StrId::STR_CAT_READER)
            .withTextSettings(),
        // Web/JSON still accept full enum 0..5; on-device Size list is 10–16 for
        // builtins and expands for SD packs that include 8/18 (TextSettingsActivity).
        SettingInfo::Enum(StrId::STR_FONT_SIZE, &CrossPointSettings::fontSize,
                          {StrId::STR_STATUS_BAR_FONT_8, StrId::STR_STATUS_BAR_FONT_10, StrId::STR_MENU_FONT_SMALL,
                           StrId::STR_MENU_FONT_MEDIUM, StrId::STR_MENU_FONT_LARGE, StrId::STR_STATUS_BAR_FONT_18},
                          "fontSize", StrId::STR_CAT_READER)
            .withTextSettings(),
        SettingInfo::Enum(StrId::STR_LINE_SPACING, &CrossPointSettings::lineSpacing,
                          {StrId::STR_TIGHT, StrId::STR_NORMAL, StrId::STR_WIDE}, "lineSpacing", StrId::STR_CAT_READER)
            .withTextSettings(),
        SettingInfo::Value(StrId::STR_SCREEN_MARGIN, &CrossPointSettings::screenMargin,
                           {CrossPointSettings::SCREEN_MARGIN_MIN, CrossPointSettings::SCREEN_MARGIN_MAX,
                            CrossPointSettings::SCREEN_MARGIN_STEP},
                           "screenMargin", StrId::STR_CAT_READER)
            .withTextSettings(),
        // Alignment picker order is Book's Style first in Manage Fonts; storage enum
        // order stays JUSTIFY…BOOK_STYLE. Embedded Style is not listed — Book's Style
        // turns embedded CSS on; any forced alignment turns it off.
        SettingInfo::Enum(StrId::STR_PARA_ALIGNMENT, &CrossPointSettings::paragraphAlignment,
                          {StrId::STR_JUSTIFY, StrId::STR_ALIGN_LEFT, StrId::STR_CENTER, StrId::STR_ALIGN_RIGHT,
                           StrId::STR_BOOK_S_STYLE},
                          "paragraphAlignment", StrId::STR_CAT_READER)
            .withTextSettings(),
        // legacy naming: Bionic Reading (bold prefixes) + Guide Dots (· between words).
        // JSON key focusReadingEnabled kept for older settings files.
        SettingInfo::Toggle(StrId::STR_BIONIC_READING, &CrossPointSettings::focusReadingEnabled, "focusReadingEnabled",
                            StrId::STR_CAT_READER)
            .withTextSettings(),
        SettingInfo::Toggle(StrId::STR_GUIDE_READING, &CrossPointSettings::guideReadingEnabled, "guideReadingEnabled",
                            StrId::STR_CAT_READER)
            .withTextSettings(),
        SettingInfo::Toggle(StrId::STR_HYPHENATION, &CrossPointSettings::hyphenationEnabled, "hyphenationEnabled",
                            StrId::STR_CAT_READER)
            .withTextSettings(),
        SettingInfo::Enum(
            StrId::STR_ORIENTATION, &CrossPointSettings::orientation,
            {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW, StrId::STR_ORIENTATION_INVERTED, StrId::STR_LANDSCAPE_CCW},
            "orientation", StrId::STR_CAT_READER),
        // Nested under Reading Orientation: nav keys follow rotated reader layout.
        SettingInfo::Toggle(StrId::STR_FRONT_BTN_FOLLOW_ORIENTATION, &CrossPointSettings::frontButtonFollowOrientation,
                            "frontButtonFollowOrientation", StrId::STR_CAT_READER)
            .withNestedUnderParent(),
        SettingInfo::Toggle(StrId::STR_EXTRA_SPACING, &CrossPointSettings::extraParagraphSpacing,
                            "extraParagraphSpacing", StrId::STR_CAT_READER)
            .withTextSettings(),
        // Nested under Extra Paragraph Spacing (Manage Fonts → Layout).
        SettingInfo::Enum(StrId::STR_SPACING_HEIGHT, &CrossPointSettings::extraParagraphSpacingHeight,
                          {StrId::STR_HALF, StrId::STR_FULL, StrId::STR_QUARTER}, "extraParagraphSpacingHeight",
                          StrId::STR_CAT_READER)
            .withNestedUnderParent()
            .withTextSettings(),
        SettingInfo::Toggle(StrId::STR_TEXT_AA, &CrossPointSettings::textAntiAliasing, "textAntiAliasing",
                            StrId::STR_CAT_READER)
            .withTextSettings(),
        SettingInfo::Enum(StrId::STR_IMAGES, &CrossPointSettings::imageRendering,
                          {StrId::STR_IMAGES_DISPLAY, StrId::STR_IMAGES_PLACEHOLDER, StrId::STR_IMAGES_SUPPRESS},
                          "imageRendering", StrId::STR_CAT_READER),
        // Page-turn anti-ghosting (HALF scrub interval) — reader only, not home/menus.
        SettingInfo::Enum(StrId::STR_REFRESH_FREQ, &CrossPointSettings::refreshFrequency,
                          {StrId::STR_PAGES_1, StrId::STR_PAGES_5, StrId::STR_PAGES_10, StrId::STR_PAGES_15,
                           StrId::STR_PAGES_30, StrId::STR_PAGES_60, StrId::STR_NEVER},
                          "refreshFrequency", StrId::STR_CAT_READER),
        // Reader Controls: section chrome (same face/rules as Settings / Reader).
        SettingInfo::Header(StrId::STR_READER_CONTROLS_HEADING, StrId::STR_CAT_READER),
        buildReaderShortcutSetting(StrId::STR_LONG_PRESS_SIDE_A_X3, &CrossPointSettings::longPressSideA,
                                   "longPressSideA"),
        buildReaderShortcutSetting(StrId::STR_LONG_PRESS_SIDE_B_X3, &CrossPointSettings::longPressSideB,
                                   "longPressSideB"),
        buildReaderShortcutSetting(StrId::STR_LONG_PRESS_SIDE_A_X4, &CrossPointSettings::longPressSideA,
                                   "longPressSideA"),
        buildReaderShortcutSetting(StrId::STR_LONG_PRESS_SIDE_B_X4, &CrossPointSettings::longPressSideB,
                                   "longPressSideB"),
        buildOrientationFlipWithSetting(),
        SettingInfo::Enum(StrId::STR_SIDE_BTN_LAYOUT, &CrossPointSettings::sideButtonLayout,
                          {StrId::STR_PREV_NEXT, StrId::STR_NEXT_PREV, StrId::STR_DISABLED}, "sideButtonLayout",
                          StrId::STR_CAT_READER),
        buildReaderShortcutSetting(StrId::STR_LONG_PRESS_MENU, &CrossPointSettings::longPressMenuFunction,
                                   "longPressMenuFunction", /*fallbackDisplayIdx=*/1),
        buildReaderShortcutSetting(StrId::STR_DOUBLE_PRESS_MENU, &CrossPointSettings::doublePressMenuFunction,
                                   "doublePressMenuFunction", /*fallbackDisplayIdx=*/0),
        // Library / Recents / Settings list chrome (not reader body, not Penumbra home panel).
        SettingInfo::Enum(StrId::STR_MENU_FONT_SIZE, &CrossPointSettings::menuFontSize,
                          {StrId::STR_MENU_FONT_XSMALL, StrId::STR_MENU_FONT_SMALL, StrId::STR_MENU_FONT_MEDIUM,
                           StrId::STR_MENU_FONT_LARGE},
                          "menuFontSize", StrId::STR_CAT_DISPLAY),
        SettingInfo::Toggle(StrId::STR_SPLIT_BOOK_TITLE_LINES, &CrossPointSettings::splitBookTitleLines,
                            "splitBookTitleLines", StrId::STR_CAT_DISPLAY)
            .withNestedUnderParent(),  // under Menu Font Size
        // --- Controls (order matches product UI) ---
        // Display order via DynamicEnum; stored SHORT_PWRBTN values unchanged.
        buildPwrBtnSetting(StrId::STR_SHORT_PWR_BTN, &CrossPointSettings::shortPwrBtn, "shortPwrBtn"),
        buildPwrBtnSetting(StrId::STR_LONG_PRESS_ACTION, &CrossPointSettings::longPwrBtn, "longPwrBtn"),
        // Remap Front Buttons is inserted by SettingsActivity after Long-Press Power (no-touch).
        // Tilt (if IMU) is inserted after Remap by getSettingsList below.
        SettingInfo::Enum(StrId::STR_TOUCH_READER_CONTROLS, &CrossPointSettings::touchReaderControls,
                          {StrId::STR_STATE_OFF, StrId::STR_STATE_ON}, "touchReaderControls", StrId::STR_CAT_CONTROLS),
        // Shown only when short or long power is Footnotes (filtered in SettingsActivity).
        SettingInfo::Toggle(StrId::STR_PWR_BTN_FOOTNOTE_BACK, &CrossPointSettings::pwrBtnFootnoteBack,
                            "pwrBtnFootnoteBack", StrId::STR_CAT_CONTROLS)
            .withNestedUnderParent(),

        // --- System ---
        // On-device order is rebuilt in SettingsActivity (Network → … → Language → Hidden → Logging → …).
        // Time to Sleep lives under Display (above Quick Resume on Timeout).
        // Parent of Clear Read from Recents (nested + shown only when this is On).
        // Moves finished EPUBs into hidden /read (browse via Recents → Show Read Books).
        SettingInfo::Toggle(StrId::STR_MOVE_FINISHED_TO_READ, &CrossPointSettings::moveFinishedToReadFolder,
                            "moveFinishedToReadFolder", StrId::STR_CAT_SYSTEM),
        SettingInfo::Toggle(StrId::STR_REMOVE_READ_FROM_RECENTS, &CrossPointSettings::removeReadBooksFromRecents,
                            "removeReadBooksFromRecents", StrId::STR_CAT_SYSTEM)
            .withNestedUnderParent(),  // under Move Finished Books
        SettingInfo::Toggle(StrId::STR_SHOW_HIDDEN_FILES, &CrossPointSettings::showHiddenFiles, "showHiddenFiles",
                            StrId::STR_CAT_SYSTEM),
        // Enable Logging (Off / On). On = Timing. DynamicEnum (not Toggle) because the
        // stored field is multi-level; UI still one-click toggles like other switches.
        SettingInfo::DynamicEnum(
            StrId::STR_ENABLE_LOGGING, {StrId::STR_STATE_OFF, StrId::STR_STATE_ON},
            [] { return static_cast<uint8_t>(SETTINGS.systemLogLevel != CrossPointSettings::SYSTEM_LOG_OFF ? 1 : 0); },
            [](uint8_t on) {
              SETTINGS.systemLogLevel = on ? static_cast<uint8_t>(CrossPointSettings::SYSTEM_LOG_TIMING)
                                           : static_cast<uint8_t>(CrossPointSettings::SYSTEM_LOG_OFF);
            },
            "systemLogLevel", StrId::STR_CAT_SYSTEM),
        // Session idle gap for stats (device UI builds ordered System list separately).
        SettingInfo::Value(
            StrId::STR_SESSION_TIME, &CrossPointSettings::readingSessionIdleMinutes,
            {CrossPointSettings::MIN_SESSION_IDLE_MINUTES, CrossPointSettings::MAX_SESSION_IDLE_MINUTES, 1},
            "readingSessionIdleMinutes", StrId::STR_CAT_SYSTEM),
        // Stats folder settings (web/JSON). On-device UI uses StatsSettingsActivity.
        SettingInfo::Toggle(StrId::STR_ENABLE_STAT_TRACKING, &CrossPointSettings::readingStatsEnabled,
                            "readingStatsEnabled", StrId::STR_STATS),
        SettingInfo::Toggle(StrId::STR_AUTO_BACKUP_STATS, &CrossPointSettings::autoBackupStats, "autoBackupStats",
                            StrId::STR_STATS),

        // OPDS download folder: persisted + web-exposed, but category-less so it
        // is hidden from the on-device Settings screen (edited via OPDS UI).
        SettingInfo::String(StrId::STR_OPDS_DOWNLOAD_FOLDER, &SETTINGS.opdsDownloadFolder[0],
                            sizeof(SETTINGS.opdsDownloadFolder), "opdsDownloadFolder"),
        // OPDS download filename format: persisted + web-exposed, category-less so it
        // is hidden from the on-device Settings screen (cycled from the OPDS UI).
        SettingInfo::Enum(StrId::STR_OPDS_FILENAME_FORMAT, &CrossPointSettings::opdsFilenameFormat,
                          {StrId::STR_FMT_AUTHOR_TITLE, StrId::STR_FMT_TITLE_AUTHOR, StrId::STR_FMT_TITLE},
                          "opdsFilenameFormat"),

        // --- KOReader Sync (web-only, uses KOReaderCredentialStore) ---
        SettingInfo::DynamicString(
            StrId::STR_KOREADER_USERNAME, [] { return KOREADER_STORE.getUsername(); },
            [](const std::string& v) {
              KOREADER_STORE.setCredentials(v, KOREADER_STORE.getPassword());
              KOREADER_STORE.saveToFile();
            },
            "koUsername", StrId::STR_KOREADER_SYNC),
        SettingInfo::DynamicString(
            StrId::STR_KOREADER_PASSWORD, [] { return KOREADER_STORE.getPassword(); },
            [](const std::string& v) {
              KOREADER_STORE.setCredentials(KOREADER_STORE.getUsername(), v);
              KOREADER_STORE.saveToFile();
            },
            "koPassword", StrId::STR_KOREADER_SYNC),
        SettingInfo::DynamicString(
            StrId::STR_SYNC_SERVER_URL, [] { return KOREADER_STORE.getServerUrl(); },
            [](const std::string& v) {
              KOREADER_STORE.setServerUrl(v);
              KOREADER_STORE.saveToFile();
            },
            "koServerUrl", StrId::STR_KOREADER_SYNC),
        SettingInfo::DynamicEnum(
            StrId::STR_DOCUMENT_MATCHING, {StrId::STR_FILENAME, StrId::STR_BINARY},
            [] { return static_cast<uint8_t>(KOREADER_STORE.getMatchMethod()); },
            [](uint8_t v) {
              KOREADER_STORE.setMatchMethod(static_cast<DocumentMatchMethod>(v));
              KOREADER_STORE.saveToFile();
            },
            "koMatchMethod", StrId::STR_KOREADER_SYNC),
        // Display order: Off, Smart Sync, Ask Every Time, Percent, Time (maps via syncBehaviorTo/FromDisplay).
        SettingInfo::DynamicEnum(
            StrId::STR_SYNC_BEHAVIOR,
            {StrId::STR_OFF, StrId::STR_SMART_SYNC, StrId::STR_ASK_EVERY_TIME, StrId::STR_PERCENT, StrId::STR_TIME},
            [] { return syncBehaviorToDisplay(KOREADER_STORE.getSyncBehavior()); },
            [](uint8_t v) {
              KOREADER_STORE.setSyncBehavior(syncBehaviorFromDisplay(v));
              KOREADER_STORE.saveToFile();
            },
            "koSyncBehavior", StrId::STR_KOREADER_SYNC),
        SettingInfo::DynamicEnum(
            StrId::STR_SEND_METADATA, {StrId::STR_STATE_OFF, StrId::STR_STATE_ON},
            [] { return static_cast<uint8_t>(KOREADER_STORE.getSendMetadata()); },
            [](uint8_t v) {
              KOREADER_STORE.setSendMetadata(v != 0);
              KOREADER_STORE.saveToFile();
            },
            "koSendMetadata", StrId::STR_KOREADER_SYNC),
        // Legacy keys kept for web/JSON; device UI drives these via Sync Behavior.
        SettingInfo::DynamicEnum(
            StrId::STR_AUTO_UPLOAD_ON_CLOSE, {StrId::STR_NO, StrId::STR_YES},
            [] { return static_cast<uint8_t>(KOREADER_STORE.getAutoUploadOnClose() ? 1 : 0); },
            [](uint8_t v) {
              KOREADER_STORE.setAutoUploadOnClose(v != 0);
              KOREADER_STORE.saveToFile();
            },
            "koAutoUploadOnClose", StrId::STR_KOREADER_SYNC),
        SettingInfo::DynamicEnum(
            StrId::STR_UPLOAD_TYPE, {StrId::STR_TIME, StrId::STR_PERCENT, StrId::STR_ADAPTIVE},
            [] { return static_cast<uint8_t>(KOREADER_STORE.getAutoUploadType()); },
            [](uint8_t v) {
              KOREADER_STORE.setAutoUploadType(static_cast<AutoUploadType>(v));
              KOREADER_STORE.saveToFile();
            },
            "koAutoUploadType", StrId::STR_KOREADER_SYNC),
        // --- Status Bar Settings (web-only, uses StatusBarSettingsActivity) ---
        // Six exclusive slots. Labels must match STATUS_BAR_CORNER_CONTENT enum index order
        // (on-device popup reorders for UX; web uses this index order).
        SettingInfo::Enum(
            StrId::STR_UPPER_LEFT, &CrossPointSettings::statusBarUpperLeft,
            {StrId::STR_HIDE, StrId::STR_BATTERY, StrId::STR_CHAPTER_PAGE_COUNTER, StrId::STR_PROGRESS_PERCENTAGE,
             StrId::STR_TIME_LEFT_BOOK_OPTION, StrId::STR_TIME_LEFT_CHAPTER_OPTION, StrId::STR_CLOCK,
             StrId::STR_BOOK_TITLE, StrId::STR_BOOK_PAGE_COUNTER, StrId::STR_CHAPTER_COUNTER, StrId::STR_CHAPTER_TITLE,
             StrId::STR_XTC_STATUS_BAR},
            "statusBarUpperLeft", StrId::STR_CUSTOMISE_STATUS_BAR),
        SettingInfo::Enum(
            StrId::STR_UPPER_MIDDLE, &CrossPointSettings::statusBarUpperMiddle,
            {StrId::STR_HIDE, StrId::STR_BATTERY, StrId::STR_CHAPTER_PAGE_COUNTER, StrId::STR_PROGRESS_PERCENTAGE,
             StrId::STR_TIME_LEFT_BOOK_OPTION, StrId::STR_TIME_LEFT_CHAPTER_OPTION, StrId::STR_CLOCK,
             StrId::STR_BOOK_TITLE, StrId::STR_BOOK_PAGE_COUNTER, StrId::STR_CHAPTER_COUNTER, StrId::STR_CHAPTER_TITLE,
             StrId::STR_XTC_STATUS_BAR},
            "statusBarUpperMiddle", StrId::STR_CUSTOMISE_STATUS_BAR),
        SettingInfo::Enum(
            StrId::STR_UPPER_RIGHT, &CrossPointSettings::statusBarUpperRight,
            {StrId::STR_HIDE, StrId::STR_BATTERY, StrId::STR_CHAPTER_PAGE_COUNTER, StrId::STR_PROGRESS_PERCENTAGE,
             StrId::STR_TIME_LEFT_BOOK_OPTION, StrId::STR_TIME_LEFT_CHAPTER_OPTION, StrId::STR_CLOCK,
             StrId::STR_BOOK_TITLE, StrId::STR_BOOK_PAGE_COUNTER, StrId::STR_CHAPTER_COUNTER, StrId::STR_CHAPTER_TITLE,
             StrId::STR_XTC_STATUS_BAR},
            "statusBarUpperRight", StrId::STR_CUSTOMISE_STATUS_BAR),
        SettingInfo::Enum(
            StrId::STR_LOWER_LEFT, &CrossPointSettings::statusBarLowerLeft,
            {StrId::STR_HIDE, StrId::STR_BATTERY, StrId::STR_CHAPTER_PAGE_COUNTER, StrId::STR_PROGRESS_PERCENTAGE,
             StrId::STR_TIME_LEFT_BOOK_OPTION, StrId::STR_TIME_LEFT_CHAPTER_OPTION, StrId::STR_CLOCK,
             StrId::STR_BOOK_TITLE, StrId::STR_BOOK_PAGE_COUNTER, StrId::STR_CHAPTER_COUNTER, StrId::STR_CHAPTER_TITLE,
             StrId::STR_XTC_STATUS_BAR},
            "statusBarLowerLeft", StrId::STR_CUSTOMISE_STATUS_BAR),
        SettingInfo::Enum(
            StrId::STR_LOWER_MIDDLE, &CrossPointSettings::statusBarLowerMiddle,
            {StrId::STR_HIDE, StrId::STR_BATTERY, StrId::STR_CHAPTER_PAGE_COUNTER, StrId::STR_PROGRESS_PERCENTAGE,
             StrId::STR_TIME_LEFT_BOOK_OPTION, StrId::STR_TIME_LEFT_CHAPTER_OPTION, StrId::STR_CLOCK,
             StrId::STR_BOOK_TITLE, StrId::STR_BOOK_PAGE_COUNTER, StrId::STR_CHAPTER_COUNTER, StrId::STR_CHAPTER_TITLE,
             StrId::STR_XTC_STATUS_BAR},
            "statusBarLowerMiddle", StrId::STR_CUSTOMISE_STATUS_BAR),
        SettingInfo::Enum(
            StrId::STR_LOWER_RIGHT, &CrossPointSettings::statusBarLowerRight,
            {StrId::STR_HIDE, StrId::STR_BATTERY, StrId::STR_CHAPTER_PAGE_COUNTER, StrId::STR_PROGRESS_PERCENTAGE,
             StrId::STR_TIME_LEFT_BOOK_OPTION, StrId::STR_TIME_LEFT_CHAPTER_OPTION, StrId::STR_CLOCK,
             StrId::STR_BOOK_TITLE, StrId::STR_BOOK_PAGE_COUNTER, StrId::STR_CHAPTER_COUNTER, StrId::STR_CHAPTER_TITLE,
             StrId::STR_XTC_STATUS_BAR},
            "statusBarLowerRight", StrId::STR_CUSTOMISE_STATUS_BAR),
        SettingInfo::Enum(StrId::STR_PROGRESS_BAR, &CrossPointSettings::statusBarProgressBar,
                          {StrId::STR_HIDE, StrId::STR_BOOK, StrId::STR_CHAPTER}, "statusBarProgressBar",
                          StrId::STR_CUSTOMISE_STATUS_BAR),
        SettingInfo::Enum(StrId::STR_PROGRESS_BAR_THICKNESS, &CrossPointSettings::statusBarProgressBarThickness,
                          {StrId::STR_PROGRESS_BAR_THIN, StrId::STR_PROGRESS_BAR_MEDIUM, StrId::STR_PROGRESS_BAR_THICK},
                          "statusBarProgressBarThickness", StrId::STR_CUSTOMISE_STATUS_BAR),
        // System top chrome slots (web + JSON). On-device UI uses SystemStatusBarSettingsActivity.
        SettingInfo::Enum(StrId::STR_LEFT, &CrossPointSettings::systemStatusBarLeft,
                          {StrId::STR_HIDE, StrId::STR_BATTERY, StrId::STR_CLOCK, StrId::STR_BATTERY_WARNING},
                          "systemStatusBarLeft", StrId::STR_STATUS_BAR),
        SettingInfo::Enum(StrId::STR_MIDDLE, &CrossPointSettings::systemStatusBarMiddle,
                          {StrId::STR_HIDE, StrId::STR_BATTERY, StrId::STR_CLOCK, StrId::STR_BATTERY_WARNING},
                          "systemStatusBarMiddle", StrId::STR_STATUS_BAR),
        SettingInfo::Enum(StrId::STR_RIGHT, &CrossPointSettings::systemStatusBarRight,
                          {StrId::STR_HIDE, StrId::STR_BATTERY, StrId::STR_CLOCK, StrId::STR_BATTERY_WARNING},
                          "systemStatusBarRight", StrId::STR_STATUS_BAR),
        SettingInfo::Enum(StrId::STR_BATTERY_DISPLAY, &CrossPointSettings::systemBatteryDisplay,
                          {StrId::STR_ICON, StrId::STR_PERCENT, StrId::STR_ICON_PLUS_PERCENT}, "systemBatteryDisplay",
                          StrId::STR_STATUS_BAR),
        // Reader chrome battery display (Customize Reader UI nests this under Battery slots).
        SettingInfo::Enum(StrId::STR_BATTERY_DISPLAY, &CrossPointSettings::readerBatteryDisplay,
                          {StrId::STR_ICON, StrId::STR_PERCENT, StrId::STR_ICON_PLUS_PERCENT}, "readerBatteryDisplay",
                          StrId::STR_CUSTOMISE_STATUS_BAR),
        // Legacy system clock show/hide (derived from slots; web-compatible).
        SettingInfo::DynamicEnum(
            StrId::STR_CLOCK, {StrId::STR_HIDE, StrId::STR_SHOW},
            [] {
              return SETTINGS.systemStatusBarHas(CrossPointSettings::SYS_SLOT_CLOCK) ? static_cast<uint8_t>(1)   // Show
                                                                                     : static_cast<uint8_t>(0);  // Hide
            },
            [](uint8_t displayIdx) {
              if (displayIdx == 1) {
                if (!SETTINGS.systemStatusBarHas(CrossPointSettings::SYS_SLOT_CLOCK)) {
                  SETTINGS.assignSystemStatusBarSlot(SETTINGS.systemStatusBarMiddle,
                                                     CrossPointSettings::SYS_SLOT_CLOCK);
                }
              } else {
                if (SETTINGS.systemStatusBarLeft == CrossPointSettings::SYS_SLOT_CLOCK)
                  SETTINGS.systemStatusBarLeft = CrossPointSettings::SYS_SLOT_HIDE;
                if (SETTINGS.systemStatusBarMiddle == CrossPointSettings::SYS_SLOT_CLOCK)
                  SETTINGS.systemStatusBarMiddle = CrossPointSettings::SYS_SLOT_HIDE;
                if (SETTINGS.systemStatusBarRight == CrossPointSettings::SYS_SLOT_CLOCK)
                  SETTINGS.systemStatusBarRight = CrossPointSettings::SYS_SLOT_HIDE;
                SETTINGS.syncSystemStatusLegacyFromSlots();
              }
            },
            "systemClock", StrId::STR_STATUS_BAR),
        SettingInfo::Value(StrId::STR_CLOCK_UTC_OFFSET, &CrossPointSettings::clockUtcOffsetQ, {0, 104, 1},
                           "clockUtcOffsetQ", StrId::STR_STATUS_BAR),
        SettingInfo::Enum(StrId::STR_CLOCK_FORMAT, &CrossPointSettings::clockFormat,
                          {StrId::STR_CLOCK_FORMAT_24H, StrId::STR_CLOCK_FORMAT_12H}, "clockFormat",
                          StrId::STR_STATUS_BAR),
        // Persistence flag for NTP debounce. Resetting from the web UI forces a re-sync
        // on next WiFi connect, which is useful when crossing time zones.
        SettingInfo::Toggle(StrId::STR_CLOCK_SYNCED, &CrossPointSettings::clockHasBeenSynced, "clockHasBeenSynced",
                            StrId::STR_STATUS_BAR),
    };
    // Only show tilt page turn when the QMI8658 IMU is present (X3).
    // Place after Remap / Long-Press Power in Controls.
    if (halTiltSensor.isAvailable()) {
      for (auto it = v.begin(); it != v.end(); ++it) {
        if (it->nameId == StrId::STR_LONG_PRESS_ACTION || it->nameId == StrId::STR_REMAP_FRONT_BUTTONS) {
          v.insert(it + 1, SettingInfo::Enum(StrId::STR_TILT_PAGE_TURN, &CrossPointSettings::tiltPageTurn,
                                             {StrId::STR_STATE_OFF, StrId::STR_NORMAL, StrId::STR_INVERTED},
                                             "tiltPageTurn", StrId::STR_CAT_CONTROLS));
          break;
        }
      }
    }
    return v;
  }();
  return baseList;
}

inline std::vector<SettingInfo> getSettingsList(const SdCardFontRegistry* registry = nullptr,
                                                const std::vector<DictionaryEntry>* dictionaries = nullptr) {
  std::vector<SettingInfo> v = getSettingsListBase();
  if (!BoardConfig::hasTouch()) {
    v.erase(std::remove_if(v.begin(), v.end(),
                           [](const SettingInfo& s) { return s.nameId == StrId::STR_TOUCH_READER_CONTROLS; }),
            v.end());
  }
  if (BoardConfig::hasTouch()) {
    v.erase(std::remove_if(v.begin(), v.end(),
                           [](const SettingInfo& s) {
                             return s.nameId == StrId::STR_FRONT_BTN_FOLLOW_ORIENTATION ||
                                    s.nameId == StrId::STR_SUNLIGHT_FADING_FIX;
                           }),
            v.end());
  }
  // Always inject string labels ("Sourcerer" / "Literata" + optional SD families)
  // so the menu never depends on STR_NOTO_SERIF i18n text.
  {
    auto it = std::find_if(v.begin(), v.end(), [](const SettingInfo& s) { return s.nameId == StrId::STR_FONT_FAMILY; });
    if (it != v.end()) {
      *it = buildFontFamilySetting(registry);
    }
  }
  // On-device dictionary selection is DictionarySelectActivity (multi-select page).
  // Web API can still expose a single dictionaryName string via manual JSON fields.
  (void)dictionaries;
  return v;
}
