#include <Arduino.h>
#include <BoardConfig.h>
#include <Epub.h>
#include <FontCacheManager.h>
#include <FontDecompressor.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <HalSystem.h>
#include <HalTiltSensor.h>
#include <I18n.h>
#include <Logging.h>
#include <RivuletEngine.h>
#include <SPI.h>
#include <WiFi.h>
#include <builtinFonts/all.h>
#include <driver/gpio.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <esp_wifi.h>

#include <cstring>

// Pulls lib/Rivulet into the link for bring-up (not the active reader path yet).
static_assert(sizeof(rivulet::RivuletEngine) > 0, "Rivulet engine present");

#include "CasperSettings.h"
#include "CasperState.h"
#include "casper/CasperProduct.h"
#include "MappedInputManager.h"
#include "KOReaderCredentialStore.h"
#include "OpdsServerStore.h"
#include "RecentBooksStore.h"
#include "WifiCredentialStore.h"
#include "SdCardFontSystem.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/reader/ReaderActivity.h"
#include "activities/reader/ReadingStatsUtils.h"
#include "activities/reader/StatsBackup.h"
#include "activities/settings/SdFirmwareUpdateActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "images/LoadingIcon.h"
#include "images/MoonIcon.h"
#include "util/ButtonNavigator.h"

#include "util/QrTimingLog.h"
#include "util/ScreenshotUtil.h"
#include "util/SleepChromeIcon.h"
#include "util/SystemLog.h"
#include "util/UiGhostPolicy.h"

GfxRenderer renderer(display);
MappedInputManager mappedInputManager(gpio, renderer);
ActivityManager activityManager(renderer, mappedInputManager);
FontDecompressor fontDecompressor;
SdCardFontSystem sdFontSystem;
FontCacheManager fontCacheManager(renderer.getFontMap(), renderer.getSdCardFonts());
static unsigned long allowSleepAt = 0;
static bool longPowerButtonHandled = false;

void enterDeepSleep(bool fromTimeout = false, bool powerQuickResume = false);

// Global long-press power actions that fire while still held (sleep / QR / refresh).
static bool isGlobalPowerButtonAction(const CasperSettings::SHORT_PWRBTN action) {
  return action == CasperSettings::SHORT_PWRBTN::SLEEP ||
         action == CasperSettings::SHORT_PWRBTN::PWR_QUICK_RESUME ||
         action == CasperSettings::SHORT_PWRBTN::FORCE_REFRESH;
}

static bool isSleepStylePowerAction(const CasperSettings::SHORT_PWRBTN action) {
  return action == CasperSettings::SHORT_PWRBTN::SLEEP ||
         action == CasperSettings::SHORT_PWRBTN::PWR_QUICK_RESUME;
}

static CasperSettings::SHORT_PWRBTN getPowerButtonAction() {
  const unsigned long held = gpio.getPowerButtonHeldTime();
  const auto shortAction = static_cast<CasperSettings::SHORT_PWRBTN>(SETTINGS.shortPwrBtn);
  const auto longAction = static_cast<CasperSettings::SHORT_PWRBTN>(SETTINGS.longPwrBtn);
  const unsigned long longMs = SETTINGS.getPowerButtonLongPressDuration();

  if (mappedInputManager.wasReleased(MappedInputManager::Button::Power)) {
    if (longPowerButtonHandled) {
      // Wake latch or long-hold action already ran while pressed (e.g. Force Refresh).
      longPowerButtonHandled = false;
      return CasperSettings::SHORT_PWRBTN::IGNORE;
    }
    // Long hold past threshold with a while-held global action: that action should
    // have fired on the hold. Never also run short sleep/QR on release.
    if (held >= longMs && isGlobalPowerButtonAction(longAction)) {
      return CasperSettings::SHORT_PWRBTN::IGNORE;
    }
    // Sleep / Quick Resume short actions fire on release for true short taps.
    if (isSleepStylePowerAction(shortAction)) {
      return shortAction;
    }
    return held < longMs ? shortAction : longAction;
  }

  if (longPowerButtonHandled || !gpio.isPressed(HalGPIO::BTN_POWER) || held < longMs) {
    return CasperSettings::SHORT_PWRBTN::IGNORE;
  }

  // While held past threshold: only fire long if it is a global sleep/QR/refresh.
  if (!isGlobalPowerButtonAction(longAction)) {
    return CasperSettings::SHORT_PWRBTN::IGNORE;
  }
  longPowerButtonHandled = true;
  return longAction;
}

static bool handleGlobalPowerButtonAction(const CasperSettings::SHORT_PWRBTN action) {
  switch (action) {
    case CasperSettings::SHORT_PWRBTN::SLEEP:
      enterDeepSleep(/*fromTimeout=*/false, /*powerQuickResume=*/false);
      return true;
    case CasperSettings::SHORT_PWRBTN::PWR_QUICK_RESUME:
      enterDeepSleep(/*fromTimeout=*/false, /*powerQuickResume=*/true);
      return true;
    case CasperSettings::SHORT_PWRBTN::FORCE_REFRESH: {
      LOG_DBG("MAIN", "Manual screen refresh triggered");
      SystemLog::logTiming("MAIN", "force_refresh long-power held=%lums",
                           static_cast<unsigned long>(gpio.getPowerButtonHeldTime()));
      if (!activityManager.handleForcedRefresh()) {
        // No activity override: hard scrub current framebuffer (X3 HALF+resync).
        RenderLock lock;
        renderer.displayBuffer(HalDisplay::HALF_REFRESH);
        UiGhostPolicy::noteHalf();
      }
      // Action is latched; release power anytime — do not require holding through scrub.
      return true;
    }
    default:
      return false;
  }
}

// Reader body: Literata + Source Serif at 10/12/14/16. Source Serif 8 = UI chrome only.
#ifndef OMIT_FONTS
EpdFont literata10RegularFont(&literata_10_regular);
EpdFont literata10BoldFont(&literata_10_bold);
EpdFont literata10ItalicFont(&literata_10_italic);
EpdFont literata10BoldItalicFont(&literata_10_bolditalic);
EpdFontFamily literata10FontFamily(&literata10RegularFont, &literata10BoldFont, &literata10ItalicFont,
                                   &literata10BoldItalicFont);
EpdFont literata12RegularFont(&literata_12_regular);
EpdFont literata12BoldFont(&literata_12_bold);
EpdFont literata12ItalicFont(&literata_12_italic);
EpdFont literata12BoldItalicFont(&literata_12_bolditalic);
EpdFontFamily literata12FontFamily(&literata12RegularFont, &literata12BoldFont, &literata12ItalicFont,
                                   &literata12BoldItalicFont);
EpdFont literata14RegularFont(&literata_14_regular);
EpdFont literata14BoldFont(&literata_14_bold);
EpdFont literata14ItalicFont(&literata_14_italic);
EpdFont literata14BoldItalicFont(&literata_14_bolditalic);
EpdFontFamily literata14FontFamily(&literata14RegularFont, &literata14BoldFont, &literata14ItalicFont,
                                   &literata14BoldItalicFont);
EpdFont literata16RegularFont(&literata_16_regular);
EpdFont literata16BoldFont(&literata_16_bold);
EpdFont literata16ItalicFont(&literata_16_italic);
EpdFont literata16BoldItalicFont(&literata_16_bolditalic);
EpdFontFamily literata16FontFamily(&literata16RegularFont, &literata16BoldFont, &literata16ItalicFont,
                                   &literata16BoldItalicFont);

// Penumbra Recents: 8 pt author (regular only), 10 pt title (regular + bold focus).
EpdFont sourceserif8RegularFont(&sourceserif4_8_regular);
EpdFontFamily sourceserif8FontFamily(&sourceserif8RegularFont);
EpdFont sourceserif10RegularFont(&sourceserif4_10_regular);
EpdFont sourceserif10BoldFont(&sourceserif4_10_bold);
EpdFontFamily sourceserif10FontFamily(&sourceserif10RegularFont, &sourceserif10BoldFont);
EpdFont sourceserif12RegularFont(&sourceserif4_12_regular);
EpdFont sourceserif12BoldFont(&sourceserif4_12_bold);
EpdFont sourceserif12ItalicFont(&sourceserif4_12_italic);
EpdFont sourceserif12BoldItalicFont(&sourceserif4_12_bolditalic);
EpdFontFamily sourceserif12FontFamily(&sourceserif12RegularFont, &sourceserif12BoldFont, &sourceserif12ItalicFont,
                                      &sourceserif12BoldItalicFont);
EpdFont sourceserif14RegularFont(&sourceserif4_14_regular);
EpdFont sourceserif14BoldFont(&sourceserif4_14_bold);
EpdFont sourceserif14ItalicFont(&sourceserif4_14_italic);
EpdFont sourceserif14BoldItalicFont(&sourceserif4_14_bolditalic);
EpdFontFamily sourceserif14FontFamily(&sourceserif14RegularFont, &sourceserif14BoldFont, &sourceserif14ItalicFont,
                                      &sourceserif14BoldItalicFont);
EpdFont sourceserif16RegularFont(&sourceserif4_16_regular);
EpdFont sourceserif16BoldFont(&sourceserif4_16_bold);
EpdFont sourceserif16ItalicFont(&sourceserif4_16_italic);
EpdFont sourceserif16BoldItalicFont(&sourceserif4_16_bolditalic);
EpdFontFamily sourceserif16FontFamily(&sourceserif16RegularFont, &sourceserif16BoldFont, &sourceserif16ItalicFont,
                                      &sourceserif16BoldItalicFont);
// Home large titles / UI (not offered as built-in reader body size).
EpdFont sourceserif18RegularFont(&sourceserif4_18_regular);
EpdFont sourceserif18BoldFont(&sourceserif4_18_bold);
EpdFont sourceserif18ItalicFont(&sourceserif4_18_italic);
EpdFont sourceserif18BoldItalicFont(&sourceserif4_18_bolditalic);
EpdFontFamily sourceserif18FontFamily(&sourceserif18RegularFont, &sourceserif18BoldFont, &sourceserif18ItalicFont,
                                      &sourceserif18BoldItalicFont);
// Penumbra hero: digits-only 72 pt (Bold face used as REGULAR style slot).
EpdFont sourceserif72ClockFont(&sourceserif4_72_clock);
EpdFontFamily sourceserif72ClockFontFamily(&sourceserif72ClockFont);
#endif  // OMIT_FONTS

// measurement of power button press duration calibration value
unsigned long t1 = 0;
unsigned long t2 = 0;

// Definitions for SilentRestart.h. RTC_NOINIT survives ESP.restart() but not power loss.
RTC_NOINIT_ATTR uint32_t silentRebootMagic;
RTC_NOINIT_ATTR uint32_t silentRebootTarget;
constexpr uint32_t SILENT_REBOOT_MAGIC = 0xC1EAB007;
constexpr uint32_t SILENT_REBOOT_TARGET_HOME = 0;
constexpr uint32_t SILENT_REBOOT_TARGET_READER = 1;

// How the device is coming back to life, resolved once at boot. Both resume
// flows suppress the splash and leave the panel holding its pre-boot frame; a
// plain boot shows the splash. See setup() for the resolution.
enum class BootResume : uint8_t {
  Splash,       // cold boot, flash, panic, or plain reboot
  Silent,       // heap-defrag ESP.restart() (RTC flag; lost on power loss)
  QuickResume,  // power-button wake from deep sleep (QR moon or wallpaper; SD flag)
};

// Latched true once enterDeepSleep() commits to sleeping, before it tears down
// the current activity. WiFi activities call silentRestart() in onExit() to
// clear heap fragmentation on the way out, but deep sleep is a full chip reset
// on wake and already clears the heap, so rebooting here would just power the
// device back up against the user's sleep gesture. Never cleared:
// startDeepSleep() does not return, so a set latch only ends at the wakeup reset.
static bool deepSleepInProgress = false;

// WiFi/LWIP leave fragmented heap and sometimes sticky radio state across
// ESP.restart() if only disconnect() is used. Tear the stack fully so the next
// boot's power-save / clock path does not hang (seen after KOReader auth).
static void tearDownWifiForSilentRestart() {
  if (WiFi.getMode() == WIFI_MODE_NULL) {
    return;
  }
  LOG_DBG("MAIN", "Silent restart: full WiFi teardown heap=%u maxAlloc=%u",
          static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
  WiFi.disconnect(true /*wifioff*/, false /*eraseap*/);
  delay(30);
  WiFi.mode(WIFI_OFF);
  delay(20);
  // Best-effort; ignore errors if already stopped.
  esp_wifi_stop();
  delay(50);
}

void silentRestart() {
  if (deepSleepInProgress) return;  // sleeping supersedes the heap-defrag reboot
  silentRebootTarget = SILENT_REBOOT_TARGET_HOME;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  LOG_DBG("MAIN", "Silent restart (target=home)");
  // E-ink retains the previous frame until Home's first paint lands (~2-3s).
  // Without an overlay, users don't see the reboot and fire input through to
  // Home. Select on the default selectorIndex=0 then opens the most-recent
  // book, looking like a trampoline back to the reader they just exited.
  // Arm system-wide Dark Mode invert before the status label refreshes the panel
  // (loop() is not running during silent restart).
  renderer.setInvertOnDisplay(SETTINGS.readerDarkMode != 0 && SETTINGS.darkModeReaderOnly == 0);
  GUI.drawTopLeftStatus(renderer, tr(STR_LOADING_POPUP), /*refresh=*/true);
  delay(50);
  tearDownWifiForSilentRestart();
  ESP.restart();
}

void silentRestartToReader() {
  if (deepSleepInProgress) return;  // sleeping supersedes the heap-defrag reboot
  silentRebootTarget = SILENT_REBOOT_TARGET_READER;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  LOG_DBG("MAIN", "Silent restart (target=reader)");
  renderer.setInvertOnDisplay(SETTINGS.readerDarkMode != 0 && SETTINGS.darkModeReaderOnly == 0);
  GUI.drawTopLeftStatus(renderer, tr(STR_LOADING_POPUP), /*refresh=*/true);
  delay(50);
  tearDownWifiForSilentRestart();
  ESP.restart();
}

void waitForPowerRelease() {
  gpio.update();
  while (gpio.isPressed(HalGPIO::BTN_POWER)) {
    delay(50);
    gpio.update();
  }
}

constexpr char SLEEP_FRAME_FILE[] = "/.crosspoint/sleep_frame.bin";

static void saveSleepFrameBuffer() {
  Storage.ensureDirectoryExists("/.crosspoint");
  HalFile file;
  if (!Storage.openFileForWrite("SLP", SLEEP_FRAME_FILE, file)) {
    LOG_ERR("MAIN", "sleep_frame save: open failed");
    return;
  }
  const size_t bufferSize = renderer.getBufferSize();
  const size_t written = file.write(renderer.getFrameBuffer(), bufferSize);
  file.close();
  if (written != bufferSize) {
    LOG_ERR("MAIN", "sleep_frame save: wrote %u/%u", static_cast<unsigned>(written), static_cast<unsigned>(bufferSize));
    Storage.remove(SLEEP_FRAME_FILE);
    return;
  }
  LOG_DBG("MAIN", "sleep_frame saved %u bytes", static_cast<unsigned>(bufferSize));
}

static bool loadSleepFrameBuffer() {
  const char* path = SLEEP_FRAME_FILE;
  HalFile file;
  if (!Storage.openFileForRead("SLP", path, file)) return false;
  const size_t bufferSize = display.getBufferSize();
  const size_t bytesRead = file.read(display.getFrameBuffer(), bufferSize);
  file.close();
  if (bytesRead != bufferSize) {
    LOG_ERR("MAIN", "sleep_frame load: read %u/%u", static_cast<unsigned>(bytesRead),
            static_cast<unsigned>(bufferSize));
    Storage.remove(path);
    return false;
  }
  Storage.remove(path);
  if (path != SLEEP_FRAME_FILE && Storage.exists(SLEEP_FRAME_FILE)) {
    Storage.remove(SLEEP_FRAME_FILE);
  }
  return true;
}

// Enter deep sleep mode.
// powerQuickResume: true when Short/Long power action is Quick Resume (not wallpaper Sleep).
void enterDeepSleep(bool fromTimeout, bool powerQuickResume) {
  HalPowerManager::Lock powerLock;  // Ensure we are at normal CPU frequency for sleep preparation

  // QR paint vs wallpaper: power action, idle timeout toggle, or legacy Sleep Screen == QR.
  // Wake path is always seamless for intentional deep sleep (no boot logo) — wallpaper
  // and last-frame both re-seed from sleep_frame and resume reader/home/settings like QR.
  const bool isQuickResumeSleep =
      powerQuickResume ||
      (fromTimeout &&
       SETTINGS.quickResumeSleepScreen == CasperSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT) ||
      (!fromTimeout && !powerQuickResume &&
       SETTINGS.sleepScreen == CasperSettings::SLEEP_SCREEN_MODE::QUICK_RESUME);

  // Classify resume target while activities still exist (reader / menu / settings / home).
  // Must run before the moon so SleepChromeIcon uses the correct context + orientation
  // (Landscape CCW moon must sit on the same edge as the reader status bar).
  activityManager.persistForSleep();
  APP_STATE.sleepResumeTarget = activityManager.classifySleepResumeTarget();
  APP_STATE.lastSleepFromReader =
      (APP_STATE.sleepResumeTarget == CasperState::RESUME_READER ||
       APP_STATE.sleepResumeTarget == CasperState::RESUME_READER_MENU);
  // Successful sleep from reader: clear crash-loop counter so the next QR can
  // open the book (loadCount guard forces Home only after repeated mid-open panics).
  if (APP_STATE.lastSleepFromReader) {
    APP_STATE.readerActivityLoadCount = 0;
  }

  // Instant feedback: moon on the retained page *before* heavy SD / teardown so
  // the user sees the device reacted the moment they pressed power.
  if (isQuickResumeSleep) {
    // System-wide: keep invertOnDisplay so light paint-space FB stays dark on glass.
    // Reader-only: FB is light; temporary invert so the moon lands on a dark page
    // without permanently flipping bits (home must stay light paint-space).
    const bool sysWideDark = SETTINGS.readerDarkMode != 0 && SETTINGS.darkModeReaderOnly == 0;
    const bool readerOnlyDark = SETTINGS.readerDarkMode != 0 && SETTINGS.darkModeReaderOnly != 0;
    renderer.setInvertOnDisplay(sysWideDark);
    SleepChromeIcon::drawAtTopChrome(renderer, MoonIcon, MOONICON_WIDTH, MOONICON_HEIGHT);
    if (readerOnlyDark && APP_STATE.lastSleepFromReader) {
      renderer.invertScreen();
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      renderer.invertScreen();
    } else {
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    }
  }

  // Skip BootActivity splash on power-button wake for both QR and wallpaper sleep.
  APP_STATE.showBootScreen = false;
  APP_STATE.lastSleepRenderedQuickResume = isQuickResumeSleep;

  APP_STATE.saveToFile();
  // Persist settings before power-off so remaps (and other in-RAM settings)
  // are not lost if a prior save failed or never ran.
  SETTINGS.saveToFile();

  SystemLog::logTiming("SLEEP", "enter fromTimeout=%d qr=%d target=%u lastReader=%d pathEmpty=%d",
                       fromTimeout ? 1 : 0, isQuickResumeSleep ? 1 : 0,
                       static_cast<unsigned>(APP_STATE.sleepResumeTarget),
                       APP_STATE.lastSleepFromReader ? 1 : 0, APP_STATE.openEpubPath.empty() ? 1 : 0);
  SystemLog::flush();

  // Commit to sleeping before goToSleep() runs the outgoing activity's onExit():
  // a WiFi activity would otherwise silentRestart() here and reboot instead.
  deepSleepInProgress = true;
  activityManager.goToSleep(fromTimeout, isQuickResumeSleep);

  // Snapshot sleep image (moon-on-page or wallpaper) so wake re-seed matches the glass
  // for no-flash differential. Greyscale wallpapers leave a BW approx in the main FB.
  saveSleepFrameBuffer();

  // X3: optional automatic daily stats backup (RTC date → stats_YYYY-MM-DD.bin).
  // Same-day sleeps overwrite that day's file; keeps a rolling week via prune.
  if (gpio.deviceIsX3() && SETTINGS.autoBackupStats != 0) {
    ReadingStatsDateTime now;
    if (getCurrentLocalReadingStatsDateTime(now)) {
      if (!backupGlobalStats(false)) {
        LOG_ERR("MAIN", "Automatic reading-stats backup failed before deep sleep");
      }
    } else {
      LOG_DBG("MAIN", "Skip auto stats backup: no RTC date/time");
    }
  }

  // Tear down WiFi so the modem power domain isn't held alive across deep sleep.
  // Wake from deep sleep is effectively a chip reset, so no state needs to survive.
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }

  halTiltSensor.deepSleep();
  display.deepSleep();
  LOG_DBG("MAIN", "Entering deep sleep");

  powerManager.startDeepSleep(gpio);
}

void setupDisplayAndFonts(bool seamless = false) {
  display.begin(seamless);
  renderer.begin();
  activityManager.begin();
  LOG_DBG("MAIN", "Display initialized");

  // Initialize font decompressor for compressed reader fonts
  if (!fontDecompressor.init()) {
    LOG_ERR("MAIN", "Font decompressor init failed");
  }
  fontCacheManager.setFontDecompressor(&fontDecompressor);
  renderer.setFontCacheManager(&fontCacheManager);
#ifndef OMIT_FONTS
  renderer.insertFont(LITERATA_10_FONT_ID, literata10FontFamily);
  renderer.insertFont(LITERATA_12_FONT_ID, literata12FontFamily);
  renderer.insertFont(LITERATA_14_FONT_ID, literata14FontFamily);
  renderer.insertFont(LITERATA_16_FONT_ID, literata16FontFamily);
  // UI_10/UI_12 alias Source Serif 12/14 — insert full families once.
  // Source Serif 8: UI chrome only (not a reader body size in the ship set).
  renderer.insertFont(SOURCESERIF4_8_FONT_ID, sourceserif8FontFamily);
  renderer.insertFont(SOURCESERIF4_10_FONT_ID, sourceserif10FontFamily);
  renderer.insertFont(SOURCESERIF4_12_FONT_ID, sourceserif12FontFamily);
  renderer.insertFont(SOURCESERIF4_14_FONT_ID, sourceserif14FontFamily);
  renderer.insertFont(SOURCESERIF4_16_FONT_ID, sourceserif16FontFamily);
  renderer.insertFont(SOURCESERIF4_18_FONT_ID, sourceserif18FontFamily);
  renderer.insertFont(SOURCESERIF4_72_CLOCK_FONT_ID, sourceserif72ClockFontFamily);
#endif  // OMIT_FONTS
  // SMALL_FONT_ID aliases SOURCESERIF4_8_FONT_ID (inserted above).

  // Discover and load SD card fonts
  sdFontSystem.begin(renderer);

  LOG_DBG("MAIN", "Fonts setup");
}

// Xteink C3: GPIO13 is the X4 battery MOSFET latch AND the X3 SD power rail.
// Re-assert after board detect — a prior sleep/firmware can leave the pin held LOW.
static void ensureXteinkStayAlive() {
  BoardConfig::holdPowerRails();
  BoardConfig::releaseSdRail();
#if defined(FREEINK_MCU_C3) && FREEINK_MCU_C3
  gpio_hold_dis(static_cast<gpio_num_t>(13));
  pinMode(13, OUTPUT);
  digitalWrite(13, HIGH);
#endif
}

void setup() {
  ensureXteinkStayAlive();

  t1 = millis();

#ifdef ENABLE_SERIAL_LOG
  // Earliest possible Serial setup. The 250 ms stall before begin() lets the
  // USB Serial/JTAG peripheral finish power-on and lets the host complete USB
  // enumeration before we touch the CDC state — otherwise cold boot races
  // and the host has to be physically replugged for logs to flow. Warm reboot
  // worked without the delay because USB was already enumerated.
  delay(250);
  Serial.begin(115200);
#if LOG_SERIAL_HAS_TX_TIMEOUT
  logSerial.setTxTimeoutMs(1);  // This is a load-bearing 1. Do not modify.
#endif
#endif

  HalSystem::begin();

  // Read-and-clear so a panic later in setup() doesn't loop into silent reboot.
  // Bound the target range too — RTC_NOINIT memory is uninitialized on cold boot.
  const bool isSilentReboot = (silentRebootMagic == SILENT_REBOOT_MAGIC);
  const uint32_t snapshotTarget =
      (isSilentReboot && silentRebootTarget <= SILENT_REBOOT_TARGET_READER) ? silentRebootTarget : 0;
  silentRebootMagic = 0;
  silentRebootTarget = 0;

  gpio.begin();
  ensureXteinkStayAlive();  // board profile may have switched X3↔X4
  powerManager.begin();
  halTiltSensor.begin();
  halClock.begin();

  LOG_INF("MAIN", "Hardware detect: %s", gpio.deviceIsX3() ? "X3" : "X4");

  // SD Card Initialization
  // We need 6 open files concurrently when parsing a new chapter
  if (!Storage.begin()) {
    LOG_ERR("MAIN", "SD card initialization failed");
    setupDisplayAndFonts(isSilentReboot);
    activityManager.goToFullScreenMessage("SD card error", EpdFontFamily::BOLD);
    return;
  }

  // Install RTC FAT timestamps as early as possible. Without a callback, SdFat
  // stamps new files 12/31/2025 11:00 PM. Install once with no offset, then again
  // after settings so UTC offset is correct for crash_report and all later files.
  Storage.installDateTimeCallback(nullptr);

  // Casper product: no boot migrate, no dual-read (see CasperProduct.h).
  static_assert(!CasperProduct::kHasBootForeignMigrate, "boot migrate must stay off");
  static_assert(!CasperProduct::kRuntimeDualReadForeign, "dual-read must stay off");

  SETTINGS.loadFromFile();
  Storage.installDateTimeCallback(&SETTINGS.clockUtcOffsetQ);
  HalSystem::checkPanic();
  APP_STATE.loadFromFile();

  // Wake cause before QR planning — flash/USB must not look like sleep-from-reader.
  const auto wakeupReason = gpio.getWakeupReason();

  // Detect sleep-wake → book early so we can skip non-critical boot work.
  // PowerButton covers: deep-sleep GPIO wake (X3 / X4+USB) and X4 battery latch
  // POWERON (MCU fully powered off in sleep). Flash/USB are never PowerButton.
  // loadCount >= 3: one flaky open must not permanently force Home; repeated mid-open
  // panics still trip the guard. success and sleep-from-reader reset it.
  // Target READER/READER_MENU always; legacy state (HOME default + lastSleepFromReader)
  // still reopens the book. SETTINGS never routes to book.
  const bool qrToBook = wakeupReason == HalGPIO::WakeupReason::PowerButton &&
                        !APP_STATE.openEpubPath.empty() && APP_STATE.readerActivityLoadCount < 3 &&
                        (APP_STATE.sleepResumeTarget == CasperState::RESUME_READER ||
                         APP_STATE.sleepResumeTarget == CasperState::RESUME_READER_MENU ||
                         (APP_STATE.lastSleepFromReader &&
                          APP_STATE.sleepResumeTarget != CasperState::RESUME_SETTINGS));

  // Defer recents/KOReader/OPDS SD reads until after first ink on QR→book
  // (saves ~50–150ms and SD contention before the page is readable).
  if (!qrToBook) {
    RECENT_BOOKS.loadFromFile();
    KOREADER_STORE.loadFromFile();
    OPDS_STORE.loadFromFile();
    WIFI_STORE.loadFromFile();
  }
  I18N.setLanguage(static_cast<Language>(SETTINGS.language));
  UITheme::getInstance().reload();
  ButtonNavigator::setMappedInputManager(mappedInputManager);
  // Buffered rotating SD log for field performance captures (/.casper-logs/).
  SystemLog::begin();
  SystemLog::logTiming("BOOT", "settings_loaded millis=%lu qrBook=%d wake=%d", static_cast<unsigned long>(millis()),
                       qrToBook ? 1 : 0, static_cast<int>(wakeupReason));

  // Flash / USB / unknown cold boots: drop sticky reader-wake flags so a later
  // power press cannot reopen the last book (flash → USB sleep → power loop).
  // Do NOT clear on PowerButton — that is a real sleep wake (incl. X4 battery).
  auto clearStickyReaderWake = []() {
    if (!APP_STATE.lastSleepFromReader && APP_STATE.showBootScreen &&
        APP_STATE.sleepResumeTarget == CasperState::RESUME_HOME && !APP_STATE.lastSleepRenderedQuickResume) {
      return;
    }
    APP_STATE.lastSleepFromReader = false;
    APP_STATE.showBootScreen = true;
    APP_STATE.sleepResumeTarget = CasperState::RESUME_HOME;
    APP_STATE.lastSleepRenderedQuickResume = false;
    APP_STATE.saveToFile();
    LOG_DBG("MAIN", "Cleared sticky lastSleepFromReader (non power-button boot)");
  };

  switch (wakeupReason) {
    case HalGPIO::WakeupReason::PowerButton:
      LOG_DBG("MAIN", "Verifying power button press duration (sleep wake)");
      // Always accept a short power press after intentional deep sleep (X3-like).
      // Gating short-wake on shortPwrBtn==SLEEP/QR made short taps re-sleep when
      // long was sleep or settings were mismatched — felt like "long press only".
      if (!gpio.verifyPowerButtonWakeup(SETTINGS.getPowerButtonDuration(),
                                        /*shortPressAllowed=*/true)) {
        powerManager.startDeepSleep(gpio);
      }
      break;
    case HalGPIO::WakeupReason::AfterUSBPower:
      // USB VBUS glitch while off — do not treat as reader QR. Clear sticky wake
      // so the next intentional power press lands on Home, then sleep again.
      LOG_DBG("MAIN", "Wakeup reason: After USB Power");
      clearStickyReaderWake();
      powerManager.startDeepSleep(gpio);
      break;
    case HalGPIO::WakeupReason::AfterFlash:
      LOG_DBG("MAIN", "Wakeup reason: After flash — cold boot to Home");
      clearStickyReaderWake();
      break;
    case HalGPIO::WakeupReason::Other:
    default:
      clearStickyReaderWake();
      break;
  }

  // Recovery firmware mode: hold left side button (BTN_UP) together with the power button at
  // boot to skip directly to the SD-card firmware update screen. Useful on devices where USB
  // flashing has been locked down (e.g. recent X3 firmware).
  bool recoveryFirmwareMode = false;
  if (wakeupReason == HalGPIO::WakeupReason::PowerButton) {
    // QR→book: quick sample only (~60ms). Full 500ms settle delayed first ink by half a second
    // on every power wake and was a large chunk of the ~5s QR total vs YACP.
    const unsigned long settleMs = qrToBook ? 60UL : 500UL;
    const unsigned long settleStart = millis();
    while (millis() - settleStart < settleMs) {
      gpio.update();
      delay(10);
    }
    if (gpio.isPressed(HalGPIO::BTN_UP)) {
      recoveryFirmwareMode = true;
      LOG_INF("MAIN", "Recovery firmware mode (UP + POWER held at boot)");
    }
  }

  // First serial output only here to avoid timing inconsistencies for power button press duration verification
  LOG_DBG("MAIN", "Starting Casper version " CASPER_VERSION);

  // Resolve the single boot-presentation decision. Skipping the splash also
  // skips the panel-clearing pass and the X3 initial-full-sync arming (see
  // HalDisplay::begin), so the first paint is FAST_REFRESH (~500ms) over the
  // retained frame and input dispatches against a visible UI.
  //
  // Power-button wake is ALWAYS seamless (wallpaper or QR moon) — never show
  // BootActivity. The panel already holds the sleep image; we only re-seed the
  // controller and resume reader/home. Flash / USB / unknown cold boot → Splash.
  // (Do not require ESP_RST_DEEPSLEEP only — that forced a full reboot-feel on
  // X4 unplugged wake.)
  const bool powerButtonWake = wakeupReason == HalGPIO::WakeupReason::PowerButton;
  const BootResume resume = isSilentReboot    ? BootResume::Silent
                            : powerButtonWake ? BootResume::QuickResume
                                              : BootResume::Splash;

  if (resume == BootResume::QuickResume) {
    // SD: /.casper-logs/qr_timing.log — pull after a QR wake and paste the latest block.
    QrTimingLog::begin("QuickResume");
    QrTimingLog::line("after SETTINGS/APP_STATE load (pre display)");
    SystemLog::logTiming("QR", "wake start");
  } else if (resume == BootResume::Splash) {
    SystemLog::logTiming("BOOT", "cold/splash path");
  } else {
    SystemLog::logTiming("BOOT", "silent reboot path");
  }

  setupDisplayAndFonts(resume != BootResume::Splash);
  if (QrTimingLog::active()) QrTimingLog::line("after setupDisplayAndFonts");
  SystemLog::logTiming("BOOT", "after setupDisplayAndFonts millis=%lu", static_cast<unsigned long>(millis()));

  switch (resume) {
    case BootResume::Silent:
      // Splash skipped: the routing block below picks the target activity; the
      // panel keeps showing the pre-reboot popup until that first paint lands.
      break;
    case BootResume::QuickResume: {
      // One-shot flag: re-arm cold-boot splash (saved after first ink on QR→book).
      APP_STATE.showBootScreen = true;
      APP_STATE.lastSleepRenderedQuickResume = false;
      // Resume destination from sleep: book, book menu, settings, or home.
      const bool qrOpenBook = qrToBook && !mappedInputManager.isPressed(MappedInputManager::Button::Back);
      const bool qrOpenSettings =
          !qrOpenBook && APP_STATE.sleepResumeTarget == CasperState::RESUME_SETTINGS &&
          !mappedInputManager.isPressed(MappedInputManager::Button::Back);
      SystemLog::logTiming("QR", "power wake openBook=%d settings=%d target=%u lastReader=%d pathEmpty=%d",
                           qrOpenBook ? 1 : 0, qrOpenSettings ? 1 : 0,
                           static_cast<unsigned>(APP_STATE.sleepResumeTarget),
                           APP_STATE.lastSleepFromReader ? 1 : 0, APP_STATE.openEpubPath.empty() ? 1 : 0);
      if (QrTimingLog::active()) {
        QrTimingLog::line("qr_plan openBook=%d settings=%d target=%u pathEmpty=%d lastReader=%d loadCount=%u",
                          qrOpenBook ? 1 : 0, qrOpenSettings ? 1 : 0,
                          static_cast<unsigned>(APP_STATE.sleepResumeTarget),
                          APP_STATE.openEpubPath.empty() ? 1 : 0, APP_STATE.lastSleepFromReader ? 1 : 0,
                          static_cast<unsigned>(APP_STATE.readerActivityLoadCount));
      }
      if (loadSleepFrameBuffer()) {
        if (QrTimingLog::active()) QrTimingLog::line("after loadSleepFrameBuffer");
        // Re-seed controller "previous" plane from the restored FB (X3 DTM1 / X4 RED).
        renderer.cleanupGrayscaleWithFrameBuffer();
        // QR→book: keep moon/page on glass — do NOT FAST the panel before first
        // ink (was a full ~0.5–1s X3 wait on every wake vs 0.1.5). First page
        // paint replaces the frame. Non-book wakes still show moon→dots feedback.
        if (!qrOpenBook) {
          const bool readerOnlyDarkWake =
              SETTINGS.readerDarkMode != 0 && SETTINGS.darkModeReaderOnly != 0;
          SleepChromeIcon::replaceAtTopChrome(renderer, LoadingIcon, LOADINGICON_WIDTH, LOADINGICON_HEIGHT);
          if (readerOnlyDarkWake) {
            renderer.invertScreen();
            renderer.displayBuffer(HalDisplay::FAST_REFRESH);
            renderer.invertScreen();
          } else {
            renderer.displayBuffer(HalDisplay::FAST_REFRESH);
          }
          if (QrTimingLog::active()) QrTimingLog::line("after moon→dots (non-book QR)");
        } else if (QrTimingLog::active()) {
          QrTimingLog::line("QR→book: skip pre-ink panel FAST (glass kept)");
        }
      } else {
        // Never show BootActivity here — glass already holds wallpaper/moon through
        // deep sleep. Missing sleep_frame only skips controller re-seed.
        SystemLog::logTiming("QR", "sleep_frame MISSING — keep glass, skip splash");
        if (QrTimingLog::active()) QrTimingLog::line("sleep_frame MISSING — keep glass (no splash)");
      }
      // QR→book: defer APP_STATE.save (showBootScreen) until after first ink — SD write
      // was ~200ms on the critical path. Non-book QR still saves now.
      if (!qrOpenBook) {
        APP_STATE.saveToFile();
        if (QrTimingLog::active()) QrTimingLog::line("after showBootScreen saveToFile");
      }
      break;
    }
    case BootResume::Splash:
      // Cold boot / flash / unknown only — never power-button sleep wake.
      activityManager.goToBoot();
      break;
  }

  if (recoveryFirmwareMode) {
    // Skip normal home/reader routing: jump straight into the SD firmware picker.
    activityManager.replaceActivity(
        std::make_unique<SdFirmwareUpdateActivity>(renderer, mappedInputManager, /*recoveryMode=*/true));
  } else if (HalSystem::isRebootFromPanic()) {
    // If we rebooted from a panic, go to crash report screen to show the panic info
    activityManager.goToCrashReport();
  } else if (resume == BootResume::Silent && snapshotTarget == SILENT_REBOOT_TARGET_READER &&
             !APP_STATE.openEpubPath.empty()) {
    activityManager.goToReader(APP_STATE.openEpubPath);
  } else if (resume == BootResume::Silent) {
    // target == home (or reader with no open book): land on home — don't fall
    // through to the sleep-wake "resume reader" logic, which fires on stale
    // openEpubPath + lastSleepFromReader from a prior session.
    activityManager.goHome();
  } else if (resume == BootResume::QuickResume &&
             APP_STATE.sleepResumeTarget == CasperState::RESUME_SETTINGS &&
             !mappedInputManager.isPressed(MappedInputManager::Button::Back)) {
    // Slept in Settings — land back in Settings without a Loading flash.
    APP_STATE.sleepResumeTarget = CasperState::RESUME_HOME;
    activityManager.goHome();
    activityManager.goToSettings();
    // Drain deferred Push (Home may enter sync; Settings is always pushed).
    activityManager.loop();
    activityManager.requestUpdateAndWait();
    if (QrTimingLog::active()) {
      QrTimingLog::line("QR → Settings");
      QrTimingLog::end("settings_resume");
    }
  } else if (resume != BootResume::QuickResume || !qrToBook ||
             mappedInputManager.isPressed(MappedInputManager::Button::Back)) {
    // Home for splash / after flash / non-reader sleep / Back held.
    // Only genuine QuickResume sleep-from-reader (qrToBook) auto-opens a book.
    if (qrToBook) {
      // Planned book open aborted (Back held) — load stores deferred for QR.
      RECENT_BOOKS.loadFromFile();
      KOREADER_STORE.loadFromFile();
      OPDS_STORE.loadFromFile();
      WIFI_STORE.loadFromFile();
    }
    APP_STATE.sleepResumeTarget = CasperState::RESUME_HOME;
    activityManager.goHome();
  } else {
    // Sleep-from-reader QuickResume: open book (and optionally book menu).
    // On QR, push first ink in setup before waiting for power release.
    //
    // Keep openEpubPath sticky until open succeeds (YACP-style). Clearing it
    // before first ink caused pathEmpty=1 on the next wake whenever deferred
    // path restore had not run yet — QR dumped users on Home.
    const auto path = APP_STATE.openEpubPath;
    // Preserve menu target for Rivulet onEnter (openReaderMenu after load).
    if (APP_STATE.sleepResumeTarget != CasperState::RESUME_READER_MENU) {
      APP_STATE.sleepResumeTarget = CasperState::RESUME_READER;
    }
    APP_STATE.readerActivityLoadCount++;
    // Non-QR: persist loadCount before open so a mid-open crash trips the guard.
    // QR: defer SD write until after first ink (latency), but never wipe the path.
    if (resume != BootResume::QuickResume) {
      APP_STATE.saveToFile();
    }
    if (resume == BootResume::QuickResume) {
      ReaderActivity::setOpenHints(/*preferFastFirstRefresh=*/true,
                                   /*deferFirstPageTextAa=*/SETTINGS.textAntiAliasing != 0);
    }
    if (QrTimingLog::active()) {
      QrTimingLog::line("before goToReader path=%s target=%u loadCount=%u stickyPath=1", path.c_str(),
                        static_cast<unsigned>(APP_STATE.sleepResumeTarget),
                        static_cast<unsigned>(APP_STATE.readerActivityLoadCount));
    }
    activityManager.goToReader(path);
    if (QrTimingLog::active()) QrTimingLog::line("after goToReader returns (open may still paint)");

    if (resume == BootResume::QuickResume) {
      // Drain Reader→EpubReader swap (pending from onEnter) and paint first page now.
      activityManager.loop();
      if (QrTimingLog::active()) QrTimingLog::line("after activityManager.loop drain");
      activityManager.requestUpdateAndWait();
      if (QrTimingLog::active()) QrTimingLog::line("after first_ink wait");
      // Open landed: keep path, clear crash-loop counter. Failed open leaves path
      // sticky for the next power wake (loadCount still elevated).
      if (activityManager.isReaderActivity()) {
        APP_STATE.openEpubPath = path;
        APP_STATE.readerActivityLoadCount = 0;
        if (QrTimingLog::active()) QrTimingLog::line("QR open OK path sticky loadCount=0");
      } else {
        APP_STATE.openEpubPath = path;  // never blank the last book on fail
        if (QrTimingLog::active()) {
          QrTimingLog::line("QR open FAILED (not reader) path kept loadCount=%u",
                            static_cast<unsigned>(APP_STATE.readerActivityLoadCount));
        }
      }
      APP_STATE.saveToFile();
      RECENT_BOOKS.loadFromFile();
      KOREADER_STORE.loadFromFile();
      OPDS_STORE.loadFromFile();
      WIFI_STORE.loadFromFile();
      if (QrTimingLog::active()) {
        QrTimingLog::line("after deferred stores+save");
        QrTimingLog::end("first_ink_setup");
      }
    }
  }

  if (resume == BootResume::Silent) {
    // Block until the first paint physically completes. refreshDisplay()
    // waits on the panel BUSY pin so when this returns the user can see the
    // new activity. Without the wait, an edge captured by gpio.update()
    // during boot dispatches against an invisible Home and the default
    // selectorIndex=0 opens the most-recent book.
    activityManager.requestUpdateAndWait();
    // Absorb any button held at this point into currentState as a non-edge:
    // two gpio.update() calls separated by > InputManager's 5ms debounce
    // transition the held bit through lastDebounceTime into currentState
    // without setting pressedEvents, so the first loop()'s own gpio.update()
    // sees state == currentState and emits nothing.
    gpio.update();
    delay(10);
    gpio.update();
  }

  // QR→book already showed first ink above; don't stall on power-release before loop.
  // (Holding power through boot used to block here and add seconds to perceived wake.)
  if (resume != BootResume::QuickResume || !APP_STATE.lastSleepFromReader) {
    waitForPowerRelease();
    longPowerButtonHandled = false;
  } else {
    // Only ignore the *wake* gesture if power is still held when setup ends.
    // Always latching true meant: if the user already released the wake press,
    // the next shortPwrBtn=SLEEP release was eaten (clear latch + IGNORE) and
    // sleep needed a second press — matches "few presses to sleep" on X3.
    longPowerButtonHandled = gpio.isPressed(HalGPIO::BTN_POWER);
  }
  // Grace so a still-held wake press cannot re-sleep the instant allowSleepAt
  // elapses; that release is swallowed via longPowerButtonHandled above.
  allowSleepAt = millis() + (resume == BootResume::QuickResume ? 800UL : 2000UL);
}

void loop() {
  static unsigned long maxLoopDuration = 0;
  const unsigned long loopStartTime = millis();
  static unsigned long lastMemPrint = 0;

  gpio.setSharedConfirmPowerShortPressEmitsPower(SETTINGS.shortPwrBtn == CasperSettings::SHORT_PWRBTN::SLEEP ||
                                                 SETTINGS.shortPwrBtn ==
                                                     CasperSettings::SHORT_PWRBTN::PWR_QUICK_RESUME);
  gpio.update();
  halTiltSensor.update(SETTINGS.tiltPageTurn, SETTINGS.orientation, activityManager.isReaderActivity());

  renderer.setFadingFix(SETTINGS.fadingFix);
  // Whole-UI Dark Mode (Display → Dark Mode, Reader Only Off). Reader-only uses paint invert.
  renderer.setInvertOnDisplay(SETTINGS.readerDarkMode != 0 && SETTINGS.darkModeReaderOnly == 0);

  if (Serial && millis() - lastMemPrint >= 10000) {
    LOG_INF("MEM", "Free: %d bytes, Total: %d bytes, Min Free: %d bytes, MaxAlloc: %d bytes", ESP.getFreeHeap(),
            ESP.getHeapSize(), ESP.getMinFreeHeap(), ESP.getMaxAllocHeap());
    lastMemPrint = millis();
  }

  // Handle incoming serial commands,
  // nb: we use logSerial from logging to avoid deprecation warnings
  if (logSerial.available() > 0) {
    String line = logSerial.readStringUntil('\n');
    if (line.startsWith("CMD:")) {
      String cmd = line.substring(4);
      cmd.trim();
      if (cmd == "SCREENSHOT") {
        const uint32_t bufferSize = display.getBufferSize();
        logSerial.printf("SCREENSHOT_START:%d\n", bufferSize);
        uint8_t* buf = display.getFrameBuffer();
        logSerial.write(buf, bufferSize);
        logSerial.printf("SCREENSHOT_END\n");
      }
    }
  }

  // Check for any user activity (button press or release) or active background work
  static unsigned long lastActivityTime = millis();
  if (gpio.wasAnyPressed() || gpio.wasAnyReleased() || gpio.wasTouchActivity() || halTiltSensor.hadActivity() ||
      activityManager.preventAutoSleep()) {
    lastActivityTime = millis();         // Reset inactivity timer
    powerManager.setPowerSaving(false);  // Restore normal CPU frequency on user activity
  }

  static bool screenshotButtonsReleased = true;
  static bool screenshotComboActive = false;
  if (gpio.isPressed(HalGPIO::BTN_POWER) && gpio.isPressed(HalGPIO::BTN_DOWN)) {
    screenshotComboActive = true;
    if (screenshotButtonsReleased) {
      screenshotButtonsReleased = false;
      {
        RenderLock lock;
        ScreenshotUtil::takeScreenshot(renderer);
      }
    }
    return;
  }
  if (screenshotComboActive) {
    if (gpio.isPressed(HalGPIO::BTN_POWER)) return;
    if (gpio.wasReleased(HalGPIO::BTN_POWER)) {
      screenshotButtonsReleased = true;
      screenshotComboActive = false;
      return;
    }
    screenshotButtonsReleased = true;
    screenshotComboActive = false;
  }

  const unsigned long sleepTimeoutMs = SETTINGS.getSleepTimeoutMs();
  if (sleepTimeoutMs > 0 && millis() - lastActivityTime >= sleepTimeoutMs) {
    LOG_DBG("SLP", "Auto-sleep triggered after %lu ms of inactivity", sleepTimeoutMs);
    enterDeepSleep(true);
    // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
    return;
  }

  // Short vs long power: long held triggers global sleep/refresh immediately;
  // release dispatches the short or long action (legacy-style).
  //
  // Do NOT clear longPowerButtonHandled merely because power is up. Force Refresh
  // blocks for ~1–3s (HALF scrub); the user usually releases during that scrub.
  // Clearing the latch here before wasReleased is handled made shortPwrBtn=SLEEP
  // fire on the release edge → refresh then sleep. Latch is cleared only when
  // getPowerButtonAction sees the release (IGNORE) or on a fresh power press.
  if (millis() >= allowSleepAt && longPowerButtonHandled && gpio.wasPressed(HalGPIO::BTN_POWER)) {
    // New press after a completed long action / wake latch — arm for this hold.
    longPowerButtonHandled = false;
  }
  if (millis() >= allowSleepAt) {
    // Screenshot combo still takes priority over power sleep.
    if (!(gpio.isPressed(HalGPIO::BTN_POWER) && gpio.isPressed(HalGPIO::BTN_DOWN))) {
      if (handleGlobalPowerButtonAction(getPowerButtonAction())) {
        return;
      }
    }
  }

  // Do not force a full-screen refresh for USB plug/unplug, clock, or battery.
  // Chrome is redrawn on the next user-driven paint (page turn, menu, navigation).
  // A background requestUpdate here used to black-flash home multipass themes.
  (void)gpio.wasUsbStateChanged();  // consume edge so it does not stick

  SystemLog::maybeSampleHeap();

  const unsigned long activityStartTime = millis();
  activityManager.loop();
  const unsigned long activityDuration = millis() - activityStartTime;

  const unsigned long loopDuration = millis() - loopStartTime;
  // Surface long main-loop stalls to SD (hang hunting). 2s covers EPD FAST; HALF is longer.
  if (activityDuration >= 2000) {
    SystemLog::logCritical("LOOP", "activity_slow %lums fre=%u", static_cast<unsigned long>(activityDuration),
                           static_cast<unsigned>(ESP.getFreeHeap()));
  } else if (loopDuration >= 5000) {
    SystemLog::logCritical("LOOP", "loop_slow %lums act=%lums fre=%u", static_cast<unsigned long>(loopDuration),
                           static_cast<unsigned long>(activityDuration), static_cast<unsigned>(ESP.getFreeHeap()));
  }
  if (loopDuration > maxLoopDuration) {
    maxLoopDuration = loopDuration;
    if (maxLoopDuration > 50) {
      LOG_DBG("LOOP", "New max loop duration: %lu ms (activity: %lu ms)", maxLoopDuration, activityDuration);
    }
  }

  // Add delay at the end of the loop to prevent tight spinning
  // When an activity requests skip loop delay (e.g., webserver running), use yield() for faster response
  // Otherwise, use longer delay to save power
  if (activityManager.skipLoopDelay()) {
    powerManager.setPowerSaving(false);  // Make sure we're at full performance when skipLoopDelay is requested
    yield();                             // Give FreeRTOS a chance to run tasks, but return immediately
  } else {
    if (millis() - lastActivityTime >= HalPowerManager::IDLE_POWER_SAVING_MS) {
      // If we've been inactive for a while, increase the delay to save power
      powerManager.setPowerSaving(true);  // Lower CPU frequency after extended inactivity
      // shortPwrBtn=SLEEP is release-edge. A 50ms sample while power is held
      // makes short taps flaky (press+release between updates). Stay snappy
      // while power is down so one short press reliably sleeps.
      delay(gpio.isPressed(HalGPIO::BTN_POWER) ? 10 : 50);
    } else {
      // Short delay to prevent tight loop while still being responsive
      delay(10);
    }
  }
}
