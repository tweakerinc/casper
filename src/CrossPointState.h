#pragma once
#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <cstdint>
#include <string>

class CrossPointState : public PersistableStore<CrossPointState> {
  CrossPointState() = default;

  friend class PersistableStore<CrossPointState>;

 public:
  static constexpr uint8_t SLEEP_RECENT_COUNT = 16;

  // Where sleep wake should land after power press (persisted across deep sleep).
  enum SleepResumeTarget : uint8_t {
    RESUME_HOME = 0,
    RESUME_READER = 1,
    RESUME_SETTINGS = 2,
    RESUME_READER_MENU = 3,
  };

  std::string openEpubPath;
  uint16_t recentSleepImages[SLEEP_RECENT_COUNT] = {};  // circular buffer of recent wallpaper indices
  uint8_t recentSleepPos = 0;                           // next write slot
  uint8_t recentSleepFill = 0;                          // valid entries (0..SLEEP_RECENT_COUNT)
  uint8_t readerActivityLoadCount = 0;
  bool lastSleepFromReader = false;
  // True except while sleeping. enterDeepSleep sets false; wake restores true.
  // X4 battery POWERON cannot tell sleep-wake from EN-reset+power; this flag
  // is the split (see bootwake::x4PowerOnIsSleepWake).
  bool showBootScreen = true;
  // Seamless sleep destination (home / reader / settings / reader menu).
  uint8_t sleepResumeTarget = RESUME_HOME;
  // True when Sleep Screen was Quick Resume (last-frame + moon) so wake can
  // re-seed sleep_frame and swap moon → dots.
  bool lastSleepRenderedQuickResume = false;
  // True when QR sleep left 2-bit greys on glass (home cover / AA). Wake must
  // not window moon→dots; that FAST/window flattens the grey pass to BW.
  bool lastSleepQrHeldGreyscale = false;

  static const char* getFilePath() { return "/.crosspoint/state.json"; }

  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  // Returns true if idx was shown within the last checkCount picks.
  // Walks backwards from the most recently written slot.
  bool isRecentSleep(uint16_t idx, uint8_t checkCount) const;

  void pushRecentSleep(uint16_t idx);
};

// Helper macro to access state
#define APP_STATE CrossPointState::getInstance()
