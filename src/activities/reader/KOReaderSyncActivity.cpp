#include "KOReaderSyncActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Arduino.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <esp_wifi.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>

#include "Epub/Section.h"
#include "EpubReaderUtils.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderDocumentId.h"
#include "MappedInputManager.h"
#include "ReaderUtils.h"
#include "SilentRestart.h"
#include "WifiCredentialStore.h"
#include "activities/ActivityManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
std::string calculateDocumentHashForMethod(const std::string& path, const DocumentMatchMethod method) {
  return method == DocumentMatchMethod::FILENAME ? KOReaderDocumentId::calculateFromFilename(path)
                                                 : KOReaderDocumentId::calculate(path);
}

DocumentMatchMethod alternateMatchMethod(const DocumentMatchMethod method) {
  return method == DocumentMatchMethod::FILENAME ? DocumentMatchMethod::BINARY : DocumentMatchMethod::FILENAME;
}

const char* matchMethodName(const DocumentMatchMethod method) {
  return method == DocumentMatchMethod::FILENAME ? "filename" : "binary";
}

void syncTimeWithNTP() {
  // Stop SNTP if already running (can't reconfigure while running)
  if (esp_sntp_enabled()) {
    esp_sntp_stop();
  }

  // Configure SNTP
  esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
  esp_sntp_setservername(0, "pool.ntp.org");
  esp_sntp_init();

  // Wait for time to sync (with timeout)
  int retry = 0;
  const int maxRetries = 50;  // 5 seconds max
  while (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED && retry < maxRetries) {
    vTaskDelay(100 / portTICK_PERIOD_MS);
    retry++;
  }

  if (retry < maxRetries) {
    LOG_DBG("KOSync", "NTP time synced");
  } else {
    LOG_DBG("KOSync", "NTP sync timeout, using fallback");
  }
}
}  // namespace

void KOReaderSyncActivity::ensureEpubLoaded() {
  if (!epub) {
    LOG_DBG("KOSync", "Loading epub for progress mapping (heap: %u)", (unsigned)ESP.getFreeHeap());
    epub = std::make_shared<Epub>(epubPath, "/.crosspoint");
    epub->setupCacheDir();
    // Load metadata only (no CSS needed for progress mapping, don't rebuild if cache is missing).
    if (!epub->load(false, true)) {
      LOG_ERR("KOSync", "Failed to load epub for progress mapping");
      epub.reset();
      return;
    }
    LOG_DBG("KOSync", "Epub loaded (heap: %u)", (unsigned)ESP.getFreeHeap());
  }
}

void KOReaderSyncActivity::saveProgressAndReturn(int spineIndex, int page) {
  // epub is guaranteed non-null here: ensureEpubLoaded() was called in performSync() before
  // SHOWING_RESULT state is entered, and this method is only called from that state.
  assert(epub);
  if (!EpubReaderUtils::saveProgress(*epub, spineIndex, page, 0)) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = tr(STR_SAVE_PROGRESS_FAILED);
    }
    requestUpdate(true);
    return;
  }
  finishToDestination();
}

void KOReaderSyncActivity::returnToReader() { activityManager.goToReader(epubPath); }

bool KOReaderSyncActivity::smartSyncEnabled() const {
  return KOREADER_STORE.getSyncBehavior() == KOReaderSyncBehavior::SMART;
}

void KOReaderSyncActivity::markAutoReturn() { autoReturnAt = millis() + AUTO_RETURN_DELAY_MS; }

void KOReaderSyncActivity::completeAlreadySynced() {
  {
    RenderLock lock(*this);
    state = SYNC_COMPLETE;
  }
  markAutoReturn();
  requestUpdate(true);
  if (shouldFinishAtHome()) {
    delay(600);
    finishToDestination();
  }
}

void KOReaderSyncActivity::finishAutoMode() {
  exitHandled = true;
  esp_wifi_stop();
  wifiActivated = false;
  LOG_DBG("KOSync", "Sync finished on leave, restarting to home");
  silentRestart();
}

void KOReaderSyncActivity::finishToDestination() {
  if (shouldFinishAtHome()) {
    finishAutoMode();
  } else {
    returnToReader();
  }
}

void KOReaderSyncActivity::showBusyStatus(const State nextState, const char* message, const char* detail) {
  {
    RenderLock lock(*this);
    state = nextState;
    statusMessage = message ? message : "";
    statusDetail = detail ? detail : "";
  }
  // Wait until the frame is on screen — leave-sync blocks the main task for
  // NTP / hash / TLS next, so requestUpdate(true) alone never paints in time.
  requestUpdateAndWait();
}

void KOReaderSyncActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    LOG_DBG("KOSync", "WiFi connection failed, exiting");
    finishToDestination();
    return;
  }

  LOG_DBG("KOSync", "WiFi connected, starting sync");

  // Paint each phase before blocking work so leave-sync never looks frozen.
  showBusyStatus(SYNCING, tr(STR_SYNCING_TIME), WiFi.SSID().c_str());

  // Sync time with NTP before making API requests (up to ~5s).
  syncTimeWithNTP();

  showBusyStatus(SYNCING, tr(STR_CALC_HASH), nullptr);

  if (autoUploadOnly) {
    performAutoUpload();
  } else {
    performSync();
  }
}

bool KOReaderSyncActivity::mapRemoteProgressForCompare() {
  ensureEpubLoaded();
  if (!epub) {
    return false;
  }
  hasRemoteProgress = true;
  std::optional<CrossPointPosition> richMapped;
  if (remoteProgress.position.has_value()) {
    richMapped = ProgressMapper::fromRichPosition(epub, *remoteProgress.position, renderer);
  }
  if (richMapped.has_value()) {
    remotePosition = *richMapped;
  } else {
    SavedProgressPosition koPos = {remoteProgress.progress, remoteProgress.percentage};
    remotePosition = ProgressMapper::toCrossPoint(epub, koPos, renderer, currentSpineIndex, totalPagesInSpine);
  }
  return true;
}

void KOReaderSyncActivity::performAutoUpload() {
  // Quiet leave path (Percent/Time): still protect against overwriting a further
  // remote position — same furthest-ahead idea as Smart, but only pause for a
  // choice when remote is ahead.
  const DocumentMatchMethod primaryMethod = KOREADER_STORE.getMatchMethod();
  documentHash = calculateDocumentHashForMethod(epubPath, primaryMethod);
  if (documentHash.empty()) {
    showBusyStatus(SYNC_FAILED, tr(STR_SYNC_FAILED_MSG), tr(STR_HASH_FAILED));
    delay(1500);
    finishAutoMode();
    return;
  }
  const std::string primaryHash = documentHash;
  LOG_DBG("KOSync", "Quiet leave sync hash (%s): %s", matchMethodName(primaryMethod), documentHash.c_str());

  showBusyStatus(SYNCING, tr(STR_FETCH_PROGRESS), nullptr);

  auto result = KOReaderSyncClient::getProgress(documentHash, remoteProgress);
  // Probe alternate document-id method so we don't miss progress from another matching mode.
  {
    const DocumentMatchMethod altMethod = alternateMatchMethod(primaryMethod);
    const std::string altHash = calculateDocumentHashForMethod(epubPath, altMethod);
    if (!altHash.empty() && altHash != documentHash) {
      KOReaderProgress altProgress;
      const auto altResult = KOReaderSyncClient::getProgress(altHash, altProgress);
      if (altResult == KOReaderSyncClient::OK &&
          (result == KOReaderSyncClient::NOT_FOUND || altProgress.percentage > remoteProgress.percentage)) {
        documentHash = altHash;
        remoteProgress = std::move(altProgress);
        result = KOReaderSyncClient::OK;
      }
    }
  }

  if (result == KOReaderSyncClient::NOT_FOUND) {
    LOG_DBG("KOSync", "Quiet leave: no remote progress; uploading local %.6f", localProgress.percentage);
    documentHash = primaryHash;
    performUpload();
    return;
  }
  if (result != KOReaderSyncClient::OK) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = KOReaderSyncClient::errorString(result);
    }
    markAutoReturn();  // allow timed exit; Back also works immediately
    requestUpdate(true);
    return;
  }

  static constexpr float SAME_PROGRESS_EPSILON = 0.001f;
  const float delta = localProgress.percentage - remoteProgress.percentage;
  LOG_DBG("KOSync", "Quiet leave compare local=%.6f remote=%.6f delta=%.6f", localProgress.percentage,
          remoteProgress.percentage, delta);

  if (std::fabs(delta) <= SAME_PROGRESS_EPSILON) {
    // Still stamp so Percent/Time gates advance after a no-op sync.
    {
      float stampPercent = autoUploadBookPercent;
      if (stampPercent < 0.0f && localProgress.percentage >= 0.0f) {
        stampPercent = localProgress.percentage * 100.0f;
      }
      KOREADER_STORE.markAutoUploadSucceeded(epubPath.c_str(), stampPercent);
    }
    completeAlreadySynced();
    return;
  }

  if (delta > 0) {
    // Local further — safe to push. Prefer configured match method for the record.
    documentHash = primaryHash;
    performUpload();
    return;
  }

  // Remote further — do not silently overwrite; ask Apply Remote vs Upload Local.
  if (!mapRemoteProgressForCompare()) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = tr(STR_SYNC_FAILED_MSG);
    }
    requestUpdate(true);
    delay(1500);
    finishAutoMode();
    return;
  }
  {
    RenderLock lock(*this);
    state = SHOWING_RESULT;
    selectedOption = 0;  // default: keep remote (safer when remote is ahead)
  }
  requestUpdate(true);
}

void KOReaderSyncActivity::performSync() {
  const DocumentMatchMethod primaryMethod = KOREADER_STORE.getMatchMethod();
  documentHash = calculateDocumentHashForMethod(epubPath, primaryMethod);
  if (documentHash.empty()) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = tr(STR_HASH_FAILED);
      statusDetail.clear();
    }
    if (shouldFinishAtHome()) {
      markAutoReturn();
    }
    requestUpdateAndWait();
    return;
  }
  const std::string primaryHash = documentHash;

  LOG_DBG("KOSync", "Document hash (%s): %s", matchMethodName(primaryMethod), documentHash.c_str());

  showBusyStatus(SYNCING, tr(STR_FETCH_PROGRESS), nullptr);

  // Fetch remote progress. In smart mode, also probe the alternate document-id
  // method and use the furthest remote state we can find. This avoids a stale
  // local upload when another KOReader device synced the same book with a
  // different document matching method.
  auto result = KOReaderSyncClient::getProgress(documentHash, remoteProgress);
  LOG_DBG("KOSync", "Primary remote (%s): result=%d http=%d doc=%s local=%.6f remote=%.6f xpath=%s",
          matchMethodName(primaryMethod), result, KOReaderSyncClient::lastHttpCode, documentHash.c_str(),
          localProgress.percentage, remoteProgress.percentage, remoteProgress.progress.c_str());

  if (smartSyncEnabled()) {
    const DocumentMatchMethod altMethod = alternateMatchMethod(primaryMethod);
    const std::string altHash = calculateDocumentHashForMethod(epubPath, altMethod);
    if (!altHash.empty() && altHash != documentHash) {
      KOReaderProgress altProgress;
      const auto altResult = KOReaderSyncClient::getProgress(altHash, altProgress);
      LOG_DBG("KOSync", "Alternate remote (%s): result=%d http=%d doc=%s local=%.6f remote=%.6f xpath=%s",
              matchMethodName(altMethod), altResult, KOReaderSyncClient::lastHttpCode, altHash.c_str(),
              localProgress.percentage, altProgress.percentage, altProgress.progress.c_str());

      if (altResult == KOReaderSyncClient::OK &&
          (result == KOReaderSyncClient::NOT_FOUND || altProgress.percentage > remoteProgress.percentage)) {
        documentHash = altHash;
        remoteProgress = std::move(altProgress);
        result = KOReaderSyncClient::OK;
      }
    }
  }

  if (result == KOReaderSyncClient::NOT_FOUND) {
    if (smartSyncEnabled()) {
      LOG_DBG("KOSync", "Smart sync: no remote progress found for known document hashes; uploading local %.6f",
              localProgress.percentage);
      performUpload();
      return;
    }

    // No remote progress - offer to upload
    {
      RenderLock lock(*this);
      state = NO_REMOTE_PROGRESS;
      hasRemoteProgress = false;
    }
    requestUpdate(true);
    return;
  }

  if (result != KOReaderSyncClient::OK) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = KOReaderSyncClient::errorString(result);
    }
    if (shouldFinishAtHome()) {
      markAutoReturn();
    }
    requestUpdate(true);
    return;
  }

  // Epub was released before sync to free RAM for the TLS handshake — reload it now.
  hasRemoteProgress = true;
  ensureEpubLoaded();
  if (!epub) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = tr(STR_SYNC_FAILED_MSG);
    }
    if (shouldFinishAtHome()) {
      markAutoReturn();
    }
    requestUpdate(true);
    return;
  }

  // Prefer the exact spine/page from a crosspoint-sync rich position (lossless
  // CrossPoint<->CrossPoint sync); fall back to the approximate XPath mapping
  // for plain kosync servers or when the rich position cannot be applied.
  std::optional<CrossPointPosition> richMapped;
  if (remoteProgress.position.has_value()) {
    richMapped = ProgressMapper::fromRichPosition(epub, *remoteProgress.position, renderer);
  }
  if (richMapped.has_value()) {
    remotePosition = *richMapped;
  } else {
    SavedProgressPosition koPos = {remoteProgress.progress, remoteProgress.percentage};
    remotePosition = ProgressMapper::toCrossPoint(epub, koPos, renderer, currentSpineIndex, totalPagesInSpine);
  }

  if (smartSyncEnabled()) {
    static constexpr float SAME_PROGRESS_EPSILON = 0.001f;  // 0.1 percentage points
    const float delta = localProgress.percentage - remoteProgress.percentage;
    LOG_DBG("KOSync", "Smart decision: doc=%s local=%.6f remote=%.6f delta=%.6f remoteXpath=%s mapped=%d/%d",
            documentHash.c_str(), localProgress.percentage, remoteProgress.percentage, delta,
            remoteProgress.progress.c_str(), remotePosition.spineIndex, remotePosition.pageNumber);
    if (std::fabs(delta) <= SAME_PROGRESS_EPSILON) {
      completeAlreadySynced();
      return;
    }

    if (delta > 0) {
      // Alternate hashes are only probes for newer remote state. Keep uploads
      // on the user's configured matching method so its primary record heals.
      documentHash = primaryHash;
      performUpload();
      return;
    }

    saveProgressAndReturn(remotePosition.spineIndex, remotePosition.pageNumber);
    return;
  }

  // localProgress was pre-computed in EpubReaderActivity before the Epub was released.
  {
    RenderLock lock(*this);
    state = SHOWING_RESULT;

    // Default to the option that corresponds to the furthest progress
    if (localProgress.percentage > remoteProgress.percentage) {
      selectedOption = 1;  // Upload local progress
    } else {
      selectedOption = 0;  // Apply remote progress
    }
  }
  requestUpdate(true);
}

void KOReaderSyncActivity::performUpload() {
  showBusyStatus(UPLOADING, tr(STR_UPLOAD_PROGRESS), nullptr);

  // localProgress was pre-computed in EpubReaderActivity before the Epub was released.
  KOReaderProgress progress;
  progress.document = documentHash;
  progress.progress = localProgress.xpath;
  progress.percentage = localProgress.percentage;

  // Rich CrossPoint position for crosspoint-sync servers (lossless
  // CrossPoint<->CrossPoint sync); plain kosync servers ignore the extra field.
  {
    KOReaderRichPosition pos;
    const float pct = localProgress.percentage < 0.0f   ? 0.0f
                      : localProgress.percentage > 1.0f ? 1.0f
                                                        : localProgress.percentage;
    pos.pctQ = static_cast<uint32_t>(pct * 1000000.0f + 0.5f);
    pos.spineIndex = static_cast<uint16_t>(currentSpineIndex);
    pos.pageNumber = static_cast<uint16_t>(currentPage);
    pos.totalPages = static_cast<uint16_t>(totalPagesInSpine > 0 ? totalPagesInSpine : 1);
    pos.paragraphIndex = currentParagraphIndex;
    pos.xpath = localProgress.xpath;
    progress.position = std::move(pos);
  }

  // Optionally include document metadata (KOReader PR #15306)
  if (KOREADER_STORE.getSendMetadata()) {
    // The Epub is released before the sync network calls and is only reloaded on the
    // remote-progress path (performSync). When uploading from NO_REMOTE_PROGRESS the
    // Epub is still null, so reload it here and guard the title/author reads to avoid
    // dereferencing a null Epub. Filename is derived from the path and is always safe.
    ensureEpubLoaded();
    KOReaderMetadata meta;
    const auto lastSlash = epubPath.rfind('/');
    meta.filename = (lastSlash != std::string::npos) ? epubPath.substr(lastSlash + 1) : epubPath;
    if (epub) {
      meta.title = epub->getTitle();
      meta.authors = epub->getAuthor();
    } else {
      LOG_ERR("KOSync", "Epub unavailable for metadata; sending filename only");
    }
    progress.metadata = std::move(meta);
  }

  // Release the Epub before the network call so the TLS handshake has enough free heap
  // (consistent with the release-before-sync pattern in performSync); nothing below needs it.
  epub.reset();

  const auto result = KOReaderSyncClient::updateProgress(progress);

  // Drop the radio while user reads the result; full teardown happens at silent reboot.
  esp_wifi_stop();

  if (result != KOReaderSyncClient::OK) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = KOReaderSyncClient::errorString(result);
    }
    requestUpdate();
    if (shouldFinishAtHome()) {
      delay(1500);
      finishToDestination();
    }
    return;
  }

  // Stamp per-book baseline after any successful upload (auto or manual).
  {
    float stampPercent = autoUploadBookPercent;
    if (stampPercent < 0.0f && localProgress.percentage >= 0.0f) {
      stampPercent = localProgress.percentage * 100.0f;
    }
    KOREADER_STORE.markAutoUploadSucceeded(epubPath.c_str(), stampPercent);
  }

  {
    RenderLock lock(*this);
    state = UPLOAD_COMPLETE;
  }
  if (smartSyncEnabled() || shouldFinishAtHome()) {
    markAutoReturn();
  }
  requestUpdate(true);

  if (shouldFinishAtHome()) {
    delay(800);
    finishToDestination();
  }
}

void KOReaderSyncActivity::startQuietWifiConnect() {
  WIFI_STORE.loadFromFile();
  quietWifiPending = true;
  quietWifiAttempts = 0;
  quietWifiBeginIssued = false;
  quietWifiStartMs = 0;

  // Prefer last-connected SSID as first try.
  quietWifiCredIndex = 0;
  const auto& creds = WIFI_STORE.getCredentials();
  const std::string& last = WIFI_STORE.getLastConnectedSsid();
  if (!last.empty()) {
    for (size_t i = 0; i < creds.size(); ++i) {
      if (creds[i].ssid == last) {
        quietWifiCredIndex = i;
        break;
      }
    }
  }

  const char* firstSsid = creds.empty() ? nullptr : creds[quietWifiCredIndex % creds.size()].ssid.c_str();
  showBusyStatus(CONNECTING, tr(STR_CONNECTING_SAVED_WIFI), firstSsid);

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);
  delay(50);
}

void KOReaderSyncActivity::tickQuietWifiConnect() {
  if (!quietWifiPending) {
    return;
  }
  if (WiFi.status() == WL_CONNECTED) {
    quietWifiPending = false;
    WIFI_STORE.setLastConnectedSsid(WiFi.SSID().c_str());
    WIFI_STORE.saveToFile();
    LOG_DBG("KOSync", "Quiet WiFi connected: %s", WiFi.SSID().c_str());
    onWifiSelectionComplete(true);
    return;
  }

  const auto& creds = WIFI_STORE.getCredentials();
  if (creds.empty()) {
    quietWifiPending = false;
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = tr(STR_WIFI_CONN_FAILED);
    }
    requestUpdate(true);
    delay(1200);
    finishToDestination();
    return;
  }

  if (!quietWifiBeginIssued) {
    if (quietWifiAttempts >= creds.size()) {
      quietWifiPending = false;
      {
        RenderLock lock(*this);
        state = SYNC_FAILED;
        statusMessage = tr(STR_WIFI_CONN_FAILED);
      }
      requestUpdate(true);
      delay(1200);
      finishToDestination();
      return;
    }
    const auto& cred = creds[quietWifiCredIndex % creds.size()];
    LOG_DBG("KOSync", "Quiet WiFi try [%u/%u]: %s", static_cast<unsigned>(quietWifiAttempts + 1),
            static_cast<unsigned>(creds.size()), cred.ssid.c_str());
    // Show network name so multi-SSID try/timeout does not look stuck.
    char detail[48];
    snprintf(detail, sizeof(detail), "%s (%u/%u)", cred.ssid.c_str(),
             static_cast<unsigned>(quietWifiAttempts + 1), static_cast<unsigned>(creds.size()));
    showBusyStatus(CONNECTING, tr(STR_CONNECTING_SAVED_WIFI), detail);
    if (!cred.password.empty()) {
      WiFi.begin(cred.ssid.c_str(), cred.password.c_str());
    } else {
      WiFi.begin(cred.ssid.c_str());
    }
    quietWifiBeginIssued = true;
    quietWifiStartMs = millis();
    return;
  }

  if (millis() - quietWifiStartMs < QUIET_WIFI_TIMEOUT_MS) {
    return;  // still waiting
  }

  // Timeout — try next credential (circular from last-connected start).
  WiFi.disconnect(true, false);
  delay(30);
  quietWifiBeginIssued = false;
  quietWifiAttempts++;
  quietWifiCredIndex = (quietWifiCredIndex + 1) % creds.size();
}

void KOReaderSyncActivity::drawFullPageStatus(const char* message, const char* detail) const {
  renderer.clearScreen();
  auto metrics = UITheme::getInstance().getMetrics();
  Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 tr(STR_KOREADER_SYNC));
  const int top = screen.y + screen.height / 2 - 40;
  if (message && message[0]) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, message, true, EpdFontFamily::BOLD);
  }
  if (detail && detail[0]) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top + 36, detail, true, EpdFontFamily::REGULAR);
  }
  // Busy phases: Back cancels. Terminal success/fail still offer Done via loop.
  const bool busy = (state == CONNECTING || state == SYNCING || state == UPLOADING);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), busy ? "" : tr(STR_DONE), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

void KOReaderSyncActivity::onEnter() {
  Activity::onEnter();
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

  // Sync Behavior → Disabled: never start WiFi or network work.
  if (KOREADER_STORE.getSyncBehavior() == KOReaderSyncBehavior::OFF) {
    LOG_DBG("KOSync", "Sync behavior Disabled; exiting without network");
    finishToDestination();
    return;
  }

  // Check for credentials first
  if (!KOREADER_STORE.hasCredentials()) {
    state = NO_CREDENTIALS;
    requestUpdate();
    return;
  }

  // Past this point every path uses WiFi.
  wifiActivated = true;

  // Check if already connected (e.g. from settings page auth)
  if (WiFi.status() == WL_CONNECTED) {
    LOG_DBG("KOSync", "Already connected to WiFi");
    onWifiSelectionComplete(true);
    return;
  }

  // Leave / auto-upload: quiet saved-network connect (Connecting… only).
  // Manual Sync Progress keeps the full WiFi picker for first-time / failures.
  if (useQuietWifi()) {
    LOG_DBG("KOSync", "Starting quiet WiFi connect for leave sync");
    startQuietWifiConnect();
    return;
  }

  LOG_DBG("KOSync", "Launching WifiSelectionActivity...");
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void KOReaderSyncActivity::onExit() {
  Activity::onExit();

  if (exitHandled) {
    return;
  }

  if (wifiActivated) {
    WiFi.disconnect(false);
    delay(30);
    if (shouldFinishAtHome()) {
      silentRestart();
    } else {
      silentRestartToReader();
    }
  }
}

void KOReaderSyncActivity::render(RenderLock&&) {
  // Quiet leave-sync: full-page status (no framebuffer snapshot — saves TLS heap).
  if (useQuietLeaveUi()) {
    if (state == NO_CREDENTIALS) {
      drawFullPageStatus(tr(STR_NO_CREDENTIALS_MSG), tr(STR_KOREADER_SETUP_HINT));
      return;
    }
    if (state == CONNECTING || state == SYNCING || state == UPLOADING) {
      // statusDetail: SSID, try count, or other phase context.
      drawFullPageStatus(statusMessage.c_str(), statusDetail.empty() ? nullptr : statusDetail.c_str());
      return;
    }
    if (state == UPLOAD_COMPLETE) {
      drawFullPageStatus(tr(STR_UPLOAD_SUCCESS), nullptr);
      return;
    }
    if (state == SYNC_COMPLETE) {
      drawFullPageStatus(tr(STR_ALREADY_SYNCED), nullptr);
      return;
    }
    if (state == SYNC_FAILED) {
      drawFullPageStatus(tr(STR_SYNC_FAILED_MSG),
                         !statusDetail.empty() ? statusDetail.c_str()
                         : !statusMessage.empty() ? statusMessage.c_str()
                                                  : nullptr);
      return;
    }
    if (state == NO_REMOTE_PROGRESS) {
      drawFullPageStatus(tr(STR_NO_REMOTE_MSG), tr(STR_UPLOAD_PROMPT));
      return;
    }
    // SHOWING_RESULT falls through to compare UI below.
  }

  renderer.clearScreen();

  auto metrics = UITheme::getInstance().getMetrics();
  Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);

  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 tr(STR_KOREADER_SYNC));

  int top = screen.y + screen.height / 2 - 40;
  if (state == NO_CREDENTIALS) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, tr(STR_NO_CREDENTIALS_MSG), true,
                              EpdFontFamily::BOLD);
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top + 40, tr(STR_KOREADER_SETUP_HINT), true,
                              EpdFontFamily::BOLD);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == CONNECTING || state == SYNCING || state == UPLOADING) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, statusMessage.c_str(), true, EpdFontFamily::BOLD);
    if (!statusDetail.empty()) {
      UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top + 36, statusDetail.c_str(), true,
                                EpdFontFamily::REGULAR);
    }
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == SHOWING_RESULT) {
    int contentLeft = screen.x + metrics.contentSidePadding;
    int contentWidth = screen.width - metrics.contentSidePadding * 2;
    top = screen.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

    renderer.drawText(UI_10_FONT_ID, contentLeft, top, tr(STR_PROGRESS_FOUND), true, EpdFontFamily::BOLD);

    // Remote chapter name requires Epub (loaded lazily in performSync before this state).
    const int remoteTocIndex = epub->getTocIndexForSpineIndex(remotePosition.spineIndex);
    const std::string remoteChapter =
        (remoteTocIndex >= 0) ? epub->getTocItem(remoteTocIndex).title
                              : (std::string(tr(STR_SECTION_PREFIX)) + std::to_string(remotePosition.spineIndex + 1));
    // Local chapter name was pre-computed before Epub was released.
    const std::string localChapter =
        !localChapterName.empty() ? localChapterName
                                  : (std::string(tr(STR_SECTION_PREFIX)) + std::to_string(currentSpineIndex + 1));

    // Remote progress - chapter and page
    renderer.drawText(UI_10_FONT_ID, contentLeft, top + 36, tr(STR_REMOTE_LABEL), true);
    char remoteChapterStr[128];
    snprintf(remoteChapterStr, sizeof(remoteChapterStr), "  %s", remoteChapter.c_str());
    renderer.drawText(UI_10_FONT_ID, contentLeft, top + 58, remoteChapterStr);
    char remotePageStr[64];
    snprintf(remotePageStr, sizeof(remotePageStr), tr(STR_PAGE_OVERALL_FORMAT), remotePosition.pageNumber + 1,
             remoteProgress.percentage * 100);
    renderer.drawText(UI_10_FONT_ID, contentLeft, top + 80, remotePageStr);

    int yLocal = top + 110;
    if (!remoteProgress.device.empty()) {
      char deviceStr[64];
      snprintf(deviceStr, sizeof(deviceStr), tr(STR_DEVICE_FROM_FORMAT), remoteProgress.device.c_str());
      renderer.drawText(UI_10_FONT_ID, contentLeft, top + 102, deviceStr);
      yLocal = top + 128;
    }

    // Local progress - chapter and page
    renderer.drawText(UI_10_FONT_ID, contentLeft, yLocal, tr(STR_LOCAL_LABEL), true);
    char localChapterStr[128];
    snprintf(localChapterStr, sizeof(localChapterStr), "  %s", localChapter.c_str());
    renderer.drawText(UI_10_FONT_ID, contentLeft, yLocal + 22, localChapterStr);
    char localPageStr[64];
    snprintf(localPageStr, sizeof(localPageStr), tr(STR_PAGE_TOTAL_OVERALL_FORMAT), currentPage + 1, totalPagesInSpine,
             localProgress.percentage * 100);
    renderer.drawText(UI_10_FONT_ID, contentLeft, yLocal + 44, localPageStr);

    const int optionY = yLocal + 74;
    const int optionHeight = 28;

    // Apply option
    if (selectedOption == 0) {
      renderer.fillRect(contentLeft - 4, optionY - 2, contentWidth + 8, optionHeight);
    }
    renderer.drawText(UI_10_FONT_ID, contentLeft, optionY, tr(STR_APPLY_REMOTE), selectedOption != 0);

    // Upload option
    if (selectedOption == 1) {
      renderer.fillRect(contentLeft - 4, optionY + optionHeight - 2, contentWidth + 8, optionHeight);
    }
    renderer.drawText(UI_10_FONT_ID, contentLeft, optionY + optionHeight, tr(STR_UPLOAD_LOCAL), selectedOption != 1);

    // Bottom button hints
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == NO_REMOTE_PROGRESS) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, tr(STR_NO_REMOTE_MSG), true, EpdFontFamily::BOLD);
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top + 40, tr(STR_UPLOAD_PROMPT));

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_UPLOAD), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == UPLOAD_COMPLETE || state == SYNC_COMPLETE) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top,
                              state == UPLOAD_COMPLETE ? tr(STR_UPLOAD_SUCCESS) : tr(STR_ALREADY_SYNCED), true,
                              EpdFontFamily::BOLD);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_DONE), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == SYNC_FAILED) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, tr(STR_SYNC_FAILED_MSG), true, EpdFontFamily::BOLD);
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top + 40, statusMessage.c_str());

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }
}

void KOReaderSyncActivity::loop() {
  // Cancel quiet Wi‑Fi / in-progress leave sync — never trap the user with no exit.
  // Only Back cancels: the busy UI labels Confirm empty, and treating Confirm as
  // cancel races with Ask-every-time (ConfirmationActivity selects on Confirm
  // *press*; the subsequent *release* arrives while quiet Wi‑Fi is connecting
  // and used to abort before NTP/hash/fetch ever ran).
  if (quietWifiPending || state == CONNECTING || state == SYNCING || state == UPLOADING) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      LOG_DBG("KOSync", "User cancelled sync (state=%d quietWifi=%d)", static_cast<int>(state),
              quietWifiPending ? 1 : 0);
      quietWifiPending = false;
      WiFi.disconnect(false);
      finishToDestination();
      return;
    }
    if (quietWifiPending) {
      tickQuietWifiConnect();
    }
    return;
  }

  if (state == NO_CREDENTIALS || state == SYNC_FAILED || state == UPLOAD_COMPLETE || state == SYNC_COMPLETE) {
    // Auto-dismiss after delay when set (success paths).
    if (autoReturnAt != 0 && millis() >= autoReturnAt) {
      finishToDestination();
      return;
    }
    // Always allow Back / Confirm / tap to leave — especially leave-to-home failures
    // that previously ignored input and left a dead screen.
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      finishToDestination();
      return;
    }
    int tx = 0;
    int ty = 0;
    if (mappedInput.wasScreenTapped(tx, ty)) {
      finishToDestination();
    }
    return;
  }

  if (state == SHOWING_RESULT) {
    auto chooseSelected = [this] {
      if (selectedOption == 0) {
        saveProgressAndReturn(remotePosition.spineIndex, remotePosition.pageNumber);
      } else if (selectedOption == 1) {
        performUpload();
      }
    };

    {
      const auto& metrics = UITheme::getInstance().getMetrics();
      const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
      const int top = screen.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
      constexpr int optionHeight = 30;
      int touchedOption = -1;
      const auto touch = mappedInput.rowTouch(touchedOption, top + 230 - 2, optionHeight, 2);
      if (touch == MappedInputManager::RowTouch::Down) {
        if (selectedOption != touchedOption) {
          selectedOption = touchedOption;
          requestUpdate();
        }
        return;
      }
      if (touch == MappedInputManager::RowTouch::Tap) {
        selectedOption = touchedOption;
        chooseSelected();
        return;
      }
    }

    // Navigate options
    if (mappedInput.wasReleased(MappedInputManager::Button::Up) ||
        mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      selectedOption = (selectedOption + 1) % 2;  // Wrap around among 2 options
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Down) ||
               mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      selectedOption = (selectedOption + 1) % 2;  // Wrap around among 2 options
      requestUpdate();
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      chooseSelected();
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      finishToDestination();
    }
    return;
  }

  if (state == NO_REMOTE_PROGRESS) {
    int tx = 0;
    int ty = 0;
    if (mappedInput.wasScreenTapped(tx, ty) && ty > renderer.getScreenHeight() / 3 &&
        ty < renderer.getScreenHeight() * 2 / 3) {
      if (documentHash.empty()) {
        if (KOREADER_STORE.getMatchMethod() == DocumentMatchMethod::FILENAME) {
          documentHash = KOReaderDocumentId::calculateFromFilename(epubPath);
        } else {
          documentHash = KOReaderDocumentId::calculate(epubPath);
        }
      }
      performUpload();
      return;
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      // Calculate hash if not done yet
      if (documentHash.empty()) {
        if (KOREADER_STORE.getMatchMethod() == DocumentMatchMethod::FILENAME) {
          documentHash = KOReaderDocumentId::calculateFromFilename(epubPath);
        } else {
          documentHash = KOReaderDocumentId::calculate(epubPath);
        }
      }
      performUpload();
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      finishToDestination();
    }
    return;
  }
}
