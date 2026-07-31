#pragma once
#include <ArduinoJson.h>
#include <Epub/ReaderRenderSpec.h>
#include <PersistableStore.h>

#include <cstdint>
#include <string>
#include <vector>

class CrossPointSettings : public PersistableStore<CrossPointSettings> {
 private:
  // Private constructor for singleton
  CrossPointSettings() = default;

  friend class PersistableStore<CrossPointSettings>;

 public:
  enum SLEEP_SCREEN_MODE {
    DARK = 0,
    LIGHT = 1,
    CUSTOM = 2,
    COVER = 3,
    COVER_CUSTOM = 4,
    BLANK = 5,
    QUICK_RESUME = 6,
    SLEEP_SCREEN_MODE_COUNT
  };
  enum SLEEP_SCREEN_COVER_MODE { FIT = 0, CROP = 1, SLEEP_SCREEN_COVER_MODE_COUNT };
  enum SLEEP_SCREEN_COVER_FILTER {
    NO_FILTER = 0,
    BLACK_AND_WHITE = 1,
    INVERTED_BLACK_AND_WHITE = 2,
    SLEEP_SCREEN_COVER_FILTER_COUNT
  };

  // Display order: Hide, Book, Chapter (matches Customize Reader UI popup).
  enum STATUS_BAR_PROGRESS_BAR {
    HIDE_PROGRESS = 0,
    BOOK_PROGRESS = 1,
    CHAPTER_PROGRESS = 2,
    STATUS_BAR_PROGRESS_BAR_COUNT
  };
  enum STATUS_BAR_PROGRESS_BAR_THICKNESS {
    PROGRESS_BAR_THIN = 0,
    PROGRESS_BAR_NORMAL = 1,
    PROGRESS_BAR_THICK = 2,
    STATUS_BAR_PROGRESS_BAR_THICKNESS_COUNT
  };
  enum STATUS_BAR_TITLE { BOOK_TITLE = 0, CHAPTER_TITLE = 1, HIDE_TITLE = 2, STATUS_BAR_TITLE_COUNT };
  enum XTC_STATUS_BAR_MODE {
    XTC_STATUS_BAR_HIDE = 0,
    XTC_STATUS_BAR_BOTTOM = 1,
    XTC_STATUS_BAR_TOP = 2,
    XTC_STATUS_BAR_MODE_COUNT
  };

  // Clock show/hide (legacy systemClock field; placement uses system status slots).
  enum STATUS_BAR_CLOCK_MODE {
    STATUS_BAR_CLOCK_HIDE = 0,
    STATUS_BAR_CLOCK_SHOW = 1,
    // Legacy values from older builds that offered Left/Right placement.
    STATUS_BAR_CLOCK_RIGHT = STATUS_BAR_CLOCK_SHOW,
    STATUS_BAR_CLOCK_LEFT = 2,
  };

  // System top chrome (home/settings headers): Left / Middle / Right slots.
  // Battery and Clock may each appear in at most one slot.
  enum SYSTEM_STATUS_SLOT {
    SYS_SLOT_HIDE = 0,
    SYS_SLOT_BATTERY = 1,
    SYS_SLOT_CLOCK = 2,
    SYSTEM_STATUS_SLOT_COUNT
  };

  // How battery is drawn when a Battery slot is placed (system chrome and reader chrome).
  // Independent per context so the reader can stay minimal while the main UI stays full.
  enum BATTERY_DISPLAY_MODE {
    BATTERY_DISPLAY_ICON = 0,          // icon only
    BATTERY_DISPLAY_PERCENT = 1,       // "NN%" text only
    BATTERY_DISPLAY_ICON_PERCENT = 2,  // icon + percent (default when battery is enabled)
    BATTERY_DISPLAY_MODE_COUNT
  };

  // Reader time-left estimate (legacy single setting; also derived from corner slots).
  enum STATUS_BAR_TIME_LEFT {
    TIME_LEFT_HIDE = 0,
    TIME_LEFT_BOOK = 1,
    TIME_LEFT_CHAPTER = 2,
    STATUS_BAR_TIME_LEFT_COUNT
  };

  // Content placed in one of six status-bar slots (reader chrome).
  // Each non-Hide value may appear in at most one slot.
  enum STATUS_BAR_CORNER_CONTENT {
    CORNER_HIDE = 0,
    CORNER_BATTERY = 1,
    CORNER_CHAPTER_PAGE_COUNTER = 2,  // was CORNER_PAGE_COUNTER; keep value for settings migration
    CORNER_PROGRESS_PERCENT = 3,
    CORNER_TIME_LEFT_BOOK = 4,
    CORNER_TIME_LEFT_CHAPTER = 5,
    CORNER_CLOCK = 6,
    CORNER_BOOK_TITLE = 7,  // was CORNER_TITLE (generic); value kept for migration
    CORNER_BOOK_PAGE_COUNTER = 8,
    CORNER_CHAPTER_COUNTER = 9,   // TOC chapter index, e.g. "Ch. 5/40"
    CORNER_CHAPTER_TITLE = 10,
    // Placement marker for XTC books: upper slot → top overlay, lower → bottom; absent → hide.
    // Does not paint chrome text; only drives xtcStatusBarMode.
    CORNER_XTC_STATUS_BAR = 11,
    STATUS_BAR_CORNER_CONTENT_COUNT
  };
  // Legacy aliases.
  static constexpr uint8_t CORNER_PAGE_COUNTER = CORNER_CHAPTER_PAGE_COUNTER;
  static constexpr uint8_t CORNER_TITLE = CORNER_BOOK_TITLE;

  enum ORIENTATION {
    PORTRAIT = 0,       // 480x800 logical coordinates (current default)
    LANDSCAPE_CW = 1,   // 800x480 logical coordinates, rotated 180° (swap top/bottom)
    INVERTED = 2,       // 480x800 logical coordinates, inverted
    LANDSCAPE_CCW = 3,  // 800x480 logical coordinates, native panel orientation
    ORIENTATION_COUNT
  };

  // Front button layout options (legacy)
  // Default: Back, Confirm, Left, Right
  // Swapped: Left, Right, Back, Confirm
  enum FRONT_BUTTON_LAYOUT {
    BACK_CONFIRM_LEFT_RIGHT = 0,
    LEFT_RIGHT_BACK_CONFIRM = 1,
    LEFT_BACK_CONFIRM_RIGHT = 2,
    BACK_CONFIRM_RIGHT_LEFT = 3,
    FRONT_BUTTON_LAYOUT_COUNT
  };

  // Front button hardware identifiers (for remapping)
  enum FRONT_BUTTON_HARDWARE {
    FRONT_HW_BACK = 0,
    FRONT_HW_CONFIRM = 1,
    FRONT_HW_LEFT = 2,
    FRONT_HW_RIGHT = 3,
    FRONT_BUTTON_HARDWARE_COUNT
  };

  // Physical buttons that can be remapped (front 0–3 + side Up/Down 4–5). Power is excluded.
  static constexpr uint8_t HW_REMAP_BUTTON_COUNT = 6;

  // Function assigned to a physical button (hwButtonFunction[hwIndex]).
  // Values 0–5 match the classic logical roles; NONE disables the key.
  enum BUTTON_FUNCTION {
    BTN_FUNC_BACK = 0,
    BTN_FUNC_CONFIRM = 1,
    BTN_FUNC_LEFT = 2,
    BTN_FUNC_RIGHT = 3,
    BTN_FUNC_UP = 4,
    BTN_FUNC_DOWN = 5,
    BTN_FUNC_NONE = 6,
    BTN_FUNC_COUNT
  };

  // Side button layout options
  // Default: Up = Previous, Down = Next
  enum SIDE_BUTTON_LAYOUT { PREV_NEXT = 0, NEXT_PREV = 1, SIDE_BUTTONS_DISABLED = 2, SIDE_BUTTON_LAYOUT_COUNT };

  // Built-in fonts only (SD card fonts use sdFontFamilyName).
  // 0 Source Serif 4 (default UI + reader), 1 Bitter.
  // Older IDs (Lexend=0, Bitter=1, Source=2, Literata=3) remapped in fromJson.
  enum FONT_FAMILY {
    SOURCESERIF4 = 0,
    BITTER = 1,
    FONT_FAMILY_COUNT
  };
  // Legacy aliases (older code / migrations).
  static constexpr uint8_t NOTOSERIF = SOURCESERIF4;
  static constexpr uint8_t NOTOSANS = BITTER;
  static constexpr uint8_t LEXENDDECA = SOURCESERIF4;  // removed from flash; alias → Source Serif
  static constexpr uint8_t LITERATA = SOURCESERIF4;    // removed from flash; alias → Source Serif
  static constexpr uint8_t LEGACY_OPENDYSLEXIC = 2;
  static constexpr uint8_t BUILTIN_FONT_COUNT = FONT_FAMILY_COUNT;
  // Font size options
  enum FONT_SIZE { SMALL = 0, MEDIUM = 1, LARGE = 2, EXTRA_LARGE = 3, FONT_SIZE_COUNT };
  enum LINE_COMPRESSION { TIGHT = 0, NORMAL = 1, WIDE = 2, LINE_COMPRESSION_COUNT };
  enum PARAGRAPH_ALIGNMENT {
    JUSTIFIED = 0,
    LEFT_ALIGN = 1,
    CENTER_ALIGN = 2,
    RIGHT_ALIGN = 3,
    BOOK_STYLE = 4,
    PARAGRAPH_ALIGNMENT_COUNT
  };

  // Auto-sleep timeout options (in minutes)
  enum SLEEP_TIMEOUT {
    SLEEP_1_MIN = 0,
    SLEEP_5_MIN = 1,
    SLEEP_10_MIN = 2,
    SLEEP_15_MIN = 3,
    SLEEP_30_MIN = 4,
    SLEEP_TIMEOUT_COUNT
  };

  // E-ink refresh frequency (pages between full refreshes)
  enum REFRESH_FREQUENCY {
    REFRESH_1 = 0,
    REFRESH_5 = 1,
    REFRESH_10 = 2,
    REFRESH_15 = 3,
    REFRESH_30 = 4,
    REFRESH_FREQUENCY_COUNT
  };

  // Short power button press actions
  enum SHORT_PWRBTN { IGNORE = 0, SLEEP = 1, PAGE_TURN = 2, FORCE_REFRESH = 3, FOOTNOTES = 4, SHORT_PWRBTN_COUNT };

  // Long-press Confirm action while reading an EPUB. The setting cycles through these values.
  // Persisted in settings.json by index: any new function (e.g. dictionary, bookmark) MUST use a
  // value >= 2 and be appended at the END of the enumValues array in SettingsList.h, otherwise the
  // stored indices shift and existing saves are silently misinterpreted.
  // Append-only: new values go at the end so existing settings.json indices stay valid.
  enum LONG_PRESS_MENU_FUNCTION {
    LP_MENU_KOSYNC = 0,
    LP_MENU_DISABLED = 1,
    LP_MENU_BOOKMARK = 2,
    LP_MENU_DICTIONARY = 3,
    LP_MENU_SLEEP = 4,
    LP_MENU_FORCE_REFRESH = 5,
    LP_MENU_FILE_BROWSER = 6,
    // CrossInk-parity actions (append-only after existing 0-6)
    LP_MENU_SCREENSHOT = 7,
    LP_MENU_FOOTNOTES = 8,
    LP_MENU_FILE_TRANSFER = 9,
    LP_MENU_READING_STATS = 10,
    LONG_PRESS_MENU_FUNCTION_COUNT
  };

  // Hide battery percentage
  enum HIDE_BATTERY_PERCENTAGE { HIDE_NEVER = 0, HIDE_READER = 1, HIDE_ALWAYS = 2, HIDE_BATTERY_PERCENTAGE_COUNT };

  // Page turn button long press behavior
  enum LONG_PRESS_BUTTON_BEHAVIOR {
    OFF = 0,
    CHAPTER_SKIP = 1,
    ORIENTATION_CHANGE = 2,
    LONG_PRESS_BUTTON_BEHAVIOR_COUNT
  };

  // UI Theme (append-only — existing saved values must keep their numbers).
  // Display names (english.yaml): Bare · Stats · Spectral (X3).
  enum UI_THEME {
    CLASSIC = 0,
    LYRA = 1,
    LYRA_3_COVERS = 2,
    ROUNDEDRAFF = 3,
    MINIMAL = 4,  // removed from picker; remapped to STATS on load
    // Stats-Life: merged into STATS (side L/R toggles lifetime under-box).
    STATS_LIFE = 5,
    LYRA_CAROUSEL = 6,  // removed from picker; remapped to STATS on load
    // Legacy A/B theme ids (no longer in the picker; remapped to STATS on load).
    DASHBOARD_MAGAZINE = 7,
    DASHBOARD_CARD = 8,
    BARE = 9,  // cover + title + author home
    // Parked (not in picker; remapped → STATS on load). Restore notes:
    // dist/theme-backup-shelf-scroll/README.md
    DASHBOARD_RECENTS = 10,  // was "Shelf"
    DASHBOARD_SCROLL = 11,   // was "Stats Scroll"
    // Stats: cover + book stats; side L/R toggles title ↔ lifetime (was FOCUS / Stats-Life).
    STATS = 12,
    // Spectral (was Clockface): X3-only large home clock + under-panel.
    // JSON value 13 stable. Hidden on X4 picker; remapped to BARE on non-X3.
    SPECTRAL = 13,
    CLOCKFACE = SPECTRAL,  // legacy name
    // Ghost: parked (enum kept for JSON stability; remapped to BARE on load).
    GHOST = 14,
  };

  // Spectral home side-button actions (X3 Left/Right, X4 Up/Down via side map).
  // Both sides same action → bidirectional (Left back / Right forward).
  // Only one side set → one-way cycle (Title→Stats→Lifetime, or newest→oldest).
  enum SPECTRAL_SIDE_ACTION : uint8_t {
    SPECTRAL_SIDE_RECENTS = 0,       // recent books (up to 4)
    SPECTRAL_SIDE_PANEL = 1,         // Title / Stats / Lifetime under-panel
    SPECTRAL_SIDE_ACTION_COUNT = 2
  };

  // Image rendering in EPUB reader
  enum IMAGE_RENDERING { IMAGES_DISPLAY = 0, IMAGES_PLACEHOLDER = 1, IMAGES_SUPPRESS = 2, IMAGE_RENDERING_COUNT };

  enum TILT_PAGE_TURN { TILT_OFF = 0, TILT_NORMAL = 1, TILT_NVERTED = 2, TILT_PAGE_TURN_COUNT };

  enum TOUCH_READER_CONTROLS { TOUCH_READER_OFF = 0, TOUCH_READER_ON = 1, TOUCH_READER_CONTROLS_COUNT };

  enum QUICK_RESUME_SLEEP_SCREEN {
    QUICK_RESUME_NEVER = 0,
    QUICK_RESUME_AFTER_TIMEOUT = 1,
    QUICK_RESUME_SLEEP_SCREEN_COUNT
  };

  // Sleep screen settings (Casper default: light logo wallpaper)
  uint8_t sleepScreen = LIGHT;
  // Sleep screen cover mode settings
  uint8_t sleepScreenCoverMode = FIT;
  // Sleep screen cover filter
  uint8_t sleepScreenCoverFilter = NO_FILTER;
  // Status bar settings
  // Legacy toggles kept for migration / web API; live chrome uses corner slots below.
  uint8_t statusBarChapterPageCount = 1;
  uint8_t statusBarBookProgressPercentage = 1;
  uint8_t statusBarProgressBar = BOOK_PROGRESS;  // default: book progress bar on
  uint8_t statusBarProgressBarThickness = PROGRESS_BAR_THIN;
  uint8_t statusBarTitle = CHAPTER_TITLE;
  uint8_t statusBarBattery = 1;
  uint8_t xtcStatusBarMode = XTC_STATUS_BAR_HIDE;
  // Six reader chrome slots: UL / UM / UR / LL / LM / LR.
  // Factory defaults for Customize Reader UI (six slots + progress bar).
  uint8_t statusBarUpperLeft = CORNER_BATTERY;
  uint8_t statusBarUpperMiddle = CORNER_CLOCK;
  uint8_t statusBarUpperRight = CORNER_PROGRESS_PERCENT;
  uint8_t statusBarLowerLeft = CORNER_TIME_LEFT_CHAPTER;
  uint8_t statusBarLowerMiddle = CORNER_CHAPTER_TITLE;
  uint8_t statusBarLowerRight = CORNER_CHAPTER_PAGE_COUNTER;
  // Legacy reader clock show/hide (migrated into CORNER_CLOCK slot; kept for JSON/web).
  uint8_t statusBarClock = STATUS_BAR_CLOCK_SHOW;
  // System top chrome slots (Display → Status Bar). Stats default: battery left, clock right.
  uint8_t systemStatusBarLeft = SYS_SLOT_BATTERY;
  uint8_t systemStatusBarMiddle = SYS_SLOT_HIDE;
  uint8_t systemStatusBarRight = SYS_SLOT_CLOCK;
  // When Battery is placed on the system bar: Icon / Percent / Icon + Percent.
  uint8_t systemBatteryDisplay = BATTERY_DISPLAY_ICON_PERCENT;
  // When Battery is placed in reader chrome (Customize Reader UI): same modes, independent.
  uint8_t readerBatteryDisplay = BATTERY_DISPLAY_ICON_PERCENT;
  // Derived from system status slots (synced on assign); kept for JSON/web + older readers.
  uint8_t systemClock = STATUS_BAR_CLOCK_SHOW;
  // Legacy single time-left mode (synced from corners when possible).
  uint8_t statusBarTimeLeft = TIME_LEFT_BOOK;
  // Clock UTC offset in quarter-hour steps, biased by 48 so it fits in uint8_t.
  // Value 48 = UTC+0, 0 = UTC-12:00, 104 = UTC+14:00.
  // Quarter-hour granularity supports oddball zones like Nepal (+5:45) and Chatham (+12:45).
  // Factory default UTC-7 (48 = UTC+0 → 48 + (-7)*4 = 20).
  uint8_t clockUtcOffsetQ = 20;
  // Clock display format: 0 = 24-hour, 1 = 12-hour (factory default 12h).
  uint8_t clockFormat = 1;
  // Set once an NTP sync succeeds. Used to skip re-syncing on every WiFi connect.
  // Resetting to 0 (e.g. via the web UI) forces a re-sync on next WiFi connect.
  uint8_t clockHasBeenSynced = 0;
  // Text rendering settings
  uint8_t extraParagraphSpacing = 1;
  // Off by default: AA greys are the main open/page-turn cost on e-ink.
  uint8_t textAntiAliasing = 0;
  // Short power button click behaviour (Casper: sleep)
  uint8_t shortPwrBtn = SLEEP;
  // Long power button press (held past getPowerButtonLongPressDuration); same SHORT_PWRBTN values.
  // Casper default: force full refresh (short remains sleep).
  uint8_t longPwrBtn = FORCE_REFRESH;
  // EPUB reading orientation settings
  // 0 = portrait (default), 1 = landscape clockwise, 2 = inverted, 3 = landscape counter-clockwise
  uint8_t orientation = PORTRAIT;
  // Button layouts (front layout retained for migration only)
  uint8_t frontButtonLayout = BACK_CONFIRM_LEFT_RIGHT;
  uint8_t sideButtonLayout = PREV_NEXT;
  uint8_t frontButtonFollowOrientation = 0;
  // Front button remap (logical -> hardware) — legacy fields kept for migration / older tools.
  // Prefer hwButtonFunction[] (physical slot → function).
  uint8_t frontButtonBack = FRONT_HW_BACK;
  uint8_t frontButtonConfirm = FRONT_HW_CONFIRM;
  uint8_t frontButtonLeft = FRONT_HW_LEFT;
  uint8_t frontButtonRight = FRONT_HW_RIGHT;
  // Physical button → function. Index: 0–3 front L→R, 4 = side left (X3) / upper (X4),
  // 5 = side right (X3) / lower (X4).
  // Default axes match names: front 3rd/4th = Up/Down (vertical list), sides = Left/Right
  // (horizontal tabs). Reader page turn accepts both pairs.
  uint8_t hwButtonFunction[HW_REMAP_BUTTON_COUNT] = {
      BTN_FUNC_BACK, BTN_FUNC_CONFIRM, BTN_FUNC_UP, BTN_FUNC_DOWN, BTN_FUNC_LEFT, BTN_FUNC_RIGHT};
  // Reader font settings
  uint8_t fontFamily = SOURCESERIF4;
  uint8_t fontSize = SMALL;  // 12 pt Lexend Deca (Casper default)
  uint8_t lineSpacing = NORMAL;
  uint8_t paragraphAlignment = LEFT_ALIGN;
  // Auto-sleep timeout setting (default 10 minutes). Legacy sleepTimeout enum values are migration-only.
  uint8_t sleepTimeoutMinutes = 10;
  // E-ink refresh frequency (default 15 pages)
  uint8_t refreshFrequency = REFRESH_15;
  uint8_t hyphenationEnabled = 0;

  // Reader screen margin settings
  static constexpr uint8_t SCREEN_MARGIN_MIN = 5;
  static constexpr uint8_t SCREEN_MARGIN_MAX = 40;
  static constexpr uint8_t SCREEN_MARGIN_STEP = 5;
  // Casper default: 10 px side margin (MIN=5, step=5).
  uint8_t screenMargin = 10;
  // OPDS download destination folder ("" = SD root). Global; edited from the
  // OPDS server list. Persisted via a category-less SettingInfo::String in
  // SettingsList.h, so it stays out of the on-device Settings screen.
  char opdsDownloadFolder[64] = "";
  // On-disk filename format for OPDS downloads (0=Author-Title default, 1=Title-Author,
  // 2=Title). See OpdsFilenameFormat. Persisted via a category-less SettingInfo::Enum,
  // edited from the OPDS server list; hidden from the on-device Settings screen.
  uint8_t opdsFilenameFormat = 0;
  // Hide battery percentage
  uint8_t hideBatteryPercentage = HIDE_NEVER;
  // Long-press page turn button behavior
  uint8_t longPressButtonBehavior = OFF;
  // Long-press Confirm function in EPUB reader (cycles through LONG_PRESS_MENU_FUNCTION values).
  // Casper: open stock CrossPoint dictionary (not a custom dictionary stack).
  uint8_t longPressMenuFunction = LP_MENU_DICTIONARY;
  // UI Theme. Compile-time default Bare (typical X4). loadFromFile() sets
  // first-boot defaults: X4 → Bare, X3 → Stats (see casperHomeMigrated).
  uint8_t uiTheme = BARE;
  // Spectral theme side buttons (X3 Left/Right; labels say Up/Down on X4 if offered).
  // Defaults: both Panel Scroll (Left = back, Right = forward through Title/Stats/Lifetime).
  uint8_t spectralSideLeft = SPECTRAL_SIDE_PANEL;
  uint8_t spectralSideRight = SPECTRAL_SIDE_PANEL;
  // One-time migration: force Casper home chrome defaults once.
  // Users can still change theme in Settings afterwards.
  uint8_t casperHomeMigrated = 0;
  // One-time migration: force short=Sleep / long=ForceRefresh / long-press menu=Dictionary.
  uint8_t casperControlsMigrated = 0;
  // One-time migration: factory clock defaults (12h, UTC-7) + keep status-bar clock shown.
  uint8_t casperClockDefaultsMigrated = 0;
  // One-time migration: map legacy battery/%/page/time-left toggles into four corners.
  uint8_t casperStatusBarCornersMigrated = 0;
  // One-time: expand 4 corners + clock/title toggles into 6 exclusive slots.
  uint8_t casperStatusBarSixSlotsMigrated = 0;
  // One-time: generic title slot (value 7) → Book Title or Chapter Title.
  uint8_t casperStatusBarTitleSplitMigrated = 0;
  // One-time: progress bar enum reorder (Book/Chapter/Hide → Hide/Book/Chapter).
  uint8_t casperProgressBarOrderMigrated = 0;
  // One-time: map hideBatteryPercentage + systemClock into Left/Middle/Right system slots.
  uint8_t casperSystemStatusBarMigrated = 0;
  // One-time: legacy fontFamily==2 OpenDyslexic builtin → SD pack.
  uint8_t casperOpendyslexicMigrated = 0;
  // One-time: drop Lexend/Literata builtins; compact enum to Source Serif + Bitter.
  uint8_t casperBuiltinFontsSlimMigrated = 0;
  // One-time: classic Left/Right list axes → Up/Down list + Left/Right sides.
  uint8_t casperButtonAxisMigrated = 0;
  // One-time: text AA + embedded style off (faster open / page turn defaults).
  uint8_t casperSpeedDefaultsMigrated = 0;
  // Sunlight fading compensation
  uint8_t fadingFix = 0;
  // Power button return from footnotes (1 = enabled, 0 = disabled)
  uint8_t pwrBtnFootnoteBack = 1;
  // Book CSS: off by default (simpler layout, faster cold index). Users can re-enable.
  uint8_t embeddedStyle = 0;
  // Bionic Reading (UI label; JSON key remains focusReadingEnabled for migration).
  // Bolds the first portion of each word — CrossInk calls this Bionic Reading;
  // upstream CrossPoint called it Focus Reading.
  uint8_t focusReadingEnabled = 0;
  // Guide Dots (CrossInk): middle-dot between words to guide the eye.
  uint8_t guideReadingEnabled = 0;
  // SD card font family name (empty = use built-in fontFamily)
  char sdFontFamilyName[32] = "";
  // Legacy single dictionary folder under /dictionaries (empty = none). Kept for
  // migration and as the first enabled name; multi-select uses dictionaryList.
  char dictionaryName[32] = "";
  // Multi-select: newline-separated folder names (e.g. "English\nSpanish-English\n").
  // Empty with empty dictionaryName = no dictionary. Max ~5 × 31-char names.
  static constexpr size_t DICTIONARY_LIST_MAX = 160;
  char dictionaryList[DICTIONARY_LIST_MAX] = "";

  // Multi-dict helpers (folder names under /dictionaries).
  bool anyDictionaryEnabled() const;
  bool isDictionaryEnabled(const char* folderName) const;
  void getEnabledDictionaries(std::vector<std::string>& out) const;
  void setEnabledDictionaries(const std::vector<std::string>& names);
  // Show hidden files/directories (starting with '.') in the file browser (0 = hidden, 1 = show)
  uint8_t showHiddenFiles = 0;
  // Remove a book from the Recent Books list when its End-of-Book screen is reached (0 = off, 1 = on)
  uint8_t removeReadBooksFromRecents = 0;
  // Move epub to /Read/ folder on SD card when finished (0 = disabled, 1 = enabled)
  uint8_t moveFinishedToReadFolder = 0;
  // X3 only: before deep sleep, copy lifetime stats to /.casper-stats-backup/
  // (one dated file per calendar day when RTC is available). Default on.
  uint8_t autoBackupStats = 1;
  // Idle gap (minutes) for reading-stats sessions / pace samples. Time on a page
  // longer than this is treated as idle and not counted toward reading time or pace.
  // Default 5 minutes (CrossInk-style "session time" threshold).
  uint8_t readingSessionIdleMinutes = 5;
  // When 1: record reading stats (default). When 0: no tracking; hide Reading Stats UI.
  // Existing on-disk stats are left alone (not deleted).
  uint8_t readingStatsEnabled = 1;
  bool readingStatsTrackingEnabled() const { return readingStatsEnabled != 0; }
  static constexpr uint8_t MIN_SESSION_IDLE_MINUTES = 1;
  static constexpr uint8_t MAX_SESSION_IDLE_MINUTES = 30;
  // Short press Back goes to file browser instead of home (0 = disabled, 1 = enabled)
  uint8_t backShortToFileBrowser = 0;
  // Image rendering mode in EPUB reader
  uint8_t imageRendering = IMAGES_DISPLAY;
  // Tilt-based page turning (X3 only — requires QMI8658 IMU)
  uint8_t tiltPageTurn = TILT_OFF;
  // Touch screen reader zones/gestures on boards with a touch controller.
  uint8_t touchReaderControls = TOUCH_READER_ON;
  // Language setting (Language enum index, default 0 = EN)
  uint8_t language = 0;
  // Quick Resume: keep current content visible with moon icon instead of showing a static sleep screen.
  uint8_t quickResumeSleepScreen = QUICK_RESUME_NEVER;

  static constexpr uint8_t MIN_SLEEP_TIMEOUT_MINUTES = 1;
  static constexpr uint8_t SLEEP_TIMEOUT_NEVER_MINUTES = 31;
  static constexpr uint8_t MAX_SLEEP_TIMEOUT_MINUTES = SLEEP_TIMEOUT_NEVER_MINUTES;

  // Callback to resolve SD card font IDs. Set by SdCardFontSystem::begin().
  // Returns font ID or 0 if not found.
  using SdFontIdResolver = int (*)(void* ctx, const char* familyName, uint8_t fontSize);
  SdFontIdResolver sdFontIdResolver = nullptr;
  void* sdFontResolverCtx = nullptr;

  static constexpr uint16_t POWER_BUTTON_WAKE_SHORT_MS = 10;
  static constexpr uint16_t POWER_BUTTON_WAKE_LONG_MS = 200;
  static constexpr uint16_t POWER_BUTTON_LONG_PRESS_MS = 500;

  // Short-press / wake verification duration. Stays short when short action is SLEEP
  // so a quick tap wakes and re-sleeps correctly; longer otherwise.
  uint16_t getPowerButtonDuration() const {
    return (shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP) ? POWER_BUTTON_WAKE_SHORT_MS
                                                                   : POWER_BUTTON_WAKE_LONG_MS;
  }
  uint16_t getPowerButtonWakeDuration() const { return getPowerButtonDuration(); }
  // Hold threshold that distinguishes short vs long power press (~400-600 ms).
  uint16_t getPowerButtonLongPressDuration() const { return POWER_BUTTON_LONG_PRESS_MS; }
  int getReaderFontId() const;

  // Session idle threshold in seconds (for pace samples and stats accumulation).
  uint32_t getReadingSessionIdleSeconds() const {
    uint8_t minutes = readingSessionIdleMinutes;
    if (minutes < MIN_SESSION_IDLE_MINUTES) minutes = MIN_SESSION_IDLE_MINUTES;
    if (minutes > MAX_SESSION_IDLE_MINUTES) minutes = MAX_SESSION_IDLE_MINUTES;
    return static_cast<uint32_t>(minutes) * 60u;
  }

  // Resolved status-bar composition. Consumers read the spec; only settings
  // editors read the raw fields.
  //
  // Deliberately NOT built under storeMutex: every field it reads is a single
  // byte, so a concurrent settings write can never produce a corrupt value —
  // only a snapshot mixing pre- and post-change fields. That costs at most one
  // e-ink frame drawn with a mixed status bar, which self-corrects on the next
  // refresh. Locking here would instead put a mutex on the render path and
  // stall it behind the SD write inside saveToFile(). Don't add one back.
  struct StatusBarSpec {
    uint8_t upperLeft = CORNER_HIDE;
    uint8_t upperMiddle = CORNER_HIDE;
    uint8_t upperRight = CORNER_HIDE;
    uint8_t lowerLeft = CORNER_HIDE;
    uint8_t lowerMiddle = CORNER_HIDE;
    uint8_t lowerRight = CORNER_HIDE;
    bool showChapterPageCount = false;
    bool showBookPageCount = false;
    bool showBookProgressPercent = false;
    bool showBattery = false;
    // BATTERY_DISPLAY_MODE for reader chrome (ignored when showBattery is false).
    uint8_t batteryDisplay = BATTERY_DISPLAY_ICON_PERCENT;
    bool showsBatteryIcon() const {
      return showBattery && batteryDisplay != BATTERY_DISPLAY_PERCENT;
    }
    bool showsBatteryPercent() const {
      return showBattery && batteryDisplay != BATTERY_DISPLAY_ICON;
    }
    bool clock12h = false;
    uint8_t clockUtcOffsetQ = 48;             // 48 = UTC+0
    uint8_t progressBarMode = HIDE_PROGRESS;  // STATUS_BAR_PROGRESS_BAR
    uint8_t progressBarHeightPx = 0;          // (thickness+1)*2; 0 when the bar is hidden
    uint8_t xtcMode = XTC_STATUS_BAR_HIDE;    // XTC_STATUS_BAR_MODE
    bool wantsTimeLeftBook = false;
    bool wantsTimeLeftChapter = false;
    bool wantsBookTitle = false;
    bool wantsChapterTitle = false;

    bool showsProgressBar() const { return progressBarMode != HIDE_PROGRESS; }
    bool showsTitle() const { return wantsBookTitle || wantsChapterTitle; }
    bool showsBookTitle() const { return wantsBookTitle; }
    bool showsChapterTitle() const { return wantsChapterTitle; }
    bool showsClock() const { return upperLeft == CORNER_CLOCK || upperMiddle == CORNER_CLOCK ||
                                     upperRight == CORNER_CLOCK || lowerLeft == CORNER_CLOCK ||
                                     lowerMiddle == CORNER_CLOCK || lowerRight == CORNER_CLOCK; }
    bool hasLowerContent() const {
      return lowerLeft != CORNER_HIDE || lowerMiddle != CORNER_HIDE || lowerRight != CORNER_HIDE;
    }
    // Bottom text lane only. Upper slots live in top chrome and must not reserve bottom height.
    // clockAvailable is kept for call-site compatibility.
    bool textLaneVisible(bool clockAvailable) const {
      (void)clockAvailable;
      return hasLowerContent();
    }
  };
  StatusBarSpec statusBarSpec() const;
  // Assign a slot content type; clears that type from any other slot (exclusive).
  void assignStatusBarCorner(uint8_t& cornerField, uint8_t content);
  bool statusBarCornerHas(uint8_t content) const;
  // Derive xtcStatusBarMode from which row holds CORNER_XTC_STATUS_BAR (if any).
  void syncXtcStatusBarModeFromSlots();

  // System top chrome (Display → Status Bar): exclusive Battery/Clock placement.
  bool systemStatusBarHas(uint8_t content) const;
  void assignSystemStatusBarSlot(uint8_t& slotField, uint8_t content);
  // Keep hideBatteryPercentage + systemClock in sync with the three slots.
  void syncSystemStatusLegacyFromSlots();
  // Spectral draws a large home clock — system status bar clock placement is disabled.
  bool systemStatusBarAllowsClock() const;
  // Clear any SYS_SLOT_CLOCK placement (used when switching to Clockface).
  void stripSystemStatusBarClock();

  // Resolved text-rendering configuration for the Epub layout engine. The
  // viewport is renderer/orientation-derived, so the caller supplies it —
  // passing it in keeps a spec from ever existing in a half-filled state.
  // Unlocked for the same reason as statusBarSpec(); see the note above.
  ReaderRenderSpec readerRenderSpec(uint16_t viewportWidth, uint16_t viewportHeight) const;

  static const char* getFilePath() { return "/.crosspoint/settings.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  static void validateFrontButtonMapping(CrossPointSettings& settings);
  // True if map has Back, Confirm, and at least one Prev (Left|Up) and Next (Right|Down).
  static bool isButtonFunctionMapValid(const uint8_t* map, uint8_t count = HW_REMAP_BUTTON_COUNT);
  static void setDefaultButtonFunctionMap(uint8_t* map, uint8_t count = HW_REMAP_BUTTON_COUNT);
  void applyButtonFunctionMap(const uint8_t* map);
  void syncLegacyFrontButtonsFromHwMap();
  // Sidecar file — survives settings.json parse quirks / partial saves across sleep.
  static const char* buttonMapSidecarPath() { return "/.crosspoint/button_map.txt"; }
  bool saveButtonMapSidecar() const;
  bool loadButtonMapSidecar();
  static uint8_t sleepTimeoutEnumToMinutes(uint8_t legacyValue);

  float getReaderLineCompression() const;
  unsigned long getSleepTimeoutMs() const;
  int getRefreshFrequency() const;
};

// Helper macro to access settings
#define SETTINGS CrossPointSettings::getInstance()
