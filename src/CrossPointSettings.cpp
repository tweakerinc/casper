#include "CrossPointSettings.h"

#include <HalGPIO.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <ObfuscationUtils.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>

#include "I18nKeys.h"
#include "SettingsList.h"
#include "fontIds.h"

namespace {

// Stack buffer for "<key>_obf" key construction — avoids a std::string
// allocation per obfuscated setting on every save and load.
constexpr size_t OBF_KEY_BUF = 64;

// Null-terminated copy into a fixed-size settings field.
void copyToField(char* dest, const char* src, const size_t maxLen) {
  strncpy(dest, src, maxLen - 1);
  dest[maxLen - 1] = '\0';
}

}  // namespace

void CrossPointSettings::setDefaultButtonFunctionMap(uint8_t* map, const uint8_t count) {
  if (map == nullptr || count == 0) return;
  const uint8_t defaults[HW_REMAP_BUTTON_COUNT] = {BTN_FUNC_BACK, BTN_FUNC_CONFIRM, BTN_FUNC_UP,
                                                   BTN_FUNC_DOWN, BTN_FUNC_LEFT,    BTN_FUNC_RIGHT};
  for (uint8_t i = 0; i < count && i < HW_REMAP_BUTTON_COUNT; i++) {
    map[i] = defaults[i];
  }
}

bool CrossPointSettings::isButtonFunctionMapValid(const uint8_t* map, const uint8_t count) {
  if (map == nullptr || count == 0) return false;
  bool hasBack = false;
  bool hasConfirm = false;
  bool hasPrev = false;  // Left or Up
  bool hasNext = false;  // Right or Down
  for (uint8_t i = 0; i < count; i++) {
    const uint8_t fn = map[i];
    if (fn >= BTN_FUNC_COUNT) return false;
    switch (fn) {
      case BTN_FUNC_BACK:
        hasBack = true;
        break;
      case BTN_FUNC_CONFIRM:
        hasConfirm = true;
        break;
      case BTN_FUNC_LEFT:
      case BTN_FUNC_UP:
        hasPrev = true;
        break;
      case BTN_FUNC_RIGHT:
      case BTN_FUNC_DOWN:
        hasNext = true;
        break;
      default:
        break;
    }
  }
  return hasBack && hasConfirm && hasPrev && hasNext;
}

void CrossPointSettings::syncLegacyFrontButtonsFromHwMap() {
  auto findHw = [this](const uint8_t func, const uint8_t fallback) -> uint8_t {
    for (uint8_t hw = 0; hw < FRONT_BUTTON_HARDWARE_COUNT; hw++) {
      if (hwButtonFunction[hw] == func) return hw;
    }
    for (uint8_t hw = 0; hw < HW_REMAP_BUTTON_COUNT; hw++) {
      if (hwButtonFunction[hw] == func) return static_cast<uint8_t>(hw % FRONT_BUTTON_HARDWARE_COUNT);
    }
    return fallback;
  };
  frontButtonBack = findHw(BTN_FUNC_BACK, FRONT_HW_BACK);
  frontButtonConfirm = findHw(BTN_FUNC_CONFIRM, FRONT_HW_CONFIRM);
  frontButtonLeft = findHw(BTN_FUNC_LEFT, FRONT_HW_LEFT);
  frontButtonRight = findHw(BTN_FUNC_RIGHT, FRONT_HW_RIGHT);
}

bool CrossPointSettings::saveButtonMapSidecar() const {
  // Tiny text file: "0,1,2,3,2,3\n" — independent of settings.json.
  char line[32];
  int n = 0;
  for (uint8_t i = 0; i < HW_REMAP_BUTTON_COUNT; i++) {
    if (i > 0) {
      if (n < static_cast<int>(sizeof(line)) - 1) line[n++] = ',';
    }
    n += snprintf(line + n, sizeof(line) - static_cast<size_t>(n), "%u",
                  static_cast<unsigned>(hwButtonFunction[i]));
    if (n < 0 || n >= static_cast<int>(sizeof(line))) return false;
  }
  if (n < static_cast<int>(sizeof(line)) - 1) line[n++] = '\n';
  line[n] = '\0';
  Storage.mkdir("/.crosspoint");
  return Storage.writeFile(buttonMapSidecarPath(), String(line));
}

bool CrossPointSettings::loadButtonMapSidecar() {
  if (!Storage.exists(buttonMapSidecarPath())) return false;
  const String raw = Storage.readFile(buttonMapSidecarPath());
  if (raw.isEmpty()) return false;

  uint8_t parsed[HW_REMAP_BUTTON_COUNT];
  setDefaultButtonFunctionMap(parsed);
  uint8_t count = 0;
  const char* p = raw.c_str();
  while (*p && count < HW_REMAP_BUTTON_COUNT) {
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (!*p) break;
    char* end = nullptr;
    const long v = strtol(p, &end, 10);
    if (end == p) break;
    if (v >= 0 && v < static_cast<long>(BTN_FUNC_COUNT)) {
      parsed[count] = static_cast<uint8_t>(v);
    }
    count++;
    p = end;
    if (*p == ',') p++;
  }
  if (count < 4 || !isButtonFunctionMapValid(parsed)) {
    LOG_ERR("CPS", "button_map.txt invalid (count=%u)", static_cast<unsigned>(count));
    return false;
  }
  for (uint8_t i = 0; i < HW_REMAP_BUTTON_COUNT; i++) {
    hwButtonFunction[i] = parsed[i];
  }
  syncLegacyFrontButtonsFromHwMap();
  LOG_DBG("CPS", "Loaded button map from sidecar: %u,%u,%u,%u,%u,%u", hwButtonFunction[0], hwButtonFunction[1],
          hwButtonFunction[2], hwButtonFunction[3], hwButtonFunction[4], hwButtonFunction[5]);
  return true;
}

void CrossPointSettings::applyButtonFunctionMap(const uint8_t* map) {
  if (map == nullptr || !isButtonFunctionMapValid(map)) return;
  for (uint8_t i = 0; i < HW_REMAP_BUTTON_COUNT; i++) {
    hwButtonFunction[i] = map[i];
  }
  syncLegacyFrontButtonsFromHwMap();
  // Always write the sidecar immediately so sleep/wake cannot lose the map
  // even if settings.json is later rewritten without the array.
  if (!saveButtonMapSidecar()) {
    LOG_ERR("CPS", "Failed to write button_map.txt sidecar");
  }
}

void CrossPointSettings::validateFrontButtonMapping(CrossPointSettings& settings) {
  // Prefer the physical→function map; repair to defaults if incomplete.
  // Never force Up/Down onto front keys when recovering from legacy fields —
  // that is what made Left/Right remaps look like they "reverted" after wake.
  if (!isButtonFunctionMapValid(settings.hwButtonFunction)) {
    // Try inverting legacy role→hardware fields once (older settings.json).
    uint8_t migrated[HW_REMAP_BUTTON_COUNT];
    setDefaultButtonFunctionMap(migrated);
    const uint8_t roles[] = {settings.frontButtonBack, settings.frontButtonConfirm, settings.frontButtonLeft,
                             settings.frontButtonRight};
    // Legacy role→hw: keep Back/Confirm/Left/Right meanings (not Up/Down).
    const uint8_t funcs[] = {BTN_FUNC_BACK, BTN_FUNC_CONFIRM, BTN_FUNC_LEFT, BTN_FUNC_RIGHT};
    bool legacyOk = true;
    for (size_t i = 0; i < 4; i++) {
      for (size_t j = i + 1; j < 4; j++) {
        if (roles[i] == roles[j]) legacyOk = false;
      }
      if (roles[i] >= FRONT_BUTTON_HARDWARE_COUNT) legacyOk = false;
    }
    if (legacyOk) {
      for (uint8_t i = 0; i < FRONT_BUTTON_HARDWARE_COUNT; i++) {
        migrated[i] = BTN_FUNC_NONE;
      }
      for (uint8_t r = 0; r < 4; r++) {
        migrated[roles[r]] = funcs[r];
      }
      // Sides default to Up/Down so list nav still has a vertical pair if fronts
      // are Left/Right (or vice versa). User can remap sides freely afterward.
      migrated[4] = BTN_FUNC_UP;
      migrated[5] = BTN_FUNC_DOWN;
    }
    if (isButtonFunctionMapValid(migrated)) {
      for (uint8_t i = 0; i < HW_REMAP_BUTTON_COUNT; i++) {
        settings.hwButtonFunction[i] = migrated[i];
      }
    } else {
      setDefaultButtonFunctionMap(settings.hwButtonFunction);
    }
  }
  settings.syncLegacyFrontButtonsFromHwMap();
}

uint8_t CrossPointSettings::sleepTimeoutEnumToMinutes(const uint8_t legacyValue) {
  switch (legacyValue) {
    case SLEEP_1_MIN:
      return 1;
    case SLEEP_5_MIN:
      return 5;
    case SLEEP_15_MIN:
      return 15;
    case SLEEP_30_MIN:
      return 30;
    case SLEEP_10_MIN:
    default:
      return 10;
  }
}

void CrossPointSettings::toJson(JsonDocument& doc) const {
  const CrossPointSettings& s = *this;

  for (const auto& info : getSettingsList()) {
    if (!info.key) continue;
    // Dynamic entries (KOReader etc.) are stored in their own files — skip.
    if (!info.valuePtr && !info.stringOffset) continue;

    if (info.stringOffset) {
      const char* strPtr = (const char*)&s + info.stringOffset;
      if (info.obfuscated) {
        char obfKey[OBF_KEY_BUF];
        snprintf(obfKey, sizeof(obfKey), "%s_obf", info.key);
        doc[obfKey] = obfuscation::obfuscateToBase64(strPtr);
      } else {
        doc[info.key] = strPtr;
      }
    } else {
      doc[info.key] = s.*(info.valuePtr);
    }
  }

  // Button remap — managed by RemapFrontButtons sub-activity, not in SettingsList.
  doc["frontButtonBack"] = frontButtonBack;
  doc["frontButtonConfirm"] = frontButtonConfirm;
  doc["frontButtonLeft"] = frontButtonLeft;
  doc["frontButtonRight"] = frontButtonRight;
  {
    JsonArray arr = doc["hwButtonFunction"].to<JsonArray>();
    for (uint8_t i = 0; i < HW_REMAP_BUTTON_COUNT; i++) {
      arr.add(static_cast<int>(hwButtonFunction[i]));  // int avoids uint8_t JSON quirks on reload
    }
  }
  // Compact string form (also written to button_map.txt sidecar on apply).
  {
    char mapStr[24];
    int n = 0;
    for (uint8_t i = 0; i < HW_REMAP_BUTTON_COUNT; i++) {
      if (i > 0 && n < static_cast<int>(sizeof(mapStr)) - 1) mapStr[n++] = ',';
      n += snprintf(mapStr + n, sizeof(mapStr) - static_cast<size_t>(n), "%u",
                    static_cast<unsigned>(hwButtonFunction[i]));
    }
    if (n > 0 && n < static_cast<int>(sizeof(mapStr))) {
      mapStr[n] = '\0';
      doc["hwButtonMap"] = mapStr;
    }
  }
  // Font family — uses dynamic getter/setter in SettingsList so the generic loop skips it.
  doc["fontFamily"] = fontFamily;
  // Theme + battery % use DynamicEnum (display remapping); valuePtr is null so the
  // generic loop skips them — persist the real storage values here or every reboot
  // would fall back to struct defaults (Dashboard / show %).
  doc["uiTheme"] = uiTheme;
  doc["hideBatteryPercentage"] = hideBatteryPercentage;
  // SD card font family name — not in SettingsList, save manually
  if (sdFontFamilyName[0] != '\0') {
    doc["sdFontFamilyName"] = sdFontFamilyName;
  }
  // Dictionary: multi-select list + legacy single name (first enabled).
  if (dictionaryList[0] != '\0') {
    doc["dictionaryList"] = dictionaryList;
  }
  if (dictionaryName[0] != '\0') {
    doc["dictionaryName"] = dictionaryName;
  }

  // Language -- managed by LanguageSelectActivity, not in SettingsList.
  // Stored as ISO code string ("EN", "DE", ...) for stability across enum reorders.
  doc["language"] = (language < getLanguageCount()) ? LANGUAGE_CODES[language] : "EN";

  // Casper migration flags (not in SettingsList).
  doc["casperHomeMigrated"] = casperHomeMigrated;
  doc["casperControlsMigrated"] = casperControlsMigrated;
  doc["casperClockDefaultsMigrated"] = casperClockDefaultsMigrated;
  doc["casperStatusBarCornersMigrated"] = casperStatusBarCornersMigrated;
  doc["casperStatusBarSixSlotsMigrated"] = casperStatusBarSixSlotsMigrated;
  doc["casperStatusBarTitleSplitMigrated"] = casperStatusBarTitleSplitMigrated;
  doc["casperProgressBarOrderMigrated"] = casperProgressBarOrderMigrated;
  doc["casperSystemStatusBarMigrated"] = casperSystemStatusBarMigrated;
  doc["casperOpendyslexicMigrated"] = casperOpendyslexicMigrated;
  doc["casperBuiltinFontsSlimMigrated"] = casperBuiltinFontsSlimMigrated;
  doc["casperButtonAxisMigrated"] = casperButtonAxisMigrated;
  doc["casperSpeedDefaultsMigrated"] = casperSpeedDefaultsMigrated;
  // System top chrome slots (also in SettingsList when present).
  doc["systemStatusBarLeft"] = systemStatusBarLeft;
  doc["systemStatusBarMiddle"] = systemStatusBarMiddle;
  doc["systemStatusBarRight"] = systemStatusBarRight;
  doc["systemBatteryDisplay"] = systemBatteryDisplay;
  doc["readerBatteryDisplay"] = readerBatteryDisplay;
  // XTC overlay placement — on-device Customize Reader UI only (not SettingsList).
  doc["xtcStatusBarMode"] = xtcStatusBarMode;
  // Long power button (also in SettingsList when present).
  doc["longPwrBtn"] = longPwrBtn;
  // Time-left mode is in SettingsList when STR_TIME_LEFT is wired; also persist manually
  // so older SettingsList builds without the enum still keep the value.
  doc["statusBarTimeLeft"] = statusBarTimeLeft;
  // Six status-bar slots (also in SettingsList when present).
  doc["statusBarUpperLeft"] = statusBarUpperLeft;
  doc["statusBarUpperMiddle"] = statusBarUpperMiddle;
  doc["statusBarUpperRight"] = statusBarUpperRight;
  doc["statusBarLowerLeft"] = statusBarLowerLeft;
  doc["statusBarLowerMiddle"] = statusBarLowerMiddle;
  doc["statusBarLowerRight"] = statusBarLowerRight;
}

bool CrossPointSettings::fromJson(JsonVariantConst doc) {
  CrossPointSettings& s = *this;
  bool needsResave = false;

  auto clamp = [](uint8_t val, uint8_t maxVal, uint8_t def) -> uint8_t { return val < maxVal ? val : def; };

  for (const auto& info : getSettingsList()) {
    if (!info.key) continue;
    // Dynamic entries (KOReader etc.) are stored in their own files — skip.
    if (!info.valuePtr && !info.stringOffset) continue;

    if (info.stringOffset) {
      // destPtr starts out holding the struct-initializer default; it stays that
      // way unless the document actually carries a value for this key.
      char* destPtr = (char*)&s + info.stringOffset;
      if (info.stringMaxLen == 0) {
        LOG_ERR("CPS", "Misconfigured SettingInfo: stringMaxLen is 0 for key '%s'", info.key);
        destPtr[0] = '\0';
        needsResave = true;
        continue;
      }

      bool loaded = false;
      if (info.obfuscated) {
        char obfKey[OBF_KEY_BUF];
        snprintf(obfKey, sizeof(obfKey), "%s_obf", info.key);
        bool ok = false;
        const std::string decoded = obfuscation::deobfuscateFromBase64(doc[obfKey] | "", &ok);
        if (ok && !decoded.empty()) {
          copyToField(destPtr, decoded.c_str(), info.stringMaxLen);
          loaded = true;
        }
      }
      if (!loaded) {
        // Read as const char*, never `| std::string(...)`: ArduinoJson's
        // std::string converter drags a per-TU copy of the serializer into
        // flash. See the note in PersistableStore.h.
        const char* raw = doc[info.key].is<const char*>() ? doc[info.key].as<const char*>() : nullptr;
        if (raw) {
          // Obfuscated field recovered from a legacy plaintext value -> resave.
          if (info.obfuscated && strcmp(raw, destPtr) != 0) needsResave = true;
          copyToField(destPtr, raw, info.stringMaxLen);
        }
      }
    } else {
      const uint8_t fieldDefault = s.*(info.valuePtr);  // struct-initializer default, read before we overwrite it
      uint8_t v = doc[info.key] | fieldDefault;
      if (info.type == SettingType::ENUM) {
        v = clamp(v, (uint8_t)info.enumValues.size(), fieldDefault);
      } else if (info.type == SettingType::TOGGLE) {
        v = clamp(v, (uint8_t)2, fieldDefault);
      } else if (info.type == SettingType::VALUE) {
        if (v < info.valueRange.min)
          v = info.valueRange.min;
        else if (v > info.valueRange.max)
          v = info.valueRange.max;
      }
      s.*(info.valuePtr) = v;
    }
  }

  if (doc["sleepTimeoutMinutes"].isNull() && !doc["sleepTimeout"].isNull()) {
    const uint8_t legacyValue =
        clamp(doc["sleepTimeout"] | (uint8_t)SLEEP_10_MIN, SLEEP_TIMEOUT_COUNT, (uint8_t)SLEEP_10_MIN);
    sleepTimeoutMinutes = sleepTimeoutEnumToMinutes(legacyValue);
    needsResave = true;
  }
  // Button remap — physical→function array, with legacy role→hardware fallback.
  frontButtonBack = clamp(doc["frontButtonBack"] | (uint8_t)FRONT_HW_BACK, FRONT_BUTTON_HARDWARE_COUNT, FRONT_HW_BACK);
  frontButtonConfirm =
      clamp(doc["frontButtonConfirm"] | (uint8_t)FRONT_HW_CONFIRM, FRONT_BUTTON_HARDWARE_COUNT, FRONT_HW_CONFIRM);
  frontButtonLeft = clamp(doc["frontButtonLeft"] | (uint8_t)FRONT_HW_LEFT, FRONT_BUTTON_HARDWARE_COUNT, FRONT_HW_LEFT);
  frontButtonRight =
      clamp(doc["frontButtonRight"] | (uint8_t)FRONT_HW_RIGHT, FRONT_BUTTON_HARDWARE_COUNT, FRONT_HW_RIGHT);
  // Physical→function map. Priority: sidecar file → hwButtonMap string → JSON array
  // → legacy role fields. Sidecar wins so sleep/wake cannot lose remaps.
  setDefaultButtonFunctionMap(hwButtonFunction);
  bool mapLoaded = loadButtonMapSidecar();
  if (!mapLoaded && doc["hwButtonMap"].is<const char*>()) {
    const char* s = doc["hwButtonMap"].as<const char*>();
    if (s != nullptr && s[0] != '\0') {
      uint8_t parsed[HW_REMAP_BUTTON_COUNT];
      setDefaultButtonFunctionMap(parsed);
      uint8_t count = 0;
      const char* p = s;
      while (*p && count < HW_REMAP_BUTTON_COUNT) {
        while (*p == ' ' || *p == '\t') p++;
        char* end = nullptr;
        const long v = strtol(p, &end, 10);
        if (end == p) break;
        if (v >= 0 && v < static_cast<long>(BTN_FUNC_COUNT)) {
          parsed[count] = static_cast<uint8_t>(v);
        }
        count++;
        p = end;
        if (*p == ',') p++;
      }
      if (count >= 4 && isButtonFunctionMapValid(parsed)) {
        for (uint8_t i = 0; i < HW_REMAP_BUTTON_COUNT; i++) {
          hwButtonFunction[i] = parsed[i];
        }
        mapLoaded = true;
        LOG_DBG("CPS", "Loaded button map from hwButtonMap string");
      }
    }
  }
  if (!mapLoaded && doc["hwButtonFunction"].is<JsonArray>()) {
    JsonArrayConst arr = doc["hwButtonFunction"].as<JsonArrayConst>();
    uint8_t i = 0;
    for (JsonVariantConst v : arr) {
      if (i >= HW_REMAP_BUTTON_COUNT) break;
      // as<int>() — never `v | (uint8_t)` (is<uint8_t>() fails for JSON ints).
      if (!v.isNull()) {
        const int raw = v.as<int>();
        if (raw >= 0 && raw < static_cast<int>(BTN_FUNC_COUNT)) {
          hwButtonFunction[i] = static_cast<uint8_t>(raw);
        }
      }
      i++;
    }
    mapLoaded = isButtonFunctionMapValid(hwButtonFunction);
    if (mapLoaded) {
      LOG_DBG("CPS", "Loaded button map from hwButtonFunction array");
    }
  }
  validateFrontButtonMapping(s);
  // Keep sidecar in sync with whatever we ended up with (repairs old installs).
  if (isButtonFunctionMapValid(hwButtonFunction)) {
    (void)saveButtonMapSidecar();
  }

  // Flag only — never rewrite a valid user map (Left/Right on front keys is fine).
  {
    const uint8_t axisMigrated = doc["casperButtonAxisMigrated"] | (uint8_t)0;
    if (axisMigrated == 0) {
      casperButtonAxisMigrated = 1;
      needsResave = true;
      LOG_DBG("CPS", "casperButtonAxisMigrated: marked (no map rewrite)");
    } else {
      casperButtonAxisMigrated = 1;
    }
  }

  // Font family — uses dynamic getter/setter in SettingsList so the generic loop skips it.
  const uint8_t storedFontFamily = doc["fontFamily"] | (uint8_t)0;
  // SD card font family name — not in SettingsList, load manually.
  const char* sfn = doc["sdFontFamilyName"] | "";
  strncpy(sdFontFamilyName, sfn, sizeof(sdFontFamilyName) - 1);
  sdFontFamilyName[sizeof(sdFontFamilyName) - 1] = '\0';

  // One-time: old OpenDyslexic builtin id (2) with empty SD name → SD OpenDyslexic.
  const uint8_t odMigrated = doc["casperOpendyslexicMigrated"] | (uint8_t)0;
  if (odMigrated == 0) {
    if (storedFontFamily == LEGACY_OPENDYSLEXIC && sdFontFamilyName[0] == '\0') {
      fontFamily = SOURCESERIF4;
      strncpy(sdFontFamilyName, "OpenDyslexic", sizeof(sdFontFamilyName) - 1);
      sdFontFamilyName[sizeof(sdFontFamilyName) - 1] = '\0';
      needsResave = true;
      LOG_DBG("CPS", "casperOpendyslexicMigrated: fontFamily 2 -> SD OpenDyslexic");
    }
    casperOpendyslexicMigrated = 1;
    needsResave = true;
  } else {
    casperOpendyslexicMigrated = 1;
  }

  // Compact builtins: Source Serif 4 (0) + Bitter (1). Remap older multi-family IDs.
  // Old: 0=Lexend, 1=Bitter, 2=Source Serif, 3=Literata.
  const uint8_t slimMigrated = doc["casperBuiltinFontsSlimMigrated"] | (uint8_t)0;
  if (slimMigrated == 0) {
    if (sdFontFamilyName[0] != '\0') {
      fontFamily = SOURCESERIF4;  // SD name is source of truth
    } else if (storedFontFamily == 1) {
      fontFamily = BITTER;  // old Bitter id
    } else {
      fontFamily = SOURCESERIF4;  // Lexend / Source / Literata / unknown → Source Serif
    }
    casperBuiltinFontsSlimMigrated = 1;
    needsResave = true;
    LOG_DBG("CPS", "casperBuiltinFontsSlimMigrated: fontFamily=%u", fontFamily);
  } else {
    casperBuiltinFontsSlimMigrated = 1;
    fontFamily = clamp(storedFontFamily, BUILTIN_FONT_COUNT, static_cast<uint8_t>(SOURCESERIF4));
    if (storedFontFamily >= BUILTIN_FONT_COUNT) needsResave = true;
  }

  // Theme / battery %: DynamicEnum (no valuePtr) — load real storage values.
  // Unknown / out-of-range theme ids fall back to Stats (then remapped if needed).
  if (!doc["uiTheme"].isNull()) {
    const uint8_t storedTheme = doc["uiTheme"] | static_cast<uint8_t>(STATS);
    // Accept any known append-only id; remaps below collapse legacy skins → Stats.
    uiTheme = storedTheme;
  }
  if (!doc["hideBatteryPercentage"].isNull()) {
    hideBatteryPercentage =
        clamp(doc["hideBatteryPercentage"] | static_cast<uint8_t>(HIDE_NEVER), HIDE_BATTERY_PERCENTAGE_COUNT,
              static_cast<uint8_t>(HIDE_NEVER));
  }
  // Stat tracking: prefer readingStatsEnabled; migrate legacy disableReadingStats (inverted).
  if (!doc["readingStatsEnabled"].isNull()) {
    readingStatsEnabled = (doc["readingStatsEnabled"] | (uint8_t)1) != 0 ? 1 : 0;
  } else if (!doc["disableReadingStats"].isNull()) {
    readingStatsEnabled = (doc["disableReadingStats"] | (uint8_t)0) != 0 ? 0 : 1;
    needsResave = true;
  }
  // Dictionary multi-select (newline-separated). Migrate legacy dictionaryName.
  copyToField(dictionaryList, doc["dictionaryList"] | "", sizeof(dictionaryList));
  copyToField(dictionaryName, doc["dictionaryName"] | "", sizeof(dictionaryName));
  if (dictionaryList[0] == '\0' && dictionaryName[0] != '\0') {
    copyToField(dictionaryList, dictionaryName, sizeof(dictionaryList));
    needsResave = true;
  } else if (dictionaryList[0] != '\0' && dictionaryName[0] == '\0') {
    // Keep dictionaryName as first entry for any code that still reads it.
    const char* nl = strchr(dictionaryList, '\n');
    const size_t n = nl ? static_cast<size_t>(nl - dictionaryList) : strlen(dictionaryList);
    const size_t copyN = std::min(n, sizeof(dictionaryName) - 1);
    memcpy(dictionaryName, dictionaryList, copyN);
    dictionaryName[copyN] = '\0';
  }

  // Language -- stored as code string for stability across enum reorders.
  if (doc["language"].is<const char*>()) {
    language = static_cast<uint8_t>(I18n::languageFromCode(doc["language"].as<const char*>()));
  }

  // Time-left (may also be loaded via SettingsList if present).
  statusBarTimeLeft =
      clamp(doc["statusBarTimeLeft"] | (uint8_t)TIME_LEFT_BOOK, STATUS_BAR_TIME_LEFT_COUNT, TIME_LEFT_BOOK);

  // One-time Casper home migration: old SD settings often still have uiTheme=Lyra (1).
  // X4 → Bare (clean cover home); X3 → Stats (stats chrome fits the taller panel).
  // After this runs once, the user can change theme freely in Settings.
  const uint8_t migrated = doc["casperHomeMigrated"] | (uint8_t)0;
  if (migrated == 0) {
    uiTheme = gpio.deviceIsX4() ? static_cast<uint8_t>(BARE) : static_cast<uint8_t>(STATS);
    statusBarProgressBar = BOOK_PROGRESS;
    statusBarProgressBarThickness = PROGRESS_BAR_THIN;
    statusBarBattery = 1;
    statusBarBookProgressPercentage = 1;
    statusBarChapterPageCount = 1;
    statusBarTimeLeft = TIME_LEFT_BOOK;
    screenMargin = 10;
    casperHomeMigrated = 1;
    casperProgressBarOrderMigrated = 1;  // BOOK_PROGRESS already uses new enum values
    needsResave = true;
    LOG_DBG("CPS", "casperHomeMigrated: defaulted uiTheme=%s, screenMargin=10",
            gpio.deviceIsX4() ? "Bare" : "Stats");
  } else {
    casperHomeMigrated = 1;
  }

  // Removed skins + Stats-Life merge into Stats (Bare / SPECTRAL stay).
  // Enum values remain stable in JSON. Shelf + Stats Scroll parked.
  if (uiTheme == MINIMAL || uiTheme == LYRA_CAROUSEL || uiTheme == LYRA || uiTheme == LYRA_3_COVERS ||
      uiTheme == ROUNDEDRAFF || uiTheme == CLASSIC || uiTheme == DASHBOARD_MAGAZINE || uiTheme == DASHBOARD_CARD ||
      uiTheme == DASHBOARD_RECENTS || uiTheme == DASHBOARD_SCROLL || uiTheme == STATS_LIFE) {
    uiTheme = STATS;
    needsResave = true;
    LOG_DBG("CPS", "Remapped removed/legacy/Stats-Life theme → Stats");
  }

  // Ghost parked — fall back to Bare until the theme returns.
  if (uiTheme == GHOST) {
    uiTheme = BARE;
    needsResave = true;
    LOG_DBG("CPS", "Remapped Ghost → Bare (theme parked)");
  }

  // SPECTRAL is X3-only. X4 (or missing RTC path) falls back to Bare.
  if (uiTheme == SPECTRAL && !gpio.deviceIsX3()) {
    uiTheme = BARE;
    needsResave = true;
    LOG_DBG("CPS", "Remapped SPECTRAL → Bare (X3-only theme)");
  }

  // SPECTRAL: never keep a system-bar clock (home owns the clock).
  if (uiTheme == SPECTRAL && systemStatusBarHas(SYS_SLOT_CLOCK)) {
    stripSystemStatusBarClock();
    needsResave = true;
  }

  // One-time: progress bar enum was Book=0, Chapter=1, Hide=2 → Hide=0, Book=1, Chapter=2.
  const uint8_t progressOrderMigrated = doc["casperProgressBarOrderMigrated"] | (uint8_t)0;
  if (progressOrderMigrated == 0 && casperProgressBarOrderMigrated == 0) {
    const uint8_t old = statusBarProgressBar;
    if (old == 0) {
      statusBarProgressBar = BOOK_PROGRESS;  // was BOOK
    } else if (old == 1) {
      statusBarProgressBar = CHAPTER_PROGRESS;  // was CHAPTER
    } else if (old == 2) {
      statusBarProgressBar = HIDE_PROGRESS;  // was HIDE
    } else {
      statusBarProgressBar = BOOK_PROGRESS;
    }
    casperProgressBarOrderMigrated = 1;
    needsResave = true;
    LOG_DBG("CPS", "casperProgressBarOrderMigrated: old=%u -> %u", old, statusBarProgressBar);
  } else {
    casperProgressBarOrderMigrated = 1;
  }

  // One-time controls migration: short sleep / long force-refresh / long-press dictionary.
  const uint8_t controlsMigrated = doc["casperControlsMigrated"] | (uint8_t)0;
  if (controlsMigrated == 0) {
    shortPwrBtn = SLEEP;
    longPwrBtn = FORCE_REFRESH;
    longPressMenuFunction = LP_MENU_DICTIONARY;
    casperControlsMigrated = 1;
    needsResave = true;
    LOG_DBG("CPS", "casperControlsMigrated: short=Sleep long=ForceRefresh menu=Dictionary");
  } else {
    casperControlsMigrated = 1;
  }

  // One-time clock defaults: 12-hour + UTC-7; keep clocks visible by default.
  const uint8_t clockMigrated = doc["casperClockDefaultsMigrated"] | (uint8_t)0;
  if (clockMigrated == 0) {
    clockFormat = 1;        // 12-hour with AM/PM
    clockUtcOffsetQ = 20;   // UTC-7 (48 + (-7)*4)
    if (statusBarClock == STATUS_BAR_CLOCK_HIDE) {
      statusBarClock = STATUS_BAR_CLOCK_SHOW;
    }
    systemClock = STATUS_BAR_CLOCK_SHOW;
    casperClockDefaultsMigrated = 1;
    needsResave = true;
    LOG_DBG("CPS", "casperClockDefaultsMigrated: 12h UTC-7 system+reader clock visible");
  } else {
    casperClockDefaultsMigrated = 1;
  }

  // One-time speed defaults: text AA and embedded CSS off (open/page-turn cost).
  // Users can re-enable either in Manage Fonts after this migration runs once.
  const uint8_t speedMigrated = doc["casperSpeedDefaultsMigrated"] | (uint8_t)0;
  if (speedMigrated == 0) {
    textAntiAliasing = 0;
    embeddedStyle = 0;
    casperSpeedDefaultsMigrated = 1;
    needsResave = true;
    LOG_DBG("CPS", "casperSpeedDefaultsMigrated: textAA=off embeddedStyle=off");
  } else {
    casperSpeedDefaultsMigrated = 1;
  }

  // Clock is always top-center when shown. Collapse legacy Left (2) / invalid into Show.
  if (statusBarClock != STATUS_BAR_CLOCK_HIDE && statusBarClock != STATUS_BAR_CLOCK_SHOW) {
    statusBarClock = STATUS_BAR_CLOCK_SHOW;
    needsResave = true;
    LOG_DBG("CPS", "statusBarClock: legacy left/right -> show (center-only)");
  }
  // systemClock is new: default Show when missing; clamp legacy values.
  if (doc["systemClock"].isNull()) {
    systemClock = STATUS_BAR_CLOCK_SHOW;
    needsResave = true;
  } else if (systemClock != STATUS_BAR_CLOCK_HIDE && systemClock != STATUS_BAR_CLOCK_SHOW) {
    systemClock = STATUS_BAR_CLOCK_SHOW;
    needsResave = true;
  }

  // One-time: map legacy hideBattery + systemClock into Left/Middle/Right system slots.
  // Also load slots when present (web API / later boots).
  auto clampSysSlot = [](uint8_t v) -> uint8_t {
    return v < SYSTEM_STATUS_SLOT_COUNT ? v : static_cast<uint8_t>(SYS_SLOT_HIDE);
  };
  const bool hasSysSlots = !doc["systemStatusBarLeft"].isNull() || !doc["systemStatusBarMiddle"].isNull() ||
                           !doc["systemStatusBarRight"].isNull();
  const uint8_t sysSlotsMigrated = doc["casperSystemStatusBarMigrated"] | (uint8_t)0;
  // Battery display mode: prefer new key; migrate legacy systemBatteryShowPercent (0/1).
  auto clampBattDisplay = [](uint8_t v) -> uint8_t {
    return v < BATTERY_DISPLAY_MODE_COUNT ? v : static_cast<uint8_t>(BATTERY_DISPLAY_ICON_PERCENT);
  };
  if (!doc["systemBatteryDisplay"].isNull()) {
    systemBatteryDisplay = clampBattDisplay(doc["systemBatteryDisplay"] | (uint8_t)BATTERY_DISPLAY_ICON_PERCENT);
  } else if (!doc["systemBatteryShowPercent"].isNull()) {
    // Legacy: 0 = icon only, 1 = icon + percent.
    systemBatteryDisplay = (doc["systemBatteryShowPercent"] | (uint8_t)1) != 0
                               ? static_cast<uint8_t>(BATTERY_DISPLAY_ICON_PERCENT)
                               : static_cast<uint8_t>(BATTERY_DISPLAY_ICON);
    needsResave = true;
  }
  if (!doc["readerBatteryDisplay"].isNull()) {
    readerBatteryDisplay = clampBattDisplay(doc["readerBatteryDisplay"] | (uint8_t)BATTERY_DISPLAY_ICON_PERCENT);
  }
  if (hasSysSlots) {
    systemStatusBarLeft = clampSysSlot(doc["systemStatusBarLeft"] | systemStatusBarLeft);
    systemStatusBarMiddle = clampSysSlot(doc["systemStatusBarMiddle"] | systemStatusBarMiddle);
    systemStatusBarRight = clampSysSlot(doc["systemStatusBarRight"] | systemStatusBarRight);
    // Enforce exclusivity (Battery/Clock only once).
    if (systemStatusBarHas(SYS_SLOT_BATTERY)) {
      // keep first occurrence left→middle→right
      bool sawBatt = false;
      auto dedupe = [&](uint8_t& s) {
        if (s == SYS_SLOT_BATTERY) {
          if (sawBatt) s = SYS_SLOT_HIDE;
          else sawBatt = true;
        }
      };
      dedupe(systemStatusBarLeft);
      dedupe(systemStatusBarMiddle);
      dedupe(systemStatusBarRight);
    }
    if (systemStatusBarHas(SYS_SLOT_CLOCK)) {
      bool sawClock = false;
      auto dedupe = [&](uint8_t& s) {
        if (s == SYS_SLOT_CLOCK) {
          if (sawClock) s = SYS_SLOT_HIDE;
          else sawClock = true;
        }
      };
      dedupe(systemStatusBarLeft);
      dedupe(systemStatusBarMiddle);
      dedupe(systemStatusBarRight);
    }
    syncSystemStatusLegacyFromSlots();
    casperSystemStatusBarMigrated = 1;
  } else if (sysSlotsMigrated == 0) {
    // Factory Stats layout: battery left + clock right (Icon + Percent by default).
    const bool showBatt = hideBatteryPercentage == HIDE_NEVER;
    const bool showClock = systemClock != STATUS_BAR_CLOCK_HIDE;
    systemStatusBarLeft = showBatt ? static_cast<uint8_t>(SYS_SLOT_BATTERY) : static_cast<uint8_t>(SYS_SLOT_HIDE);
    systemStatusBarMiddle = SYS_SLOT_HIDE;
    systemStatusBarRight = showClock ? static_cast<uint8_t>(SYS_SLOT_CLOCK) : static_cast<uint8_t>(SYS_SLOT_HIDE);
    if (showBatt && doc["systemBatteryDisplay"].isNull() && doc["systemBatteryShowPercent"].isNull()) {
      systemBatteryDisplay = BATTERY_DISPLAY_ICON_PERCENT;
    }
    syncSystemStatusLegacyFromSlots();
    casperSystemStatusBarMigrated = 1;
    needsResave = true;
    LOG_DBG("CPS", "casperSystemStatusBarMigrated: L=%u M=%u R=%u battDisp=%u", systemStatusBarLeft,
            systemStatusBarMiddle, systemStatusBarRight, systemBatteryDisplay);
  } else {
    casperSystemStatusBarMigrated = 1;
    syncSystemStatusLegacyFromSlots();
  }

  // One-time: map legacy battery / % / pages / time-left toggles into four corners.
  const uint8_t cornersMigrated = doc["casperStatusBarCornersMigrated"] | (uint8_t)0;
  auto clampCorner = [](uint8_t v) -> uint8_t {
    return v < STATUS_BAR_CORNER_CONTENT_COUNT ? v : static_cast<uint8_t>(CORNER_HIDE);
  };
  if (cornersMigrated == 0) {
    // Prefer explicit corner keys if present (partial saves); else map legacy toggles.
    if (!doc["statusBarUpperLeft"].isNull() || !doc["statusBarUpperRight"].isNull() ||
        !doc["statusBarLowerLeft"].isNull() || !doc["statusBarLowerRight"].isNull()) {
      statusBarUpperLeft = clampCorner(doc["statusBarUpperLeft"] | (uint8_t)CORNER_BATTERY);
      statusBarUpperRight = clampCorner(doc["statusBarUpperRight"] | (uint8_t)CORNER_PROGRESS_PERCENT);
      statusBarLowerLeft = clampCorner(doc["statusBarLowerLeft"] | (uint8_t)CORNER_TIME_LEFT_CHAPTER);
      statusBarLowerRight = clampCorner(doc["statusBarLowerRight"] | (uint8_t)CORNER_CHAPTER_PAGE_COUNTER);
    } else {
      statusBarUpperLeft = statusBarBattery ? CORNER_BATTERY : CORNER_HIDE;
      statusBarUpperRight = statusBarBookProgressPercentage ? CORNER_PROGRESS_PERCENT : CORNER_HIDE;
      if (statusBarTimeLeft == TIME_LEFT_BOOK) {
        statusBarLowerLeft = CORNER_TIME_LEFT_BOOK;
      } else if (statusBarTimeLeft == TIME_LEFT_CHAPTER) {
        statusBarLowerLeft = CORNER_TIME_LEFT_CHAPTER;
      } else {
        statusBarLowerLeft = CORNER_HIDE;
      }
      statusBarLowerRight = statusBarChapterPageCount ? CORNER_CHAPTER_PAGE_COUNTER : CORNER_HIDE;
    }
    casperStatusBarCornersMigrated = 1;
    needsResave = true;
    LOG_DBG("CPS", "casperStatusBarCornersMigrated: UL=%u UR=%u LL=%u LR=%u", statusBarUpperLeft, statusBarUpperRight,
            statusBarLowerLeft, statusBarLowerRight);
  } else {
    casperStatusBarCornersMigrated = 1;
    statusBarUpperLeft = clampCorner(doc["statusBarUpperLeft"] | statusBarUpperLeft);
    statusBarUpperRight = clampCorner(doc["statusBarUpperRight"] | statusBarUpperRight);
    statusBarLowerLeft = clampCorner(doc["statusBarLowerLeft"] | statusBarLowerLeft);
    statusBarLowerRight = clampCorner(doc["statusBarLowerRight"] | statusBarLowerRight);
  }

  // One-time: add middle slots from legacy clock/title show settings.
  const uint8_t sixSlotsMigrated = doc["casperStatusBarSixSlotsMigrated"] | (uint8_t)0;
  if (sixSlotsMigrated == 0) {
    if (!doc["statusBarUpperMiddle"].isNull()) {
      statusBarUpperMiddle = clampCorner(doc["statusBarUpperMiddle"] | (uint8_t)CORNER_HIDE);
    } else {
      statusBarUpperMiddle =
          (statusBarClock != STATUS_BAR_CLOCK_HIDE) ? static_cast<uint8_t>(CORNER_CLOCK) : static_cast<uint8_t>(CORNER_HIDE);
    }
    if (!doc["statusBarLowerMiddle"].isNull()) {
      statusBarLowerMiddle = clampCorner(doc["statusBarLowerMiddle"] | (uint8_t)CORNER_HIDE);
    } else {
      // Prefer chapter title when migrating from the old Book/Chapter/Hide title setting.
      if (statusBarTitle == BOOK_TITLE) {
        statusBarLowerMiddle = CORNER_BOOK_TITLE;
      } else if (statusBarTitle != HIDE_TITLE) {
        statusBarLowerMiddle = CORNER_CHAPTER_TITLE;
      } else {
        statusBarLowerMiddle = CORNER_HIDE;
      }
    }
    // Drop duplicate clock/title if already used in a corner (exclusive).
    auto clearDup = [this](uint8_t content, uint8_t& keepSlot) {
      uint8_t* slots[] = {&statusBarUpperLeft,   &statusBarUpperMiddle, &statusBarUpperRight,
                          &statusBarLowerLeft,   &statusBarLowerMiddle, &statusBarLowerRight};
      for (uint8_t* s : slots) {
        if (s != &keepSlot && *s == content) *s = CORNER_HIDE;
      }
    };
    if (statusBarUpperMiddle == CORNER_CLOCK) clearDup(CORNER_CLOCK, statusBarUpperMiddle);
    if (statusBarLowerMiddle == CORNER_BOOK_TITLE) clearDup(CORNER_BOOK_TITLE, statusBarLowerMiddle);
    if (statusBarLowerMiddle == CORNER_CHAPTER_TITLE) clearDup(CORNER_CHAPTER_TITLE, statusBarLowerMiddle);
    casperStatusBarSixSlotsMigrated = 1;
    needsResave = true;
    LOG_DBG("CPS", "casperStatusBarSixSlotsMigrated: UM=%u LM=%u", statusBarUpperMiddle, statusBarLowerMiddle);
  } else {
    casperStatusBarSixSlotsMigrated = 1;
    statusBarUpperMiddle = clampCorner(doc["statusBarUpperMiddle"] | statusBarUpperMiddle);
    statusBarLowerMiddle = clampCorner(doc["statusBarLowerMiddle"] | statusBarLowerMiddle);
  }

  // One-time: generic title slot (value 7) → Book Title or Chapter Title from legacy statusBarTitle.
  const uint8_t titleSplitMigrated = doc["casperStatusBarTitleSplitMigrated"] | (uint8_t)0;
  if (titleSplitMigrated == 0) {
    const uint8_t mappedTitle =
        (statusBarTitle == BOOK_TITLE) ? static_cast<uint8_t>(CORNER_BOOK_TITLE)
                                       : static_cast<uint8_t>(CORNER_CHAPTER_TITLE);
    uint8_t* slots[] = {&statusBarUpperLeft,   &statusBarUpperMiddle, &statusBarUpperRight,
                        &statusBarLowerLeft,   &statusBarLowerMiddle, &statusBarLowerRight};
    for (uint8_t* s : slots) {
      // 7 was the only "Title" content before the split.
      if (*s == CORNER_BOOK_TITLE) {
        *s = mappedTitle;
      }
    }
    casperStatusBarTitleSplitMigrated = 1;
    needsResave = true;
  } else {
    casperStatusBarTitleSplitMigrated = 1;
  }

  // Sync legacy clock toggle from slot placement (web clients that still read statusBarClock).
  statusBarClock = statusBarCornerHas(CORNER_CLOCK) ? STATUS_BAR_CLOCK_SHOW : STATUS_BAR_CLOCK_HIDE;

  // XTC overlay: load legacy mode, then prefer slot placement if present.
  if (!doc["xtcStatusBarMode"].isNull()) {
    xtcStatusBarMode = clamp(doc["xtcStatusBarMode"] | (uint8_t)XTC_STATUS_BAR_HIDE, XTC_STATUS_BAR_MODE_COUNT,
                             (uint8_t)XTC_STATUS_BAR_HIDE);
  }
  // One-time: map legacy Hide/Bottom/Top into a slot placement when no XTC marker yet.
  if (!statusBarCornerHas(CORNER_XTC_STATUS_BAR) && xtcStatusBarMode != XTC_STATUS_BAR_HIDE) {
    auto tryPlace = [this](uint8_t& slot) -> bool {
      if (slot == CORNER_HIDE) {
        slot = CORNER_XTC_STATUS_BAR;
        return true;
      }
      return false;
    };
    bool placed = false;
    if (xtcStatusBarMode == XTC_STATUS_BAR_TOP) {
      placed = tryPlace(statusBarUpperMiddle) || tryPlace(statusBarUpperLeft) || tryPlace(statusBarUpperRight);
    } else if (xtcStatusBarMode == XTC_STATUS_BAR_BOTTOM) {
      placed = tryPlace(statusBarLowerMiddle) || tryPlace(statusBarLowerLeft) || tryPlace(statusBarLowerRight);
    }
    if (placed) needsResave = true;
  }
  syncXtcStatusBarModeFromSlots();

  // longPwrBtn may also load via SettingsList; clamp if only present in doc.
  if (!doc["longPwrBtn"].isNull()) {
    longPwrBtn = clamp(doc["longPwrBtn"] | (uint8_t)FORCE_REFRESH, SHORT_PWRBTN_COUNT, (uint8_t)FORCE_REFRESH);
  }

  if (needsResave) {
    LOG_DBG("CPS", "Resaving settings to update format");
    requestResave();
  }

  LOG_DBG("CPS", "Settings loaded from file");

  return true;
}

bool CrossPointSettings::statusBarCornerHas(const uint8_t content) const {
  if (content == CORNER_HIDE) return false;
  return statusBarUpperLeft == content || statusBarUpperMiddle == content || statusBarUpperRight == content ||
         statusBarLowerLeft == content || statusBarLowerMiddle == content || statusBarLowerRight == content;
}

bool CrossPointSettings::systemStatusBarHas(const uint8_t content) const {
  if (content == SYS_SLOT_HIDE) return false;
  return systemStatusBarLeft == content || systemStatusBarMiddle == content || systemStatusBarRight == content;
}

void CrossPointSettings::syncSystemStatusLegacyFromSlots() {
  // System clock show/hide follows whether Clock is placed on the system bar.
  // Battery display mode (systemBatteryDisplay) is independent of slot placement;
  // reader battery uses readerBatteryDisplay + hideBatteryPercentage master separately.
  systemClock = systemStatusBarHas(SYS_SLOT_CLOCK) ? static_cast<uint8_t>(STATUS_BAR_CLOCK_SHOW)
                                                   : static_cast<uint8_t>(STATUS_BAR_CLOCK_HIDE);
}

void CrossPointSettings::assignSystemStatusBarSlot(uint8_t& slotField, uint8_t content) {
  if (content >= SYSTEM_STATUS_SLOT_COUNT) content = SYS_SLOT_HIDE;
  // SPECTRAL owns the clock on the home screen — never place it on the system bar.
  if (content == SYS_SLOT_CLOCK && !systemStatusBarAllowsClock()) {
    content = SYS_SLOT_HIDE;
  }
  if (content != SYS_SLOT_HIDE) {
    uint8_t* slots[] = {&systemStatusBarLeft, &systemStatusBarMiddle, &systemStatusBarRight};
    for (uint8_t* s : slots) {
      if (s != &slotField && *s == content) *s = SYS_SLOT_HIDE;
    }
  }
  slotField = content;
  syncSystemStatusLegacyFromSlots();
}

bool CrossPointSettings::systemStatusBarAllowsClock() const {
  return static_cast<UI_THEME>(uiTheme) != UI_THEME::SPECTRAL;
}

void CrossPointSettings::stripSystemStatusBarClock() {
  if (systemStatusBarLeft == SYS_SLOT_CLOCK) systemStatusBarLeft = SYS_SLOT_HIDE;
  if (systemStatusBarMiddle == SYS_SLOT_CLOCK) systemStatusBarMiddle = SYS_SLOT_HIDE;
  if (systemStatusBarRight == SYS_SLOT_CLOCK) systemStatusBarRight = SYS_SLOT_HIDE;
  syncSystemStatusLegacyFromSlots();
}

void CrossPointSettings::syncXtcStatusBarModeFromSlots() {
  // Upper row → top overlay; lower row → bottom; not placed → hide (XTC reader only).
  if (statusBarUpperLeft == CORNER_XTC_STATUS_BAR || statusBarUpperMiddle == CORNER_XTC_STATUS_BAR ||
      statusBarUpperRight == CORNER_XTC_STATUS_BAR) {
    xtcStatusBarMode = XTC_STATUS_BAR_TOP;
  } else if (statusBarLowerLeft == CORNER_XTC_STATUS_BAR || statusBarLowerMiddle == CORNER_XTC_STATUS_BAR ||
             statusBarLowerRight == CORNER_XTC_STATUS_BAR) {
    xtcStatusBarMode = XTC_STATUS_BAR_BOTTOM;
  } else {
    xtcStatusBarMode = XTC_STATUS_BAR_HIDE;
  }
}

void CrossPointSettings::assignStatusBarCorner(uint8_t& cornerField, uint8_t content) {
  if (content >= STATUS_BAR_CORNER_CONTENT_COUNT) content = CORNER_HIDE;
  if (content != CORNER_HIDE) {
    uint8_t* slots[] = {&statusBarUpperLeft,   &statusBarUpperMiddle, &statusBarUpperRight,
                        &statusBarLowerLeft,   &statusBarLowerMiddle, &statusBarLowerRight};
    for (uint8_t* s : slots) {
      if (s != &cornerField && *s == content) *s = CORNER_HIDE;
    }
  }
  cornerField = content;

  // Keep legacy toggles in sync for any remaining readers of the old fields.
  statusBarBattery = statusBarCornerHas(CORNER_BATTERY) ? 1 : 0;
  statusBarBookProgressPercentage = statusBarCornerHas(CORNER_PROGRESS_PERCENT) ? 1 : 0;
  statusBarChapterPageCount = statusBarCornerHas(CORNER_CHAPTER_PAGE_COUNTER) ? 1 : 0;
  statusBarClock = statusBarCornerHas(CORNER_CLOCK) ? STATUS_BAR_CLOCK_SHOW : STATUS_BAR_CLOCK_HIDE;
  if (statusBarCornerHas(CORNER_TIME_LEFT_BOOK)) {
    statusBarTimeLeft = TIME_LEFT_BOOK;
  } else if (statusBarCornerHas(CORNER_TIME_LEFT_CHAPTER)) {
    statusBarTimeLeft = TIME_LEFT_CHAPTER;
  } else {
    statusBarTimeLeft = TIME_LEFT_HIDE;
  }
  syncXtcStatusBarModeFromSlots();
}

CrossPointSettings::StatusBarSpec CrossPointSettings::statusBarSpec() const {
  StatusBarSpec spec;
  auto clampSlot = [](uint8_t v) -> uint8_t {
    return v < STATUS_BAR_CORNER_CONTENT_COUNT ? v : static_cast<uint8_t>(CORNER_HIDE);
  };
  spec.upperLeft = clampSlot(statusBarUpperLeft);
  spec.upperMiddle = clampSlot(statusBarUpperMiddle);
  spec.upperRight = clampSlot(statusBarUpperRight);
  spec.lowerLeft = clampSlot(statusBarLowerLeft);
  spec.lowerMiddle = clampSlot(statusBarLowerMiddle);
  spec.lowerRight = clampSlot(statusBarLowerRight);
  spec.showChapterPageCount = statusBarCornerHas(CORNER_CHAPTER_PAGE_COUNTER);
  spec.showBookPageCount = statusBarCornerHas(CORNER_BOOK_PAGE_COUNTER);
  spec.showBookProgressPercent = statusBarCornerHas(CORNER_PROGRESS_PERCENT);
  // Display → Battery Show/Hide is master: off means no icon and no percent.
  const bool batteryMasterOn = hideBatteryPercentage == HIDE_NEVER;
  spec.showBattery = batteryMasterOn && statusBarCornerHas(CORNER_BATTERY);
  spec.batteryDisplay = readerBatteryDisplay < BATTERY_DISPLAY_MODE_COUNT
                            ? readerBatteryDisplay
                            : static_cast<uint8_t>(BATTERY_DISPLAY_ICON_PERCENT);
  spec.clock12h = clockFormat == 1;
  spec.clockUtcOffsetQ = clockUtcOffsetQ;
  spec.progressBarMode = statusBarProgressBar;
  spec.progressBarHeightPx =
      statusBarProgressBar != HIDE_PROGRESS ? static_cast<uint8_t>((statusBarProgressBarThickness + 1) * 2) : 0;
  spec.xtcMode = xtcStatusBarMode;
  spec.wantsTimeLeftBook = statusBarCornerHas(CORNER_TIME_LEFT_BOOK);
  spec.wantsTimeLeftChapter = statusBarCornerHas(CORNER_TIME_LEFT_CHAPTER);
  spec.wantsBookTitle = statusBarCornerHas(CORNER_BOOK_TITLE);
  spec.wantsChapterTitle = statusBarCornerHas(CORNER_CHAPTER_TITLE);
  return spec;
}

ReaderRenderSpec CrossPointSettings::readerRenderSpec(const uint16_t viewportWidth,
                                                      const uint16_t viewportHeight) const {
  ReaderRenderSpec spec;
  spec.fontId = getReaderFontId();
  spec.lineCompression = getReaderLineCompression();
  spec.extraParagraphSpacing = extraParagraphSpacing != 0;
  spec.paragraphAlignment = paragraphAlignment;
  spec.viewportWidth = viewportWidth;
  spec.viewportHeight = viewportHeight;
  spec.hyphenationEnabled = hyphenationEnabled != 0;
  spec.embeddedStyle = embeddedStyle != 0;
  spec.imageRendering = imageRendering;
  spec.focusReadingEnabled = focusReadingEnabled != 0;
  spec.guideReadingEnabled = guideReadingEnabled != 0;
  return spec;
}

float CrossPointSettings::getReaderLineCompression() const {
  // SD card fonts use same compression as Bookerly (the most neutral values)
  if (sdFontFamilyName[0] != '\0') {
    switch (lineSpacing) {
      case TIGHT:
        return 0.95f;
      case NORMAL:
      default:
        return 1.0f;
      case WIDE:
        return 1.1f;
    }
  }

  switch (fontFamily) {
    case BITTER:
    case SOURCESERIF4:
    default:
      switch (lineSpacing) {
        case TIGHT:
          return 0.95f;
        case NORMAL:
        default:
          return 1.0f;
        case WIDE:
          return 1.1f;
      }
  }
}

unsigned long CrossPointSettings::getSleepTimeoutMs() const {
  if (sleepTimeoutMinutes >= SLEEP_TIMEOUT_NEVER_MINUTES) return 0UL;
  const uint8_t minutes =
      std::clamp(sleepTimeoutMinutes, MIN_SLEEP_TIMEOUT_MINUTES, static_cast<uint8_t>(SLEEP_TIMEOUT_NEVER_MINUTES - 1));
  return static_cast<unsigned long>(minutes) * 60UL * 1000UL;
}

int CrossPointSettings::getRefreshFrequency() const {
  switch (refreshFrequency) {
    case REFRESH_1:
      return 1;
    case REFRESH_5:
      return 5;
    case REFRESH_10:
      return 10;
    case REFRESH_15:
    default:
      return 15;
    case REFRESH_30:
      return 30;
  }
}

int CrossPointSettings::getReaderFontId() const {
  // Check SD card font first
  if (sdFontFamilyName[0] != '\0' && sdFontIdResolver) {
    int id = sdFontIdResolver(sdFontResolverCtx, sdFontFamilyName, fontSize);
    if (id != 0) return id;
    // Fall through to built-in if SD font not found
  }

  switch (fontFamily) {
    case BITTER:
      switch (fontSize) {
        case SMALL:
          return BITTER_12_FONT_ID;
        case MEDIUM:
        default:
          return BITTER_14_FONT_ID;
        case LARGE:
          return BITTER_16_FONT_ID;
        case EXTRA_LARGE:
          return BITTER_18_FONT_ID;
      }
    case SOURCESERIF4:
    default:
      switch (fontSize) {
        case SMALL:
          return SOURCESERIF4_12_FONT_ID;
        case MEDIUM:
        default:
          return SOURCESERIF4_14_FONT_ID;
        case LARGE:
          return SOURCESERIF4_16_FONT_ID;
        case EXTRA_LARGE:
          return SOURCESERIF4_18_FONT_ID;
      }
  }
}

bool CrossPointSettings::anyDictionaryEnabled() const {
  // Only multi-select list counts. dictionaryName is a legacy mirror of the first
  // enabled pack and is not used alone (avoids dict working after all bubbles cleared).
  const char* p = dictionaryList;
  while (*p) {
    if (*p != '\n' && *p != ' ' && *p != '\t') {
      return true;
    }
    ++p;
  }
  return false;
}

bool CrossPointSettings::isDictionaryEnabled(const char* folderName) const {
  if (!folderName || folderName[0] == '\0') {
    return false;
  }
  const char* p = dictionaryList;
  while (*p) {
    const char* start = p;
    while (*p && *p != '\n') {
      ++p;
    }
    const size_t len = static_cast<size_t>(p - start);
    if (len > 0 && strncmp(start, folderName, len) == 0 && folderName[len] == '\0') {
      return true;
    }
    if (*p == '\n') {
      ++p;
    }
  }
  return false;
}

void CrossPointSettings::getEnabledDictionaries(std::vector<std::string>& out) const {
  out.clear();
  const char* p = dictionaryList;
  while (*p) {
    const char* start = p;
    while (*p && *p != '\n') {
      ++p;
    }
    if (p > start) {
      out.emplace_back(start, static_cast<size_t>(p - start));
    }
    if (*p == '\n') {
      ++p;
    }
  }
}

void CrossPointSettings::setEnabledDictionaries(const std::vector<std::string>& names) {
  dictionaryList[0] = '\0';
  dictionaryName[0] = '\0';
  size_t pos = 0;
  for (size_t i = 0; i < names.size(); ++i) {
    const std::string& n = names[i];
    if (n.empty() || n.size() >= 32) {
      continue;
    }
    // +1 for newline (except we always use newline between entries)
    if (pos + n.size() + 1 >= DICTIONARY_LIST_MAX) {
      break;
    }
    if (pos > 0) {
      dictionaryList[pos++] = '\n';
    }
    memcpy(dictionaryList + pos, n.c_str(), n.size());
    pos += n.size();
    dictionaryList[pos] = '\0';
    if (dictionaryName[0] == '\0') {
      copyToField(dictionaryName, n.c_str(), sizeof(dictionaryName));
    }
  }
}
