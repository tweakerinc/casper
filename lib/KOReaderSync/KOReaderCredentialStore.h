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

// How manual "Sync Progress" resolves differences after fetching remote progress.
enum class KOReaderSyncBehavior : uint8_t {
  ASK_EVERY_TIME = 0,  // Preserve legacy behavior: always show Apply/Upload choices.
  SMART = 1,           // Auto-resolve simple cases using furthest progress.
};

// Auto-upload gate when Auto Upload on Close is enabled.
enum class AutoUploadType : uint8_t {
  TIME = 0,        // Upload if enough wall-clock minutes since last auto-upload *for this book*
  PERCENT = 1,     // Upload if book progress advanced enough since last auto-upload *for this book*
  ADAPTIVE = 2,  // Always upload on close (no time/percent gate); value 2 kept for existing settings JSON
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
  KOReaderSyncBehavior syncBehavior = KOReaderSyncBehavior::SMART;
  // Upload local progress when leaving a book (0 = off, 1 = on).
  uint8_t autoUploadOnClose = 0;
  AutoUploadType autoUploadType = AutoUploadType::TIME;
  // Time mode: minutes between auto-uploads (1..1440). Default 60 (1 hour).
  uint16_t autoUploadIntervalMinutes = 60;
  // Percent mode: minimum book-progress gain (1..25). Default 5.
  uint8_t autoUploadPercentThreshold = 5;

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
  static constexpr uint8_t DEFAULT_PERCENT_THRESHOLD = 5;

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

  // Send metadata setting
  void setSendMetadata(bool enabled);
  bool getSendMetadata() const { return sendMetadata; }

  // Sync behavior
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
  // Time mode: upload if never uploaded this book, or interval elapsed since that book's last auto-upload.
  // Percent mode: upload if never uploaded this book, or progress advanced by >= threshold
  //   (e.g. last=80%, threshold=1% → current >= 81%).
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
