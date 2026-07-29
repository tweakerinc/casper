#pragma once
#include <Epub.h>

#include <functional>
#include <memory>
#include <optional>

#include "KOReaderSyncClient.h"
#include "ProgressMapper.h"
#include "activities/Activity.h"

/**
 * Activity for syncing reading progress with KOReader sync server.
 *
 * Manual: full WiFi picker → fetch → Ask or Smart resolve → reader.
 * Leave Percent/Time (quiet): full-page status (no framebuffer snapshot — TLS heap)
 *   + saved-network connect → smart-safe upload (ask if remote further) → home.
 * Leave Ask/Smart: full sync UI, finish at home.
 */
class KOReaderSyncActivity final : public Activity {
 public:
  explicit KOReaderSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& epubPath,
                                int currentSpineIndex, int currentPage, int totalPagesInSpine,
                                SavedProgressPosition localKoPos, std::string localChapterName,
                                std::optional<uint16_t> currentParagraphIndex = std::nullopt,
                                bool autoUploadOnly = false, float autoUploadBookPercent = -1.0f,
                                bool leaveToHome = false)
      : Activity("KOReaderSync", renderer, mappedInput),
        epubPath(epubPath),
        currentSpineIndex(currentSpineIndex),
        currentPage(currentPage),
        totalPagesInSpine(totalPagesInSpine),
        currentParagraphIndex(currentParagraphIndex),
        localChapterName(std::move(localChapterName)),
        remoteProgress{},
        remotePosition{},
        localProgress(std::move(localKoPos)),
        autoUploadOnly(autoUploadOnly),
        autoUploadBookPercent(autoUploadBookPercent),
        leaveToHome(leaveToHome) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override {
    return state == CONNECTING || state == SYNCING || state == UPLOADING || quietWifiPending;
  }

 private:
  enum State {
    WIFI_SELECTION,
    CONNECTING,
    SYNCING,
    SHOWING_RESULT,
    UPLOADING,
    UPLOAD_COMPLETE,
    SYNC_COMPLETE,
    NO_REMOTE_PROGRESS,
    SYNC_FAILED,
    NO_CREDENTIALS
  };

  std::shared_ptr<Epub> epub;  // null until lazy-loaded after TLS in performSync()
  std::string epubPath;
  std::string localChapterName;
  int currentSpineIndex;
  int currentPage;
  int totalPagesInSpine;
  std::optional<uint16_t> currentParagraphIndex;

  State state = WIFI_SELECTION;
  std::string statusMessage;
  // Optional second line under statusMessage (SSID, phase hint, error detail).
  std::string statusDetail;
  std::string documentHash;

  // Remote progress data
  bool hasRemoteProgress = false;
  KOReaderProgress remoteProgress;
  CrossPointPosition remotePosition;

  // Local progress as KOReader format (pre-computed before Epub was released)
  SavedProgressPosition localProgress;

  // When true: Percent/Time leave path (quiet WiFi + smart-safe upload).
  bool autoUploadOnly = false;
  float autoUploadBookPercent = -1.0f;
  // When true (book leave): finish at home instead of returning to the reader.
  bool leaveToHome = false;

  // Selection in result screen (0=Apply, 1=Upload)
  int selectedOption = 0;

  // Timed return for successful smart-sync terminal states.
  unsigned long autoReturnAt = 0;
  static constexpr unsigned long AUTO_RETURN_DELAY_MS = 1200;
  static constexpr unsigned long QUIET_WIFI_TIMEOUT_MS = 8000;

  // Tracks whether this session activated WiFi. Set in onEnter past the credentials
  // check; checked in onExit to decide whether to silent-reboot. Can't rely on
  // WiFi.getMode() because performUpload() calls esp_wifi_stop() on the way out,
  // which makes WiFi.getMode() return WIFI_MODE_NULL.
  bool wifiActivated = false;
  // When true, onExit must not silent-restart (exit destination already handled).
  bool exitHandled = false;
  // Quiet leave path: connect saved SSIDs without WifiSelectionActivity UI.
  bool quietWifiPending = false;
  size_t quietWifiCredIndex = 0;
  size_t quietWifiAttempts = 0;
  unsigned long quietWifiStartMs = 0;
  bool quietWifiBeginIssued = false;

  void onWifiSelectionComplete(bool success);
  void performSync();
  void performAutoUpload();
  void performUpload();
  void finishAutoMode();
  bool smartSyncEnabled() const;
  bool shouldFinishAtHome() const { return autoUploadOnly || leaveToHome; }
  bool useQuietWifi() const { return autoUploadOnly || leaveToHome; }
  void finishToDestination();  // home if leave/auto, else reader
  void markAutoReturn();
  void completeAlreadySynced();
  void ensureEpubLoaded();
  void saveProgressAndReturn(int spineIndex, int page);
  void returnToReader();
  void startQuietWifiConnect();
  void tickQuietWifiConnect();
  bool mapRemoteProgressForCompare();
  // Full-page status for leave-sync (Connecting / Syncing / result) — no popup snapshot.
  void drawFullPageStatus(const char* message, const char* detail = nullptr) const;
  bool useQuietLeaveUi() const { return leaveToHome || autoUploadOnly; }
  // Set status, paint, and wait for the e-ink frame before blocking work (TLS/NTP/hash).
  // Without the wait, leave-sync looks frozen on "Connecting…" while Wi‑Fi/sync runs.
  void showBusyStatus(State nextState, const char* message, const char* detail = nullptr);
};
