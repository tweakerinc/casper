#include "KOReaderAuthActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include "KOReaderCredentialStore.h"
#include "KOReaderSyncClient.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "WifiCredentialStore.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/UiGhostPolicy.h"

void KOReaderAuthActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    {
      RenderLock lock(*this);
      state = FAILED;
      errorMessage = tr(STR_WIFI_CONN_FAILED);
    }
    requestUpdate();
    return;
  }

  // Drop any residual scan tables before TLS (WifiSelection may leave none;
  // quiet path never scanned — cheap no-op).
  WiFi.scanDelete();

  {
    RenderLock lock(*this);
    state = AUTHENTICATING;
    statusMessage = mode == Mode::SIGN_UP ? tr(STR_CREATING_ACCOUNT) : tr(STR_AUTHENTICATING);
  }
  requestUpdate();

  LOG_DBG("KOSync", "Auth pre-TLS heap=%u maxAlloc=%u", static_cast<unsigned>(ESP.getFreeHeap()),
          static_cast<unsigned>(ESP.getMaxAllocHeap()));
  performAuthentication();
}

void KOReaderAuthActivity::performAuthentication() {
  const auto result = mode == Mode::SIGN_UP ? KOReaderSyncClient::createUser() : KOReaderSyncClient::authenticate();

  {
    RenderLock lock(*this);
    if (result == KOReaderSyncClient::OK) {
      state = SUCCESS;
      statusMessage = mode == Mode::SIGN_UP ? tr(STR_ACCOUNT_CREATED) : tr(STR_AUTH_SUCCESS);
    } else {
      state = FAILED;
      errorMessage =
          result == KOReaderSyncClient::USER_EXISTS ? tr(STR_USERNAME_TAKEN) : KOReaderSyncClient::errorString(result);
    }
  }
  requestUpdate();
}

void KOReaderAuthActivity::startQuietWifiConnect() {
  WIFI_STORE.loadFromFile();
  quietWifiPending = true;
  quietWifiAttempts = 0;
  quietWifiBeginIssued = false;
  quietWifiStartMs = 0;
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

  {
    RenderLock lock(*this);
    state = CONNECTING;
    statusMessage = tr(STR_CONNECTING_SAVED_WIFI);
  }
  requestUpdate();

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);
  delay(50);
  LOG_DBG("KOSync", "Auth quiet WiFi start heap=%u maxAlloc=%u creds=%u", static_cast<unsigned>(ESP.getFreeHeap()),
          static_cast<unsigned>(ESP.getMaxAllocHeap()), static_cast<unsigned>(creds.size()));
}

void KOReaderAuthActivity::tickQuietWifiConnect() {
  if (!quietWifiPending) {
    return;
  }
  if (WiFi.status() == WL_CONNECTED) {
    quietWifiPending = false;
    WIFI_STORE.setLastConnectedSsid(WiFi.SSID().c_str());
    WIFI_STORE.saveToFile();
    LOG_DBG("KOSync", "Auth quiet WiFi connected: %s heap=%u maxAlloc=%u", WiFi.SSID().c_str(),
            static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
    onWifiSelectionComplete(true);
    return;
  }

  const auto& creds = WIFI_STORE.getCredentials();
  if (creds.empty()) {
    quietWifiPending = false;
    onWifiSelectionComplete(false);
    return;
  }

  if (!quietWifiBeginIssued) {
    if (quietWifiAttempts >= creds.size()) {
      quietWifiPending = false;
      onWifiSelectionComplete(false);
      return;
    }
    const auto& cred = creds[quietWifiCredIndex % creds.size()];
    LOG_DBG("KOSync", "Auth quiet WiFi try [%u/%u]: %s", static_cast<unsigned>(quietWifiAttempts + 1),
            static_cast<unsigned>(creds.size()), cred.ssid.c_str());
    {
      RenderLock lock(*this);
      state = CONNECTING;
      statusMessage = tr(STR_CONNECTING_SAVED_WIFI);
    }
    requestUpdate();
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
    return;
  }

  WiFi.disconnect(true, false);
  delay(30);
  quietWifiBeginIssued = false;
  quietWifiAttempts++;
  quietWifiCredIndex = (quietWifiCredIndex + 1) % creds.size();
}

void KOReaderAuthActivity::onEnter() {
  Activity::onEnter();

  if (WiFi.status() == WL_CONNECTED) {
    onWifiSelectionComplete(true);
    return;
  }

  // Prefer quiet saved-network connect: full scan UI + settings stack left
  // ~45 KB free and blocked TLS. Quiet path keeps STA without scan tables.
  WIFI_STORE.loadFromFile();
  if (!WIFI_STORE.getCredentials().empty()) {
    startQuietWifiConnect();
    return;
  }

  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void KOReaderAuthActivity::onExit() {
  Activity::onExit();
  quietWifiPending = false;

  // silentRestart() itself tears WiFi fully; still disconnect so we always
  // take the defrag reboot after a radio session (heap fragmentation).
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void KOReaderAuthActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 mode == Mode::SIGN_UP ? tr(STR_SIGN_UP) : tr(STR_KOREADER_AUTH));
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = (pageHeight - height) / 2;

  if (state == CONNECTING || state == AUTHENTICATING) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, statusMessage.c_str());
  } else if (state == SUCCESS) {
    renderer.drawCenteredText(UI_10_FONT_ID, top,
                              mode == Mode::SIGN_UP ? tr(STR_ACCOUNT_CREATED) : tr(STR_AUTH_SUCCESS), true,
                              EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, top + height + 10, tr(STR_SYNC_READY));
  } else if (state == FAILED) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, mode == Mode::SIGN_UP ? tr(STR_SIGNUP_FAILED) : tr(STR_AUTH_FAILED),
                              true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, top + height + 10, errorMessage.c_str());
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  UiGhostPolicy::displayMenuFrame(renderer);
}

void KOReaderAuthActivity::loop() {
  if (quietWifiPending) {
    tickQuietWifiConnect();
    return;
  }

  if (state == SUCCESS || state == FAILED) {
    int x = 0;
    int y = 0;
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(x, y)) {
      finish();
    }
  }
}
