#include "KOReaderSyncActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <esp_wifi.h>

#include <algorithm>
#include <cassert>
#include <cmath>

#include "CrossPointSettings.h"
#include "Epub/Section.h"
#include "EpubReaderUtils.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderDocumentId.h"
#include "MappedInputManager.h"
#include "ReaderUtils.h"
#include "SdCardFontSystem.h"
#include "SilentRestart.h"
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

void wifiOff() {
  if (esp_sntp_enabled()) {
    esp_sntp_stop();
  }
  WiFi.disconnect(false);
  delay(100);
  WiFi.mode(WIFI_OFF);
  delay(100);
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

void KOReaderSyncActivity::saveProgressAndReturn(const CrossPointPosition& position) {
  // epub is guaranteed non-null here: ensureEpubLoaded() was called in performSync() before
  // SHOWING_RESULT state is entered, and this method is only called from that state.
  assert(epub);
  const int pageCount = std::max(position.totalPages, position.pageNumber + 1);
  if (pageCount != position.totalPages) {
    LOG_DBG("KOSync", "Adjusted remote page count before save: page=%d count=%d -> %d", position.pageNumber,
            position.totalPages, pageCount);
  }
  if (!EpubReaderUtils::saveProgress(*epub, position.spineIndex, position.pageNumber, pageCount)) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = tr(STR_SAVE_PROGRESS_FAILED);
    }
    requestUpdate(true);
    return;
  }
  returnToReader();
}

void KOReaderSyncActivity::returnToReader() { activityManager.goToReader(epubPath); }

bool KOReaderSyncActivity::consumeInitialConfirmRelease() {
  if (!lockInitialConfirmRelease) {
    return false;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
      !mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
    lockInitialConfirmRelease = false;
  }
  return true;
}

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
}

void KOReaderSyncActivity::finishAutoMode() {
  exitHandled = true;
  wifiOff();
  wifiActivated = false;
  LOG_DBG("KOSync", "Auto-upload finished, restarting to home");
  silentRestart();
}

void KOReaderSyncActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    LOG_DBG("KOSync", "WiFi connection failed, exiting");
    if (autoUploadOnly) {
      finishAutoMode();
      return;
    }
    returnToReader();
    return;
  }

  LOG_DBG("KOSync", "WiFi connected, starting sync");
  sdFontSystem.releaseForNetwork(renderer);

  {
    RenderLock lock(*this);
    state = SYNCING;
    statusMessage = tr(STR_SYNCING_TIME);
  }
  requestUpdate(true);

  // Sync time with NTP before making API requests
  syncTimeWithNTP();

  {
    RenderLock lock(*this);
    statusMessage = tr(STR_CALC_HASH);
  }
  requestUpdate(true);

  if (autoUploadOnly) {
    performAutoUpload();
  } else {
    performSync();
  }
}

void KOReaderSyncActivity::performAutoUpload() {
  if (KOREADER_STORE.getMatchMethod() == DocumentMatchMethod::FILENAME) {
    documentHash = KOReaderDocumentId::calculateFromFilename(epubPath);
  } else {
    documentHash = KOReaderDocumentId::calculate(epubPath);
  }
  if (documentHash.empty()) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = tr(STR_HASH_FAILED);
    }
    requestUpdate(true);
    delay(1500);
    finishAutoMode();
    return;
  }

  LOG_DBG("KOSync", "Auto-upload document hash: %s", documentHash.c_str());
  performUpload();
}

void KOReaderSyncActivity::performSync() {
  const DocumentMatchMethod primaryMethod = KOREADER_STORE.getMatchMethod();
  documentHash = calculateDocumentHashForMethod(epubPath, primaryMethod);
  if (documentHash.empty()) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = tr(STR_HASH_FAILED);
    }
    requestUpdate(true);
    return;
  }
  const std::string primaryHash = documentHash;

  LOG_DBG("KOSync", "Document hash (%s): %s", matchMethodName(primaryMethod), documentHash.c_str());

  {
    RenderLock lock(*this);
    statusMessage = tr(STR_FETCH_PROGRESS);
  }
  if (requestUpdateAndWait() != RequestUpdateResult::Rendered) {
    LOG_ERR("KOSync", "Fetch progress screen could not be rendered synchronously; aborting sync");
    wifiOff();
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = tr(STR_SYNC_FAILED_MSG);
    }
    requestUpdate(true);
    return;
  }

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
      statusMessage = "";
    }
    requestUpdate(true);
    return;
  }

  KOReaderPosition koPos = {remoteProgress.progress, remoteProgress.percentage};
  remotePosition = ProgressMapper::toCrossPoint(epub, koPos, currentSpineIndex, totalPagesInSpine);

  // Refine page using section cache LUTs: li index, anchor, or paragraph index.
  if (remotePosition.hasLiIndex || remotePosition.xpathAnchorId[0] != '\0' || remotePosition.hasParagraphIndex) {
    Section tempSection(epub, remotePosition.spineIndex, renderer);
    bool refined = false;
    if (remotePosition.hasLiIndex) {
      const auto liPage = tempSection.getPageForListItemIndex(remotePosition.liIndex);
      if (liPage.has_value()) {
        LOG_DBG("KOSync", "Li index %u -> page %d (was %d)", remotePosition.liIndex, *liPage,
                remotePosition.pageNumber);
        remotePosition.pageNumber = *liPage;
        refined = true;
      } else {
        LOG_DBG("KOSync", "Li index %u not found in section LUT", remotePosition.liIndex);
      }
    }
    if (!refined && remotePosition.xpathAnchorId[0] != '\0') {
      const auto anchorPage = tempSection.getPageForAnchor(std::string(remotePosition.xpathAnchorId));
      if (anchorPage.has_value()) {
        LOG_DBG("KOSync", "Anchor '%s' -> page %d (was %d)", remotePosition.xpathAnchorId, *anchorPage,
                remotePosition.pageNumber);
        remotePosition.pageNumber = *anchorPage;
        refined = true;
      } else {
        LOG_DBG("KOSync", "Anchor '%s' not found in section cache", remotePosition.xpathAnchorId);
      }
    }
    if (!refined && remotePosition.hasParagraphIndex) {
      const auto paragraphPage = tempSection.getPageForParagraphIndex(remotePosition.paragraphIndex);
      const auto nextParagraphPage = tempSection.getPageForParagraphIndex(remotePosition.paragraphIndex + 1);
      if (paragraphPage.has_value()) {
        int refinedPage = std::max(remotePosition.pageNumber, static_cast<int>(*paragraphPage));
        if (nextParagraphPage.has_value()) {
          const int lutSpan = static_cast<int>(*nextParagraphPage) - static_cast<int>(*paragraphPage);
          // Keep the percentage-derived page inside the paragraph's cached page range.
          // A one-page paragraph should not allow byte-percentage drift to jump to later paragraphs.
          if (lutSpan > 0 && refinedPage >= static_cast<int>(*nextParagraphPage)) {
            refinedPage = static_cast<int>(*nextParagraphPage) - 1;
          }
        }
        char nextParaBuf[8];
        if (nextParagraphPage.has_value())
          snprintf(nextParaBuf, sizeof(nextParaBuf), "%d", *nextParagraphPage);
        else
          snprintf(nextParaBuf, sizeof(nextParaBuf), "none");
        LOG_DBG("KOSync", "Paragraph %u -> LUT page %d, nextPara page %s, intra page %d, using %d",
                remotePosition.paragraphIndex, *paragraphPage, nextParaBuf, remotePosition.pageNumber, refinedPage);
        remotePosition.pageNumber = refinedPage;
      } else {
        LOG_DBG("KOSync", "Paragraph %u not found in section LUT", remotePosition.paragraphIndex);
      }
    }
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

    saveProgressAndReturn(remotePosition);
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
  {
    RenderLock lock(*this);
    state = UPLOADING;
    statusMessage = tr(STR_UPLOAD_PROGRESS);
  }
  if (requestUpdateAndWait() != RequestUpdateResult::Rendered) {
    LOG_ERR("KOSync", "Upload progress screen could not be rendered synchronously; aborting upload");
    wifiOff();
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = tr(STR_SYNC_FAILED_MSG);
    }
    requestUpdate(true);
    if (autoUploadOnly) {
      delay(1000);
      finishAutoMode();
    }
    return;
  }

  if (epub) {
    epub.reset();
    LOG_DBG("KOSync", "Released epub before upload (heap: %u)", (unsigned)ESP.getFreeHeap());
  }

  // localProgress was pre-computed in EpubReaderActivity before the Epub was released.
  KOReaderProgress progress;
  progress.document = documentHash;
  progress.progress = localProgress.xpath;
  progress.percentage = localProgress.percentage;
  progress.device = SETTINGS.getEffectiveDeviceName();

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
  wifiOff();

  if (result != KOReaderSyncClient::OK) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = KOReaderSyncClient::errorString(result);
    }
    requestUpdate();
    if (autoUploadOnly) {
      delay(1500);
      finishAutoMode();
    }
    return;
  }

  // Stamp per-book baseline after any successful upload so auto-upload on close
  // does not immediately re-upload after a manual Sync Progress upload.
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
  if (smartSyncEnabled()) {
    markAutoReturn();
  }
  requestUpdate(true);

  if (autoUploadOnly) {
    delay(800);
    finishAutoMode();
  }
}

void KOReaderSyncActivity::onEnter() {
  Activity::onEnter();
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
  lockInitialConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);

  // Check for credentials first
  if (!KOREADER_STORE.hasCredentials()) {
    state = NO_CREDENTIALS;
    requestUpdate();
    return;
  }

  // Past this point every path uses WiFi.
  sdFontSystem.releaseLoadedFont(renderer);
  wifiActivated = true;

  // Check if already connected (e.g. from settings page auth)
  if (WiFi.status() == WL_CONNECTED) {
    LOG_DBG("KOSync", "Already connected to WiFi");
    onWifiSelectionComplete(true);
    return;
  }

  // Launch WiFi selection subactivity
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
    wifiOff();
    if (autoUploadOnly) {
      silentRestart();
    } else {
      silentRestartToReader();
    }
  }
}

void KOReaderSyncActivity::render(RenderLock&&) {
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
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
    renderer.displayBuffer();
    return;
  }

  if (state == SYNCING || state == UPLOADING) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, statusMessage.c_str(), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  if (state == SHOWING_RESULT) {
    // Show comparison
    top = screen.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_PROGRESS_FOUND), true, EpdFontFamily::BOLD);

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
    renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, top + 40, tr(STR_REMOTE_LABEL), true);
    char remoteChapterStr[128];
    snprintf(remoteChapterStr, sizeof(remoteChapterStr), "  %s", remoteChapter.c_str());
    renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, top + 65, remoteChapterStr);
    char remotePageStr[64];
    snprintf(remotePageStr, sizeof(remotePageStr), tr(STR_PAGE_OVERALL_FORMAT), remotePosition.pageNumber + 1,
             remoteProgress.percentage * 100);
    renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, top + 90, remotePageStr);

    if (!remoteProgress.device.empty()) {
      char deviceStr[64];
      snprintf(deviceStr, sizeof(deviceStr), tr(STR_DEVICE_FROM_FORMAT), remoteProgress.device.c_str());
      renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, top + 115, deviceStr);
    }

    // Local progress - chapter and page
    renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, top + 150, tr(STR_LOCAL_LABEL), true);
    char localChapterStr[128];
    snprintf(localChapterStr, sizeof(localChapterStr), "  %s", localChapter.c_str());
    renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, top + 175, localChapterStr);
    char localPageStr[64];
    snprintf(localPageStr, sizeof(localPageStr), tr(STR_PAGE_TOTAL_OVERALL_FORMAT), currentPage + 1, totalPagesInSpine,
             localProgress.percentage * 100);
    renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, top + 200, localPageStr);

    const int optionY = top + 230;
    const int optionHeight = 30;

    // Apply option
    if (selectedOption == 0) {
      renderer.fillRect(screen.x, optionY - 2, screen.width - 1, optionHeight);
    }
    renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, optionY, tr(STR_APPLY_REMOTE),
                      selectedOption != 0);

    // Upload option
    if (selectedOption == 1) {
      renderer.fillRect(screen.x, optionY + optionHeight - 2, screen.width - 1, optionHeight);
    }
    renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, optionY + optionHeight,
                      tr(STR_UPLOAD_LOCAL), selectedOption != 1);

    // Bottom button hints
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
    renderer.displayBuffer();
    return;
  }

  if (state == NO_REMOTE_PROGRESS) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, tr(STR_NO_REMOTE_MSG), true, EpdFontFamily::BOLD);
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top + 40, tr(STR_UPLOAD_PROMPT));

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_UPLOAD), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
    renderer.displayBuffer();
    return;
  }

  if (state == UPLOAD_COMPLETE || state == SYNC_COMPLETE) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top,
                              state == UPLOAD_COMPLETE ? tr(STR_UPLOAD_SUCCESS) : tr(STR_ALREADY_SYNCED), true,
                              EpdFontFamily::BOLD);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_DONE), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
    renderer.displayBuffer();
    return;
  }

  if (state == SYNC_FAILED) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, tr(STR_SYNC_FAILED_MSG), true, EpdFontFamily::BOLD);
    const int messageWidth = screen.width - metrics.contentSidePadding * 2;
    const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
    const auto messageLines = renderer.wrappedText(UI_10_FONT_ID, statusMessage.c_str(), messageWidth, 3);
    int messageY = top + 40;
    for (const auto& line : messageLines) {
      UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, messageY, line.c_str());
      messageY += lineHeight + 4;
    }

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
    renderer.displayBuffer();
    return;
  }
}

void KOReaderSyncActivity::loop() {
  if (consumeInitialConfirmRelease()) {
    return;
  }

  if (state == NO_CREDENTIALS || state == SYNC_FAILED || state == UPLOAD_COMPLETE || state == SYNC_COMPLETE) {
    if (autoReturnAt != 0 && millis() >= autoReturnAt) {
      if (autoUploadOnly) {
        finishAutoMode();
      } else {
        returnToReader();
      }
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (autoUploadOnly) {
        finishAutoMode();
      } else {
        returnToReader();
      }
    }
    return;
  }

  if (state == SHOWING_RESULT) {
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
      if (selectedOption == 0) {
        saveProgressAndReturn(remotePosition);
      } else if (selectedOption == 1) {
        // Upload local progress
        performUpload();
      }
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      returnToReader();
    }
    return;
  }

  if (state == NO_REMOTE_PROGRESS) {
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
      returnToReader();
    }
    return;
  }
}
