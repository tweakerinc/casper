#pragma once
#include <ArduinoJson.h>
#include <Epub/ReaderRenderSpec.h>
#include <PersistableStore.h>

#include <cstdint>
#include <string>
#include <vector>

class CasperSettings : public PersistableStore<CasperSettings> {
 private:
  // Private constructor for singleton
  CasperSettings() = default;

  friend class PersistableStore<CasperSettings>;

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
  // Battery, Clock, and Battery Warning may each appear in at most one slot.
  enum SYSTEM_STATUS_SLOT {
    SYS_SLOT_HIDE = 0,
    SYS_SLOT_BATTERY = 1,
    SYS_SLOT_CLOCK = 2,
    SYS_SLOT_BATTERY_WARNING = 3,  // "Battery N% · Charge Soon" when SoC ≤ threshold
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

  // Low-battery message threshold (Display → Status Bar → nest under Battery Warning slot).
  // When SoC ≤ threshold, shows "Battery N% · Charge Soon" in the slot that holds
  // SYS_SLOT_BATTERY_WARNING (default Middle on Bare/Penumbra).
  enum BATTERY_WARNING {
    BATTERY_WARNING_OFF = 0,
    BATTERY_WARNING_5 = 1,
    BATTERY_WARNING_10 = 2,
    BATTERY_WARNING_15 = 3,
    BATTERY_WARNING_20 = 4,
    BATTERY_WARNING_25 = 5,
    BATTERY_WARNING_COUNT
  };

  // SD system performance log (System → System Log). See util/SystemLog.
  enum SYSTEM_LOG_LEVEL {
    SYSTEM_LOG_OFF = 0,
    SYSTEM_LOG_TIMING = 1,   // boot, sleep/wake, open, page turn, network, heap
    SYSTEM_LOG_VERBOSE = 2,  // + activity enter/exit and extra detail
    SYSTEM_LOG_LEVEL_COUNT
  };

  // Reader time-left estimate (legacy single setting; also derived from corner slots).
  enum STATUS_BAR_TIME_LEFT {
    TIME_LEFT_HIDE = 0,
    TIME_LEFT_BOOK = 1,
    TIME_LEFT_CHAPTER = 2,
    STATUS_BAR_TIME_LEFT_COUNT
  };

  // Customize Reader UI — chrome text size (clock, battery %, pages, ETAs, titles).
  enum STATUS_BAR_FONT_SIZE {
    STATUS_BAR_FONT_8 = 0,   // Source Serif 8 pt (shipping default)
    STATUS_BAR_FONT_10 = 1,  // Source Serif 10 pt
    STATUS_BAR_FONT_12 = 2,  // Source Serif 12 pt
    STATUS_BAR_FONT_SIZE_COUNT
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
    CORNER_CHAPTER_COUNTER = 9,  // TOC chapter index, e.g. "Ch. 5/40"
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
  // 0 Source Serif 4 (default UI + reader), 1 Literata (OFL serif).
  // Older IDs (Lexend=0, Bitter=1, Source=2, Literata=3) remapped in fromJson.
  enum FONT_FAMILY { SOURCESERIF4 = 0, LITERATA = 1, FONT_FAMILY_COUNT };
  // Legacy aliases (older code / migrations). Slot 1 was Lexend/Bitter; now Literata.
  static constexpr uint8_t NOTOSERIF = SOURCESERIF4;
  static constexpr uint8_t NOTOSANS = LITERATA;
  static constexpr uint8_t LEXENDDECA = LITERATA;
  static constexpr uint8_t BITTER = LITERATA;
  static constexpr uint8_t LEGACY_OPENDYSLEXIC = 2;
  static constexpr uint8_t BUILTIN_FONT_COUNT = FONT_FAMILY_COUNT;
  // Reader body size (pt). Enum order after casperReaderFontSize8Migrated:
  // 0..5 = 8/10/12/14/16/18. Prior post-pt migr: 0..4 = 10/12/14/16/18.
  enum FONT_SIZE {
    SIZE_8 = 0,
    SIZE_10 = 1,
    SIZE_12 = 2,
    SIZE_14 = 3,
    SIZE_16 = 4,
    SIZE_18 = 5,
    FONT_SIZE_COUNT
  };
  // Legacy aliases (pre-6-size enum used these names for 12/14/16/18).
  static constexpr uint8_t SMALL = SIZE_12;
  static constexpr uint8_t MEDIUM = SIZE_14;
  static constexpr uint8_t LARGE = SIZE_16;
  static constexpr uint8_t EXTRA_LARGE = SIZE_18;
  // Shared list / settings menus (Library, Recents activity, Settings rows) — not reader body,
  // not Penumbra home under-panel. Maps to Source Serif 12 / 14 / 16 (labels: 12pt / 14pt / 16pt).
  // Menu list title size (Source Serif bitmaps). Values are stable on-disk.
  // 0=10pt, 1=12pt, 2=14pt, 3=16pt. Pre-10pt firmware used 0=12/1=14/2=16 — migrated on load.
  enum MENU_FONT_SIZE {
    MENU_FONT_XSMALL = 0,  // 10pt
    MENU_FONT_SMALL = 1,   // 12pt
    MENU_FONT_MEDIUM = 2,  // 14pt
    MENU_FONT_LARGE = 3,   // 16pt
    MENU_FONT_SIZE_COUNT
  };
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

  // E-ink page maintenance interval (pages between soft reinforce / scrub).
  // Append-only — saved JSON indices must stay stable.
  enum REFRESH_FREQUENCY {
    REFRESH_1 = 0,
    REFRESH_5 = 1,
    REFRESH_10 = 2,
    REFRESH_15 = 3,
    REFRESH_30 = 4,
    REFRESH_60 = 5,     // YACP-style extended interval
    REFRESH_NEVER = 6,  // FAST only; required cleanups still scrub
    REFRESH_FREQUENCY_COUNT
  };
  // Countdown sentinels used by ReaderUtils::displayWithRefreshCycle.
  static constexpr int REFRESH_COUNTDOWN_DISABLED = -1;
  static constexpr int REFRESH_COUNTDOWN_FORCE_SCRUB = 0;

  // Short/long power button actions (append-only storage indices for settings.json).
  // UI order is remapped in SettingsList::buildPwrBtnSetting (Ignore, Sleep, Quick
  // Resume, Refresh Screen, Page Turn, Footnotes) — do not reorder these values.
  // Note: cannot be named QUICK_RESUME — that enumerator already exists on SLEEP_SCREEN_MODE
  // (legacy value 6) and unscoped enums share the class scope.
  enum SHORT_PWRBTN {
    IGNORE = 0,
    SLEEP = 1,  // wallpaper / Sleep Screen style
    PAGE_TURN = 2,
    FORCE_REFRESH = 3,
    FOOTNOTES = 4,
    PWR_QUICK_RESUME = 5,  // last-frame + fast wake (not a Sleep Screen value)
    SHORT_PWRBTN_COUNT
  };

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
    // legacy-parity actions (append-only after existing 0-6)
    LP_MENU_SCREENSHOT = 7,
    LP_MENU_FOOTNOTES = 8,
    LP_MENU_FILE_TRANSFER = 9,
    LP_MENU_READING_STATS = 10,
    // Append-only: starts Create Clipping (word select) on long-press Confirm.
    LP_MENU_CLIPPINGS = 11,
    LONG_PRESS_MENU_FUNCTION_COUNT
  };

  // Hide battery percentage
  enum HIDE_BATTERY_PERCENTAGE { HIDE_NEVER = 0, HIDE_READER = 1, HIDE_ALWAYS = 2, HIDE_BATTERY_PERCENTAGE_COUNT };

  // Page turn button long press behavior (append-only storage indices).
  enum LONG_PRESS_BUTTON_BEHAVIOR {
    OFF = 0,
    CHAPTER_SKIP = 1,
    ORIENTATION_CHANGE = 2,  // cycle all four orientations
    // Retired: was Clipping Tool on side hold; kept so old settings.json index 3
    // does not collide with a new meaning. Load clamps → OFF.
    LONG_PRESS_BUTTON_BEHAVIOR_RESERVED_3 = 3,
    ORIENTATION_FLIP = 4,  // toggle Portrait ↔ orientationFlipWith
    LONG_PRESS_BUTTON_BEHAVIOR_COUNT
  };

  // UI Theme (append-only — existing saved values must keep their numbers).
  // Display names (english.yaml): Bare · Penumbra (X3 clock / X4 progress).
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
    // Penumbra (was Spectral / Clockface): X3 clock + under-panel; X4 title + progress ring.
    // JSON value 13 stable.
    PENUMBRA = 13,
    SPECTRAL = PENUMBRA,   // legacy name
    CLOCKFACE = PENUMBRA,  // legacy name
    // Ghost: parked (enum kept for JSON stability; remapped to BARE on load).
    GHOST = 14,
  };

  // Penumbra home side-button actions (X3 Left/Right, X4 Up/Down via side map).
  // Both sides same action → bidirectional (Left back / Right forward).
  // Only one side set → one-way cycle (Title→Stats→Lifetime, or newest→oldest).
  enum PENUMBRA_SIDE_ACTION : uint8_t {
    PENUMBRA_SIDE_RECENTS = 0,  // recent books (up to 4)
    PENUMBRA_SIDE_PANEL = 1,    // Title / Stats / Lifetime under-panel
    PENUMBRA_SIDE_ACTION_COUNT = 2,
    // Legacy aliases
    SPECTRAL_SIDE_RECENTS = PENUMBRA_SIDE_RECENTS,
    SPECTRAL_SIDE_PANEL = PENUMBRA_SIDE_PANEL,
    SPECTRAL_SIDE_ACTION_COUNT = PENUMBRA_SIDE_ACTION_COUNT
  };
  using SPECTRAL_SIDE_ACTION = PENUMBRA_SIDE_ACTION;

  // Image rendering in EPUB reader
  enum IMAGE_RENDERING { IMAGES_DISPLAY = 0, IMAGES_PLACEHOLDER = 1, IMAGES_SUPPRESS = 2, IMAGE_RENDERING_COUNT };

  enum TILT_PAGE_TURN { TILT_OFF = 0, TILT_NORMAL = 1, TILT_NVERTED = 2, TILT_PAGE_TURN_COUNT };

  enum TOUCH_READER_CONTROLS { TOUCH_READER_OFF = 0, TOUCH_READER_ON = 1, TOUCH_READER_CONTROLS_COUNT };

  enum QUICK_RESUME_SLEEP_SCREEN {
    QUICK_RESUME_NEVER = 0,
    QUICK_RESUME_AFTER_TIMEOUT = 1,
    QUICK_RESUME_SLEEP_SCREEN_COUNT
  };

  // Sleep wallpaper style (Dark/Light/Cover/Custom/…). Not used when the sleep
  // path is Quick Resume (power action or Timeout QR). Default: Casper Light.
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
  // Reader status chrome font size (Manage Reader UI → Font Size). Default 8 pt.
  uint8_t statusBarFontSize = STATUS_BAR_FONT_8;
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
  // System top chrome slots (Display → Status Bar). Bare/Penumbra: warning middle.
  // Legacy factory (pre-Bare): battery left, clock right — migration may overwrite.
  uint8_t systemStatusBarLeft = SYS_SLOT_HIDE;
  uint8_t systemStatusBarMiddle = SYS_SLOT_BATTERY_WARNING;
  uint8_t systemStatusBarRight = SYS_SLOT_HIDE;
  // When Battery is placed on the system bar: Icon / Percent / Icon + Percent.
  uint8_t systemBatteryDisplay = BATTERY_DISPLAY_ICON_PERCENT;
  // When Battery is placed in reader chrome (Customize Reader UI): same modes, independent.
  uint8_t readerBatteryDisplay = BATTERY_DISPLAY_ICON_PERCENT;
  // Low-battery center message threshold (Bare/Penumbra default 15%).
  uint8_t batteryWarning = BATTERY_WARNING_15;
  // Field performance log on SD (/.casper-logs/). Off by default; Settings → Enable Logging
  // turns on Timing capture (SystemLog + QR timing). Crash reports always write regardless.
  uint8_t systemLogLevel = SYSTEM_LOG_OFF;
  // Derived from system status slots (synced on assign); kept for JSON/web + older readers.
  uint8_t systemClock = STATUS_BAR_CLOCK_SHOW;
  // Legacy single time-left mode (synced from corners when possible).
  uint8_t statusBarTimeLeft = TIME_LEFT_CHAPTER;
  // Clock UTC offset in quarter-hour steps, biased by 48 so it fits in uint8_t.
  // Value 48 = UTC+0, 0 = UTC-12:00, 104 = UTC+14:00.
  // Quarter-hour granularity supports oddball zones like Nepal (+5:45) and Chatham (+12:45).
  // Factory default UTC+0. Display applies this offset to the RTC (which stores UTC after NTP).
  // Never force a developer timezone on other users — they set it under Status Bar → UTC offset.
  uint8_t clockUtcOffsetQ = 48;
  // Clock display format: 0 = 24-hour, 1 = 12-hour (factory default 12h).
  uint8_t clockFormat = 1;
  // Set once an NTP sync succeeds. Used to skip re-syncing on every WiFi connect.
  // Resetting to 0 (e.g. via the web UI) forces a re-sync on next WiFi connect.
  uint8_t clockHasBeenSynced = 0;
  // Text rendering settings. Factory default On (air between paragraphs).
  // Never force this on in migrations — honor the user's saved value.
  uint8_t extraParagraphSpacing = 1;
  // Nested under Extra Paragraph Spacing: gap height when the toggle is On.
  // 0 = Half (½ line — historic default), 1 = Full (1 line), 2 = Quarter (¼ line).
  // Values 0/1 preserved so existing settings.bin stays valid.
  enum EXTRA_PARA_SPACING_HEIGHT : uint8_t {
    SPACING_HALF = 0,
    SPACING_FULL = 1,
    SPACING_QUARTER = 2,
    EXTRA_PARA_SPACING_HEIGHT_COUNT = 3
  };
  uint8_t extraParagraphSpacingHeight = SPACING_HALF;
  // Off by default: AA greys are the main open/page-turn cost on e-ink.
  uint8_t textAntiAliasing = 0;
  // Short power: Quick Resume by default (wallpaper sleep is SHORT_PWRBTN::SLEEP).
  uint8_t shortPwrBtn = PWR_QUICK_RESUME;
  // Long power button press (held past getPowerButtonLongPressDuration); same SHORT_PWRBTN values.
  // Casper default: force full refresh (short remains sleep).
  uint8_t longPwrBtn = FORCE_REFRESH;
  // EPUB reading orientation settings
  // 0 = portrait (default), 1 = landscape clockwise, 2 = inverted, 3 = landscape counter-clockwise
  uint8_t orientation = PORTRAIT;
  // Button layouts (front layout retained for migration only)
  uint8_t frontButtonLayout = BACK_CONFIRM_LEFT_RIGHT;
  uint8_t sideButtonLayout = PREV_NEXT;
  // When 1: front nav follows rotated reader orientation (nested under Reading Orientation).
  // Portrait / Landscape CW: default Off. Portrait 180° / Landscape CCW: default On
  // (CCW: front slot 3 = Down, slot 4 = Up). Applied when orientation changes; user can still toggle.
  uint8_t frontButtonFollowOrientation = 0;

  // Factory default for Orient Front Buttons for a given Reading Orientation.
  static constexpr uint8_t defaultFrontButtonFollowForOrientation(uint8_t orient) {
    return (orient == INVERTED || orient == LANDSCAPE_CCW) ? 1 : 0;
  }
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
  uint8_t hwButtonFunction[HW_REMAP_BUTTON_COUNT] = {BTN_FUNC_BACK, BTN_FUNC_CONFIRM, BTN_FUNC_UP,
                                                     BTN_FUNC_DOWN, BTN_FUNC_LEFT,    BTN_FUNC_RIGHT};
  // Reader font settings (new-device / missing-key factory defaults).
  uint8_t fontFamily = LITERATA;
  uint8_t fontSize = SIZE_12;  // 12 pt Literata
  // Library / Recents / Settings list title size (not reader body, not Penumbra home panel).
  uint8_t menuFontSize = MENU_FONT_XSMALL;  // 10pt Source Serif (shipping default)
  // One-time: old menuFontSize 0/1/2 (12/14/16) → 1/2/3 after inserting 10pt at 0.
  uint8_t casperMenuFont10ptMigrated = 0;
  // When 1, list titles wrap to two lines before ellipsis (UI: "Text Wrapping").
  // Default on (including 12pt) — long titles need it on X4 list width too.
  uint8_t splitBookTitleLines = 1;
  // Normal: Tight crushed descenders on faces whose advanceY ≈ ink height.
  uint8_t lineSpacing = NORMAL;
  // Ship Book's Style: follow EPUB CSS alignment (titles center, body as designed).
  uint8_t paragraphAlignment = BOOK_STYLE;
  // Auto-sleep timeout setting (default 5 minutes). Legacy sleepTimeout enum values are migration-only.
  uint8_t sleepTimeoutMinutes = 5;
  // E-ink refresh frequency (default 15 pages)
  uint8_t refreshFrequency = REFRESH_15;  // default Anti-Ghosting every 15 pages
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
  // Default Off — Flip Orientation is opt-in (not a factory default).
  // Per physical side key (hw 4 / hw 5): X3 Left/Right, X4 Up/Down.
  // Replaces the old shared longPressButtonBehavior (migrated on load).
  uint8_t longPressSideA = OFF;  // hw 4: X3 Left / X4 Up
  uint8_t longPressSideB = OFF;  // hw 5: X3 Right / X4 Down
  // Second end of Flip Orientation: always Portrait ↔ this. Must not be PORTRAIT.
  // Default Landscape CCW (common one-handed reading pair); only used when Flip is selected.
  uint8_t orientationFlipWith = LANDSCAPE_CCW;
  // Long-press Confirm function in EPUB reader (cycles through LONG_PRESS_MENU_FUNCTION values).
  // Casper: open stock Casper dictionary (not a custom dictionary stack).
  uint8_t longPressMenuFunction = LP_MENU_DICTIONARY;
  // Long-press Back while reading — same action enum as longPressMenuFunction.
  // Default Off: short Back release still leaves to Home.
  // Prefer Double-Press Menu for Clipping Tool (Back release cancels child UIs).
  uint8_t longPressBackFunction = LP_MENU_DISABLED;
  // Two quick Confirm releases (within ~400 ms). Same action enum.
  // Casper default: Clipping Tool (classic readers often leave this Off; we want
  // long-press Dictionary + double-tap Clip working out of the box).
  // First tap is delayed slightly when this is not Off so a double can be detected.
  uint8_t doublePressMenuFunction = LP_MENU_CLIPPINGS;
  // UI Theme. Factory default Penumbra on both X3 and X4 (fresh SD / no settings file).
  // casperHomeMigrated still upgrades older saved themes once on load.
  uint8_t uiTheme = PENUMBRA;
  // Penumbra theme side buttons (X3 Left/Right; labels say Up/Down on X4).
  // Defaults: both Panel Scroll (Left = back, Right = forward through Title/Stats/Lifetime).
  // JSON keys remain spectralSideLeft/Right for saved-settings compatibility.
  uint8_t spectralSideLeft = PENUMBRA_SIDE_PANEL;
  uint8_t spectralSideRight = PENUMBRA_SIDE_PANEL;
  // One-time migration: force Casper home chrome defaults once on old SD cards.
  // Default 1 so a first save after factory defaults does not re-force Penumbra
  // if the user already switched theme before reboot.
  uint8_t casperHomeMigrated = 1;
  // One-time: seed missing control keys only (never overwrite saved power / long-press).
  uint8_t casperControlsMigrated = 0;
  // One-time: Off → Clipping Tool for double-press Confirm (Rivulet reader shortcut).
  uint8_t casperDoublePressClipMigrated = 0;
  // One-time flag only — does not rewrite reader time-left slots.
  uint8_t casperChapterTimeLeftDefaultMigrated = 0;
  // One-time migration: factory clock defaults (12h) + keep status-bar clock shown.
  uint8_t casperClockDefaultsMigrated = 0;
  // One-time: undo forced UTC-7 from pre-0.1.6 clock migration (only if still stamp 20).
  uint8_t casperUtcOffsetFixMigrated = 0;
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
  // One-time: drop Lexend/Literata builtins; compact enum to Source Serif + Bitter/Lexend.
  uint8_t casperBuiltinFontsSlimMigrated = 0;
  // One-time: reader fontSize 0..3 (12/14/16/18) → 0..4 (10/12/14/16/18).
  uint8_t casperReaderFontSizePtMigrated = 0;
  // One-time: reader fontSize 0..4 (10/12/14/16/18) → 0..5 (8/10/12/14/16/18).
  uint8_t casperReaderFontSize8Migrated = 0;
  // One-time: classic Left/Right list axes → Up/Down list + Left/Right sides.
  uint8_t casperButtonAxisMigrated = 0;
  // One-time: historical AA/embedded speed defaults. Default 1 so factory installs
  // keep Book's Style + Embedded on (migration must not re-force embedded off).
  uint8_t casperSpeedDefaultsMigrated = 1;
  // One-time: bump Anti-Ghosting default 10 → 15 for existing installs still on 10.
  uint8_t casperAntiGhost15Migrated = 0;
  // One-time: Stats (FocusTheme) removed from firmware → Penumbra (legacy flag).
  // Default 1: factory install is already post-migration.
  uint8_t casperStatsThemeDisabledMigrated = 1;
  // One-time: X4 Bare (after Stats drop) → Penumbra (title + progress face).
  // Default 1 so first-boot users who choose Bare are not remapped on next load.
  uint8_t casperX4SpectralDefaultMigrated = 1;

  // Sunlight fading compensation
  uint8_t fadingFix = 0;
  // Dark Mode master (Settings → Display). Default off. 0 = light, 1 = dark.
  // When On and darkModeReaderOnly is Off: whole UI (home, menus, reader) inverts
  // at display time. When On and darkModeReaderOnly is On: only the book page.
  // JSON key readerDarkMode kept for existing settings files.
  uint8_t readerDarkMode = 0;
  // Nested under Dark Mode. Default off (whole UI). On = invert reader pages only.
  uint8_t darkModeReaderOnly = 0;
  // Power button return from footnotes (1 = enabled, 0 = disabled)
  uint8_t pwrBtnFootnoteBack = 1;
  // Book CSS: follows Alignment. Book's Style ⇒ on; Left/Justify/Center/Right ⇒ off.
  // No separate Style-tab toggle (merged into Alignment / Book's Style).
  uint8_t embeddedStyle = 1;
  // Bionic Reading (UI label; JSON key remains focusReadingEnabled for migration).
  // Bolds the first portion of each word — legacy calls this Bionic Reading;
  // upstream Casper called it Focus Reading. Default off.
  uint8_t focusReadingEnabled = 0;
  // Guide Dots (legacy): middle-dot between words to guide the eye. Default off.
  uint8_t guideReadingEnabled = 0;
  // One-time: Book's Style owns Embedded Style; style-tab toggle removed; ship defaults.
  uint8_t casperBooksStyleOwnsEmbeddedMigrated = 0;
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
  // Child of moveFinishedToReadFolder in UI. When On + parent On: drop finished books from Recents.
  // Default off — user opts in after enabling Move Finished.
  uint8_t removeReadBooksFromRecents = 0;
  // Move EPUB to /read/ when marked finished (0 = off by default, 1 = on)
  uint8_t moveFinishedToReadFolder = 0;
  // X3 only: before deep sleep, copy lifetime stats to /.casper-stats-backup/
  // (one dated file per calendar day when RTC is available). Default on.
  uint8_t autoBackupStats = 1;
  // Idle gap (minutes) for reading-stats sessions / pace samples. Time on a page
  // longer than this is treated as idle and not counted toward reading time or pace.
  // Default 5 minutes (legacy-style "session time" threshold).
  uint8_t readingSessionIdleMinutes = 5;
  // When 1: record reading stats (default). When 0: no tracking; hide Reading Stats UI.
  // Existing on-disk stats are left alone (not deleted).
  // X4 has no RTC — pace/session stats are unreliable; tracking is hard-disabled
  // there (see readingStatsTrackingEnabled). Setting still persists for X3 / web.
  uint8_t readingStatsEnabled = 1;
  // True only when tracking is enabled *and* the device can support it (X3).
  bool readingStatsTrackingEnabled() const;
  static constexpr uint8_t MIN_SESSION_IDLE_MINUTES = 1;
  static constexpr uint8_t MAX_SESSION_IDLE_MINUTES = 30;
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
  // Long-press power (sleep / force refresh) — snappy threshold; scrub itself still
  // takes ~1.5–3s on e-ink. Was 500ms and felt like multi-second holds with HALF lag.
  static constexpr uint16_t POWER_BUTTON_LONG_PRESS_MS = 350;

  // Short-press / wake verification duration. Stays short when short action is
  // Sleep or Quick Resume so a quick tap wakes and re-sleeps correctly.
  uint16_t getPowerButtonDuration() const {
    const bool shortWake = shortPwrBtn == CasperSettings::SHORT_PWRBTN::SLEEP ||
                           shortPwrBtn == CasperSettings::SHORT_PWRBTN::PWR_QUICK_RESUME;
    return shortWake ? POWER_BUTTON_WAKE_SHORT_MS : POWER_BUTTON_WAKE_LONG_MS;
  }
  uint16_t getPowerButtonWakeDuration() const { return getPowerButtonDuration(); }
  // Hold threshold that distinguishes short vs long power press.
  uint16_t getPowerButtonLongPressDuration() const { return POWER_BUTTON_LONG_PRESS_MS; }
  int getReaderFontId() const;
  // Source Serif font id for shared list titles (Library, Recents, Settings, etc.).
  int getMenuListFontId() const;
  // Max lines for list titles when Text Wrapping is on (wrap only if text needs it).
  int getMenuListTitleMaxLines() const { return splitBookTitleLines ? 2 : 1; }

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
    bool showsBatteryIcon() const { return showBattery && batteryDisplay != BATTERY_DISPLAY_PERCENT; }
    bool showsBatteryPercent() const { return showBattery && batteryDisplay != BATTERY_DISPLAY_ICON; }
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
    bool showsClock() const {
      return upperLeft == CORNER_CLOCK || upperMiddle == CORNER_CLOCK || upperRight == CORNER_CLOCK ||
             lowerLeft == CORNER_CLOCK || lowerMiddle == CORNER_CLOCK || lowerRight == CORNER_CLOCK;
    }
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
  // GfxRenderer font id for reader status chrome (clock, %, pages, ETAs, titles).
  int getStatusBarFontId() const;
  // Bottom text-lane height reservation (scales with statusBarFontSize).
  int getStatusBarTextLaneHeight() const;
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
  // Penumbra draws a large home clock (X3) / progress face (X4) — system status bar clock disabled.
  bool systemStatusBarAllowsClock() const;
  // Clear any SYS_SLOT_CLOCK placement (used when switching to Penumbra).
  void stripSystemStatusBarClock();
  // Battery Warning threshold percent (0 = Off).
  int batteryWarningThresholdPercent() const;

  // Resolved text-rendering configuration for the Epub layout engine. The
  // viewport is renderer/orientation-derived, so the caller supplies it —
  // passing it in keeps a spec from ever existing in a half-filled state.
  // Unlocked for the same reason as statusBarSpec(); see the note above.
  ReaderRenderSpec readerRenderSpec(uint16_t viewportWidth, uint16_t viewportHeight) const;

  static const char* getFilePath() { return "/.crosspoint/settings.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  static void validateFrontButtonMapping(CasperSettings& settings);
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
#define SETTINGS CasperSettings::getInstance()
