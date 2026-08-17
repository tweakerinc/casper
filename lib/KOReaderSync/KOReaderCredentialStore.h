#pragma once
#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <cstdint>
#include <string>

// Document matching method for KOReader sync
enum class DocumentMatchMethod : uint8_t {
  FILENAME = 0,  // Match by filename (simpler, works across different file sources)
  BINARY = 1,    // Match by partial MD5 of file content (more accurate, but files must be identical)
};

// Sync Behavior — mutually exclusive modes (device popup + web).
// Stored values stay fixed for JSON compatibility. UI display order is different
// (Off, Smart Sync, Ask Every Time, Percent, Time) — use syncBehaviorToDisplay /
// syncBehaviorFromDisplay for menus.
// Off: no leave sync, no manual sync, no quiet upload (feature off). Default for new installs.
// Ask: leave asks "Sync Progress?" first; if yes, full Apply/Upload UI (never silent).
// Smart: leave auto-resolves furthest progress (upload / apply / already synced).
// Percent / Time: leave auto-upload when gate met; if remote is further, ask how to proceed.
enum class KOReaderSyncBehavior : uint8_t {
  ASK_EVERY_TIME = 0,  // Leave: confirm first. Manual: always choose Apply/Upload.
  SMART = 1,           // Leave + manual: auto furthest-ahead (remote ahead applies without ask).
  PERCENT = 2,         // Leave: auto when +% gate met; remote ahead → ask.
  TIME = 3,            // Leave: auto when time gate met; remote ahead → ask.
  // Named OFF (not DISABLED): esp32-hal-gpio.h #defines DISABLED as 0x00.
  OFF = 4,  // Fully off. Appended so existing 0–3 JSON values stay valid.
  COUNT = 5,
};

// Menu order: Off, Smart Sync, Ask Every Time, Percent, Time.
inline uint8_t syncBehaviorToDisplay(KOReaderSyncBehavior b) {
  switch (b) {
    case KOReaderSyncBehavior::OFF:
      return 0;
    case KOReaderSyncBehavior::SMART:
      return 1;
    case KOReaderSyncBehavior::ASK_EVERY_TIME:
      return 2;
    case KOReaderSyncBehavior::PERCENT:
      return 3;
    case KOReaderSyncBehavior::TIME:
      return 4;
    default:
      return 0;
  }
}
inline KOReaderSyncBehavior syncBehaviorFromDisplay(uint8_t displayIndex) {
  switch (displayIndex) {
    case 0:
      return KOReaderSyncBehavior::OFF;
    case 1:
      return KOReaderSyncBehavior::SMART;
    case 2:
      return KOReaderSyncBehavior::ASK_EVERY_TIME;
    case 3:
      return KOReaderSyncBehavior::PERCENT;
    case 4:
      return KOReaderSyncBehavior::TIME;
    default:
      return KOReaderSyncBehavior::OFF;
  }
}

// Legacy single-gate auto-upload type (kept for JSON compatibility).
// Device UI configures Time and Percent together; evaluate uses dual OR gates
// (upload if either time or percent condition is met). ADAPTIVE = always upload.
enum class AutoUploadType : uint8_t {
  TIME = 0,      // Prefer time gate (still OR'd with percent in evaluate)
  PERCENT = 1,   // Prefer percent gate (still OR'd with time in evaluate)
  ADAPTIVE = 2,  // Always upload on close (no time/percent gate)
};

// Why auto-upload ran or was skipped (for user-facing toasts).
enum class AutoUploadDecision : uint8_t {
  Upload = 0,
  SkipDisabled,
  SkipNoCredentials,
  SkipTimeNotElapsed,
  SkipPercentNotMet,
  SkipInvalid,
};

/**
 * Singleton class for storing KOReader sync credentials on the SD card.
 * Passwords are XOR-obfuscated with the device's unique hardware MAC address
 * and base64-encoded before writing to JSON (not cryptographically secure,
 * but prevents casual reading and ties credentials to the specific device).
 */
class KOReaderCredentialStore : public PersistableStore<KOReaderCredentialStore> {
 private:
  static constexpr size_t kMaxBookStates = 32;
  static constexpr size_t kBookKeyLen = 16;  // hex FNV-1a 64-bit (16 chars + NUL)

  struct BookAutoUploadState {
    char key[kBookKeyLen + 1] = {};
    uint32_t lastUnix = 0;      // 0 = never
    float lastPercent = -1.0f;  // <0 = never
  };

  std::string username;
  std::string password;
  std::string serverUrl;                                            // Custom sync server URL (empty = default)
  DocumentMatchMethod matchMethod = DocumentMatchMethod::FILENAME;  // Default to filename for compatibility
  bool sendMetadata = false;                                        // Send document metadata with progress sync
  KOReaderSyncBehavior syncBehavior = KOReaderSyncBehavior::OFF;
  // Upload local progress when leaving a book (0 = off, 1 = on).
  uint8_t autoUploadOnClose = 0;
  AutoUploadType autoUploadType = AutoUploadType::TIME;
  // Time gate: minutes between auto-uploads (0 = Always). Default 60 (1 hour).
  uint16_t autoUploadIntervalMinutes = 60;
  // Percent gate: minimum book-progress gain (0 = Always). Default 1%.
  uint8_t autoUploadPercentThreshold = 1;

  BookAutoUploadState bookStates[kMaxBookStates] = {};
  size_t bookStateCount = 0;

  // Legacy global stamps (migrated into per-book state on first use).
  uint32_t legacyLastAutoUploadUnix = 0;
  float legacyLastAutoUploadPercent = -1.0f;

  KOReaderCredentialStore() = default;
  ~KOReaderCredentialStore() = default;

  friend class PersistableStore<KOReaderCredentialStore>;

  static void makeBookKey(const char* bookPath, char* outKey, size_t outKeyLen);
  const BookAutoUploadState* findBookState(const char* key) const;
  BookAutoUploadState* findOrCreateBookState(const char* key);

 public:
  // 0 = Always (upload every book exit when Time/Percent mode is selected).
  static constexpr uint16_t ALWAYS_INTERVAL_MINUTES = 0;
  static constexpr uint16_t MIN_INTERVAL_MINUTES = 0;
  static constexpr uint16_t MAX_INTERVAL_MINUTES = 24 * 60;  // 24 hours
  static constexpr uint16_t DEFAULT_INTERVAL_MINUTES = 60;
  static constexpr uint8_t ALWAYS_PERCENT_THRESHOLD = 0;
  static constexpr uint8_t MIN_PERCENT_THRESHOLD = 0;
  static constexpr uint8_t MAX_PERCENT_THRESHOLD = 25;
  static constexpr uint8_t DEFAULT_PERCENT_THRESHOLD = 1;

  static const char* getFilePath() { return "/.crosspoint/koreader.json"; }

  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  void setCredentials(const std::string& user, const std::string& pass);
  const std::string& getUsername() const { return username; }
  const std::string& getPassword() const { return password; }
  std::string getMd5Password() const;
  bool hasCredentials() const;
  void clearCredentials();

  void setServerUrl(const std::string& url);
  const std::string& getServerUrl() const { return serverUrl; }
  std::string getBaseUrl() const;

  void setMatchMethod(DocumentMatchMethod method);
  DocumentMatchMethod getMatchMethod() const { return matchMethod; }

  void setSendMetadata(bool enabled);
  bool getSendMetadata() const { return sendMetadata; }

  void setSyncBehavior(KOReaderSyncBehavior behavior);
  KOReaderSyncBehavior getSyncBehavior() const { return syncBehavior; }

  void setAutoUploadOnClose(bool enabled);
  bool getAutoUploadOnClose() const { return autoUploadOnClose != 0; }

  void setAutoUploadType(AutoUploadType type);
  AutoUploadType getAutoUploadType() const { return autoUploadType; }

  void setAutoUploadIntervalMinutes(uint16_t minutes);
  uint16_t getAutoUploadIntervalMinutes() const { return autoUploadIntervalMinutes; }

  void setAutoUploadPercentThreshold(uint8_t percent);
  uint8_t getAutoUploadPercentThreshold() const { return autoUploadPercentThreshold; }

  // bookPath identifies the open book (per-book time/percent baselines).
  AutoUploadDecision evaluateAutoUpload(const char* bookPath, float currentBookPercent = -1.0f) const;
  bool shouldAutoUploadNow(const char* bookPath, float currentBookPercent = -1.0f) const {
    return evaluateAutoUpload(bookPath, currentBookPercent) == AutoUploadDecision::Upload;
  }
  // Call after a successful upload (auto or manual) so baselines stay in sync.
  void markAutoUploadSucceeded(const char* bookPath, float currentBookPercent = -1.0f);

  static uint16_t clampIntervalMinutes(uint16_t minutes);
  static uint8_t clampPercentThreshold(uint8_t percent);
};

#define KOREADER_STORE KOReaderCredentialStore::getInstance()
