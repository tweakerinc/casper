#include <Arduino.h>
#include <esp_sleep.h>
#include <esp_system.h>
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
#include <SPI.h>
#include <WiFi.h>
#include <builtinFonts/all.h>

#include <cstring>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "KOReaderCredentialStore.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "RecentBooksStore.h"
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
#include "util/SleepChromeIcon.h"
#include "util/ButtonNavigator.h"
#include "util/QrTimingLog.h"
#include "util/ScreenshotUtil.h"
#include "util/SystemLog.h"

GfxRenderer renderer(display);
MappedInputManager mappedInputManager(gpio, renderer);
ActivityManager activityManager(renderer, mappedInputManager);
FontDecompressor fontDecompressor;
SdCardFontSystem sdFontSystem;
FontCacheManager fontCacheManager(renderer.getFontMap(), renderer.getSdCardFonts());
static unsigned long allowSleepAt = 0;
static bool longPowerButtonHandled = false;

void enterDeepSleep(bool fromTimeout = false);

// Global long-press power actions that fire while still held (sleep / refresh).
static bool isGlobalPowerButtonAction(const CrossPointSettings::SHORT_PWRBTN action) {
  return action == CrossPointSettings::SHORT_PWRBTN::SLEEP ||
         action == CrossPointSettings::SHORT_PWRBTN::FORCE_REFRESH;
}

static CrossPointSettings::SHORT_PWRBTN getPowerButtonAction() {
  const unsigned long held = gpio.getPowerButtonHeldTime();
  const auto shortAction = static_cast<CrossPointSettings::SHORT_PWRBTN>(SETTINGS.shortPwrBtn);
  const auto longAction = static_cast<CrossPointSettings::SHORT_PWRBTN>(SETTINGS.longPwrBtn);
  const unsigned long longMs = SETTINGS.getPowerButtonLongPressDuration();

  if (mappedInputManager.wasReleased(MappedInputManager::Button::Power)) {
    if (longPowerButtonHandled) {
      // Wake latch or long-hold action already ran while pressed (e.g. Force Refresh).
      longPowerButtonHandled = false;
      return CrossPointSettings::SHORT_PWRBTN::IGNORE;
    }
    // Long hold past threshold with a while-held global action (Force Refresh /
    // Sleep): that action should have fired on the hold. Never also run short
    // SLEEP on release — Force Refresh's blocking HALF used to clear the latch
    // before this edge, then short=SLEEP put the device to sleep after the scrub.
    if (held >= longMs && isGlobalPowerButtonAction(longAction)) {
      return CrossPointSettings::SHORT_PWRBTN::IGNORE;
    }
    // shortPwrBtn=SLEEP: sleep on release for true short taps (held < longMs).
    // Long holds that are not global long-actions still map to longAction below.
    if (shortAction == CrossPointSettings::SHORT_PWRBTN::SLEEP) {
      return CrossPointSettings::SHORT_PWRBTN::SLEEP;
    }
    return held < longMs ? shortAction : longAction;
  }

  if (longPowerButtonHandled || !gpio.isPressed(HalGPIO::BTN_POWER) || held < longMs) {
    return CrossPointSettings::SHORT_PWRBTN::IGNORE;
  }

  // While held past threshold: only fire long if it is a global sleep/refresh.
  // (Force Refresh still works on long-hold; Sleep short-press is release-edge above.)
  if (!isGlobalPowerButtonAction(longAction)) {
    return CrossPointSettings::SHORT_PWRBTN::IGNORE;
  }
  longPowerButtonHandled = true;
  return longAction;
}

static bool handleGlobalPowerButtonAction(const CrossPointSettings::SHORT_PWRBTN action) {
  switch (action) {
    case CrossPointSettings::SHORT_PWRBTN::SLEEP:
      enterDeepSleep();
      return true;
    case CrossPointSettings::SHORT_PWRBTN::FORCE_REFRESH: {
      LOG_DBG("MAIN", "Manual screen refresh triggered");
      SystemLog::logTiming("MAIN", "force_refresh long-power held=%lums",
                           static_cast<unsigned long>(gpio.getPowerButtonHeldTime()));
      if (!activityManager.handleForcedRefresh()) {
        // No activity override: scrub current framebuffer (HALF + X3 resync).
        RenderLock lock;
        renderer.displayBuffer(HalDisplay::HALF_REFRESH);
      }
      // Action is latched; release power anytime — do not require holding through scrub.
      return true;
    }
    default:
      return false;
  }
}

// Reader + UI: Source Serif 4 (default) and Bitter. Lexend/Literata via SD download only.
#ifndef OMIT_FONTS
EpdFont bitter12RegularFont(&bitter_12_regular);
EpdFont bitter12BoldFont(&bitter_12_bold);
EpdFont bitter12ItalicFont(&bitter_12_italic);
EpdFont bitter12BoldItalicFont(&bitter_12_bolditalic);
EpdFontFamily bitter12FontFamily(&bitter12RegularFont, &bitter12BoldFont, &bitter12ItalicFont,
                                 &bitter12BoldItalicFont);
EpdFont bitter14RegularFont(&bitter_14_regular);
EpdFont bitter14BoldFont(&bitter_14_bold);
EpdFont bitter14ItalicFont(&bitter_14_italic);
EpdFont bitter14BoldItalicFont(&bitter_14_bolditalic);
EpdFontFamily bitter14FontFamily(&bitter14RegularFont, &bitter14BoldFont, &bitter14ItalicFont,
                                 &bitter14BoldItalicFont);
EpdFont bitter16RegularFont(&bitter_16_regular);
EpdFont bitter16BoldFont(&bitter_16_bold);
EpdFont bitter16ItalicFont(&bitter_16_italic);
EpdFont bitter16BoldItalicFont(&bitter_16_bolditalic);
EpdFontFamily bitter16FontFamily(&bitter16RegularFont, &bitter16BoldFont, &bitter16ItalicFont,
                                 &bitter16BoldItalicFont);
EpdFont bitter18RegularFont(&bitter_18_regular);
EpdFont bitter18BoldFont(&bitter_18_bold);
EpdFont bitter18ItalicFont(&bitter_18_italic);
EpdFont bitter18BoldItalicFont(&bitter_18_bolditalic);
EpdFontFamily bitter18FontFamily(&bitter18RegularFont, &bitter18BoldFont, &bitter18ItalicFont,
                                 &bitter18BoldItalicFont);

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
  QuickResume,  // wake from a quick-resume deep sleep (SD flag; survives power loss)
};

// Latched true once enterDeepSleep() commits to sleeping, before it tears down
// the current activity. WiFi activities call silentRestart() in onExit() to
// clear heap fragmentation on the way out, but deep sleep is a full chip reset
// on wake and already clears the heap, so rebooting here would just power the
// device back up against the user's sleep gesture. Never cleared:
// startDeepSleep() does not return, so a set latch only ends at the wakeup reset.
static bool deepSleepInProgress = false;

void silentRestart() {
  if (deepSleepInProgress) return;  // sleeping supersedes the heap-defrag reboot
  silentRebootTarget = SILENT_REBOOT_TARGET_HOME;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  LOG_DBG("MAIN", "Silent restart (target=home)");
  // E-ink retains the previous frame until Home's first paint lands (~2-3s).
  // Without an overlay, users don't see the reboot and fire input through to
  // Home. Select on the default selectorIndex=0 then opens the most-recent
  // book, looking like a trampoline back to the reader they just exited.
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  delay(50);
  ESP.restart();
}

void silentRestartToReader() {
  if (deepSleepInProgress) return;  // sleeping supersedes the heap-defrag reboot
  silentRebootTarget = SILENT_REBOOT_TARGET_READER;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  LOG_DBG("MAIN", "Silent restart (target=reader)");
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  delay(50);
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
  HalFile file;
  if (!Storage.openFileForWrite("SLP", SLEEP_FRAME_FILE, file)) return;
  file.write(renderer.getFrameBuffer(), renderer.getBufferSize());
  file.close();
}

static bool loadSleepFrameBuffer() {
  HalFile file;
  if (!Storage.openFileForRead("SLP", SLEEP_FRAME_FILE, file)) return false;
  const size_t bufferSize = display.getBufferSize();
  const size_t bytesRead = file.read(display.getFrameBuffer(), bufferSize);
  file.close();
  if (bytesRead != bufferSize) {
    Storage.remove(SLEEP_FRAME_FILE);
    return false;
  }
  Storage.remove(SLEEP_FRAME_FILE);
  return true;
}

// Enter deep sleep mode
void enterDeepSleep(bool fromTimeout) {
  HalPowerManager::Lock powerLock;  // Ensure we are at normal CPU frequency for sleep preparation
  // Resume target for Quick Resume: reader only if we actually slept in a book.
  // Home / Settings / Library / etc. all wake to Home (Settings must never auto-resume).
  APP_STATE.lastSleepFromReader = activityManager.isReaderActivity();
  // Successful sleep from reader: clear crash-loop counter so the next QR can
  // open the book (loadCount>=2 forces Home as a boot-loop guard).
  if (APP_STATE.lastSleepFromReader) {
    APP_STATE.readerActivityLoadCount = 0;
  }

  const bool isQuickResumeSleep =
      SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::QUICK_RESUME ||
      (fromTimeout &&
       SETTINGS.quickResumeSleepScreen == CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT);
  APP_STATE.showBootScreen = !isQuickResumeSleep;

  APP_STATE.saveToFile();
  // Persist settings before power-off so remaps (and other in-RAM settings)
  // are not lost if a prior save failed or never ran.
  SETTINGS.saveToFile();

  SystemLog::logTiming("SLEEP", "enter fromTimeout=%d lastSleepFromReader=%d", fromTimeout ? 1 : 0,
                       APP_STATE.lastSleepFromReader ? 1 : 0);
  SystemLog::flush();

  // Commit to sleeping before goToSleep() runs the outgoing activity's onExit():
  // a WiFi activity would otherwise silentRestart() here and reboot instead.
  deepSleepInProgress = true;
  activityManager.goToSleep(fromTimeout);

  // Snapshot after the moon is on the FB so X3 wake re-seed matches the glass
  // (needed for no-flash differential). Moon is ink-only in the top chrome band.
  if (isQuickResumeSleep) {
    saveSleepFrameBuffer();
  }

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
  renderer.insertFont(BITTER_12_FONT_ID, bitter12FontFamily);
  renderer.insertFont(BITTER_14_FONT_ID, bitter14FontFamily);
  renderer.insertFont(BITTER_16_FONT_ID, bitter16FontFamily);
  renderer.insertFont(BITTER_18_FONT_ID, bitter18FontFamily);
  // UI_10/UI_12 alias Source Serif 12/14 — insert full families once.
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

void setup() {
  BoardConfig::holdPowerRails();

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
  SETTINGS.loadFromFile();
  Storage.installDateTimeCallback(&SETTINGS.clockUtcOffsetQ);
  HalSystem::checkPanic();
  APP_STATE.loadFromFile();

  // Wake cause before QR planning — flash/USB must not look like sleep-from-reader.
  const auto wakeupReason = gpio.getWakeupReason();

  // Detect Quick Resume → book early so we can skip non-critical boot work.
  // PowerButton covers: deep-sleep GPIO wake (X3 / X4+USB) and X4 battery latch
  // POWERON (MCU fully powered off in sleep). Flash/USB are never PowerButton.
  const bool qrToBook = wakeupReason == HalGPIO::WakeupReason::PowerButton &&
                        !APP_STATE.showBootScreen && APP_STATE.lastSleepFromReader &&
                        !APP_STATE.openEpubPath.empty() && APP_STATE.readerActivityLoadCount < 2;

  // Defer recents/KOReader/OPDS SD reads until after first ink on QR→book
  // (saves ~50–150ms and SD contention before the page is readable).
  if (!qrToBook) {
    RECENT_BOOKS.loadFromFile();
    KOREADER_STORE.loadFromFile();
    OPDS_STORE.loadFromFile();
  }
  I18N.setLanguage(static_cast<Language>(SETTINGS.language));
  UITheme::getInstance().reload();
  ButtonNavigator::setMappedInputManager(mappedInputManager);
  // Buffered rotating SD log for field performance captures (/.casper-logs/).
  SystemLog::begin();
  SystemLog::logTiming("BOOT", "settings_loaded millis=%lu qrBook=%d wake=%d",
                       static_cast<unsigned long>(millis()), qrToBook ? 1 : 0,
                       static_cast<int>(wakeupReason));

  // Flash / USB / unknown cold boots: drop sticky reader-wake flags so a later
  // power press cannot reopen the last book (flash → USB sleep → power loop).
  // Do NOT clear on PowerButton — that is a real sleep wake (incl. X4 battery).
  auto clearStickyReaderWake = []() {
    if (!APP_STATE.lastSleepFromReader && APP_STATE.showBootScreen) return;
    APP_STATE.lastSleepFromReader = false;
    APP_STATE.showBootScreen = true;
    APP_STATE.saveToFile();
    LOG_DBG("MAIN", "Cleared sticky lastSleepFromReader (non power-button boot)");
  };

  switch (wakeupReason) {
    case HalGPIO::WakeupReason::PowerButton:
      LOG_DBG("MAIN", "Verifying power button press duration (sleep wake)");
      if (!gpio.verifyPowerButtonWakeup(SETTINGS.getPowerButtonDuration(),
                                        SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP)) {
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
  LOG_DBG("MAIN", "Starting Casper version " CROSSPOINT_VERSION);

  // Resolve the single boot-presentation decision. Skipping the splash also
  // skips the panel-clearing pass and the X3 initial-full-sync arming (see
  // HalDisplay::begin), so the first paint is FAST_REFRESH (~500ms) over the
  // retained frame and input dispatches against a visible UI.
  //
  // QuickResume for PowerButton wakes (deep sleep OR X4 battery POWERON latch).
  // Flash / unknown cold boot → Splash. (Do not require ESP_RST_DEEPSLEEP only —
  // that forced a full reboot-feel on X4 unplugged wake.)
  const bool forceColdHome = wakeupReason != HalGPIO::WakeupReason::PowerButton;
  const BootResume resume = isSilentReboot              ? BootResume::Silent
                            : forceColdHome             ? BootResume::Splash
                            : !APP_STATE.showBootScreen ? BootResume::QuickResume
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
      // One-shot flag: re-arm splash for next non-QR boot (saved after first ink on QR→book).
      APP_STATE.showBootScreen = true;
      // Resume the screen we slept from: reader only if last sleep was in a book.
      const bool qrOpenBook = qrToBook && !mappedInputManager.isPressed(MappedInputManager::Button::Back);
      if (QrTimingLog::active()) {
        QrTimingLog::line("qr_plan openBook=%d pathEmpty=%d lastReader=%d loadCount=%u", qrOpenBook ? 1 : 0,
                          APP_STATE.openEpubPath.empty() ? 1 : 0, APP_STATE.lastSleepFromReader ? 1 : 0,
                          static_cast<unsigned>(APP_STATE.readerActivityLoadCount));
      }
      if (loadSleepFrameBuffer()) {
        if (QrTimingLog::active()) QrTimingLog::line("after loadSleepFrameBuffer");
        // Re-seed controller "previous" plane from the restored FB (X3 DTM1 / X4 RED).
        renderer.cleanupGrayscaleWithFrameBuffer();
        if (qrOpenBook) {
          // Leave sleep image on glass; first page FAST replaces it (no spinner flash).
          if (QrTimingLog::active()) QrTimingLog::line("sleep frame kept (no spinner; open book next)");
        } else {
          // Landing on Home: moon → loading icon.
          SleepChromeIcon::replaceAtTopChrome(renderer, LoadingIcon, LOADINGICON_WIDTH, LOADINGICON_HEIGHT);
          // Plain FAST only — do not use displayGrayscaleBase here (AA-pre-BW mid
          // is grayscale preconditioning and leaves white muddy if no greys follow).
          renderer.displayBuffer(HalDisplay::FAST_REFRESH);
          if (QrTimingLog::active()) QrTimingLog::line("after sleep-frame loading-icon panel update");
        }
      } else {
        if (QrTimingLog::active()) QrTimingLog::line("sleep_frame MISSING — fall back to splash");
        activityManager.goToBoot();
      }
      // QR→book: defer APP_STATE.save (showBootScreen) until after first ink — SD write
      // was ~200ms on the critical path. Non-book QR still saves now (home is less latency-critical).
      if (!qrOpenBook) {
        APP_STATE.saveToFile();
        if (QrTimingLog::active()) QrTimingLog::line("after showBootScreen saveToFile");
      }
      break;
    }
    case BootResume::Splash:
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
  } else if (resume != BootResume::QuickResume || !qrToBook ||
             mappedInputManager.isPressed(MappedInputManager::Button::Back)) {
    // Home for splash / after flash / non-reader sleep / Back held.
    // Only genuine QuickResume sleep-from-reader (qrToBook) auto-opens a book.
    if (qrToBook) {
      // Planned book open aborted (Back held) — load stores deferred for QR.
      RECENT_BOOKS.loadFromFile();
      KOREADER_STORE.loadFromFile();
      OPDS_STORE.loadFromFile();
    }
    activityManager.goHome();
  } else {
    // Sleep-from-reader QuickResume: open book. On QR, push first ink in setup
    // before waiting for power release — waitForPowerRelease used to block the
    // entire open until the user let go of the wake press.
    const auto path = APP_STATE.openEpubPath;
    APP_STATE.openEpubPath = "";
    APP_STATE.readerActivityLoadCount++;
    // Defer APP_STATE SD write on QR (showBootScreen + loadCount) until after first ink.
    if (resume != BootResume::QuickResume) {
      APP_STATE.saveToFile();
    }
    if (resume == BootResume::QuickResume) {
      ReaderActivity::setOpenHints(/*preferFastFirstRefresh=*/true,
                                   /*deferFirstPageTextAa=*/SETTINGS.textAntiAliasing != 0);
    }
    if (QrTimingLog::active()) {
      QrTimingLog::line("before goToReader path=%s lastReader=1 loadCount=%u", path.c_str(),
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
      // Now safe to touch SD for deferred boot work.
      APP_STATE.saveToFile();
      RECENT_BOOKS.loadFromFile();
      KOREADER_STORE.loadFromFile();
      OPDS_STORE.loadFromFile();
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

  gpio.setSharedConfirmPowerShortPressEmitsPower(SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP);
  gpio.update();
  halTiltSensor.update(SETTINGS.tiltPageTurn, SETTINGS.orientation, activityManager.isReaderActivity());

  renderer.setFadingFix(SETTINGS.fadingFix);

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
  // release dispatches the short or long action (CrossInk-style).
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
                           static_cast<unsigned long>(activityDuration),
                           static_cast<unsigned>(ESP.getFreeHeap()));
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
