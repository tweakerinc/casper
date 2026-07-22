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
 * Interactive flow:
 * 1. Connect to WiFi (if not connected)
 * 2. Calculate document hash
 * 3. Fetch remote progress
 * 4. Show comparison and options (Apply/Upload)
 * 5. Apply or upload progress
 *
 * Auto-upload flow (book close with setting enabled):
 * 1. Connect to WiFi (auto-connect when possible)
 * 2. Calculate document hash
 * 3. Upload local progress only
 * 4. Silent-restart to home
 */
class KOReaderSyncActivity final : public Activity {
 public:
  explicit KOReaderSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& epubPath,
                                int currentSpineIndex, int currentPage, int totalPagesInSpine,
                                KOReaderPosition localKoPos, std::string localChapterName,
                                std::optional<uint16_t> currentParagraphIndex = std::nullopt,
                                bool autoUploadOnly = false, float autoUploadBookPercent = -1.0f)
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
        autoUploadBookPercent(autoUploadBookPercent) {}
  // epubPath is already stored; used when stamping auto-upload success.

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == CONNECTING || state == SYNCING || state == UPLOADING; }
  bool isReaderActivity() const override { return true; }
  bool allowPowerAsConfirmInReaderMode() const override { return true; }

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
  std::string documentHash;

  // Remote progress data
  bool hasRemoteProgress = false;
  KOReaderProgress remoteProgress;
  CrossPointPosition remotePosition;

  // Local progress as KOReader format (pre-computed before Epub was released)
  KOReaderPosition localProgress;

  // When true: skip compare UI, upload local progress, restart to home.
  bool autoUploadOnly = false;
  // Book progress percent at auto-upload start (for percent-threshold baseline).
  float autoUploadBookPercent = -1.0f;

  // Selection in result screen (0=Apply, 1=Upload)
  int selectedOption = 0;

  // Timed return for successful smart-sync terminal states.
  unsigned long autoReturnAt = 0;
  static constexpr unsigned long AUTO_RETURN_DELAY_MS = 1200;

  // Tracks whether this session activated WiFi. Set in onEnter past the credentials
  // check; checked in onExit to decide whether to silent-reboot. Can't rely on
  // WiFi.getMode() because performUpload() calls esp_wifi_stop() on the way out,
  // which makes WiFi.getMode() return WIFI_MODE_NULL.
  bool wifiActivated = false;
  bool lockInitialConfirmRelease = false;
  // When true, onExit must not silent-restart (exit destination already handled).
  bool exitHandled = false;

  void onWifiSelectionComplete(bool success);
  void performSync();
  void performAutoUpload();
  void performUpload();
  void finishAutoMode();
  bool consumeInitialConfirmRelease();
  bool smartSyncEnabled() const;
  void markAutoReturn();
  void completeAlreadySynced();
  void ensureEpubLoaded();
  void saveProgressAndReturn(const CrossPointPosition& position);
  void returnToReader();
};
