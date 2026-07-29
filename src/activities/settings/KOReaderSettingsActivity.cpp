#include "KOReaderSettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "KOReaderAuthActivity.h"
#include "KOReaderCredentialStore.h"
#include "MappedInputManager.h"
#include "activities/ActivityResult.h"
#include "activities/util/IntervalSelectionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
// Layout:
//  0 Username
//  1 Password
//  2 Sign Up
//  3 Authenticate
//  4 Server URL
//  5 Document Matching
//  6 Sync Behavior  (popup: Off / Smart / Ask / Percent / Time — exclusive)
//  [if Percent] Percent threshold
//  [if Time]    Time interval
//  Upload Metadata

constexpr int kIdxUsername = 0;
constexpr int kIdxPassword = 1;
constexpr int kIdxSignUp = 2;
constexpr int kIdxAuth = 3;
constexpr int kIdxServer = 4;
constexpr int kIdxMatch = 5;
constexpr int kIdxSyncBehavior = 6;
constexpr int kFixedThroughSync = 7;  // indices 0..6 always present

// Display order (not storage enum order): Off, Smart Sync, Ask Every Time, Percent, Time.
constexpr int SYNC_BEHAVIOR_OPTION_COUNT = static_cast<int>(KOReaderSyncBehavior::COUNT);
const StrId syncBehaviorOptionNames[SYNC_BEHAVIOR_OPTION_COUNT] = {
    StrId::STR_OFF,
    StrId::STR_SMART_SYNC,
    StrId::STR_ASK_EVERY_TIME,
    StrId::STR_PERCENT,
    StrId::STR_TIME,
};

KOReaderSyncBehavior currentBehavior() { return KOREADER_STORE.getSyncBehavior(); }

bool showPercentDetail() { return currentBehavior() == KOReaderSyncBehavior::PERCENT; }
bool showTimeDetail() { return currentBehavior() == KOReaderSyncBehavior::TIME; }
bool showUploadMetadata() { return currentBehavior() != KOReaderSyncBehavior::OFF; }

int menuItemCount() {
  int n = kFixedThroughSync;
  if (showPercentDetail() || showTimeDetail()) n += 1;
  if (showUploadMetadata()) n += 1;  // Upload Metadata only when sync is enabled
  return n;
}

int gateDetailIndex() {
  if (!showPercentDetail() && !showTimeDetail()) return -1;
  return kFixedThroughSync;
}

int uploadMetaIndex() {
  if (!showUploadMetadata()) return -1;
  int idx = kFixedThroughSync;
  if (showPercentDetail() || showTimeDetail()) idx += 1;
  return idx;
}

void formatIntervalMinutes(const uint16_t minutes, char* buf, const size_t len) {
  if (minutes == KOReaderCredentialStore::ALWAYS_INTERVAL_MINUTES) {
    snprintf(buf, len, "%s", tr(STR_ALWAYS));
    return;
  }
  // Compact "1H 5M" / "0H 30M" so users don't convert minutes mentally.
  const unsigned h = minutes / 60u;
  const unsigned m = minutes % 60u;
  snprintf(buf, len, "%uH %uM", h, m);
}

const char* syncBehaviorLabel(const KOReaderSyncBehavior b) {
  const uint8_t i = syncBehaviorToDisplay(b);
  if (i >= SYNC_BEHAVIOR_OPTION_COUNT) return I18N.get(StrId::STR_OFF);
  return I18N.get(syncBehaviorOptionNames[i]);
}

StrId menuNameAt(const int index) {
  if (index == kIdxUsername) return StrId::STR_USERNAME;
  if (index == kIdxPassword) return StrId::STR_PASSWORD;
  if (index == kIdxSignUp) return StrId::STR_SIGN_UP;
  if (index == kIdxAuth) return StrId::STR_AUTHENTICATE;
  if (index == kIdxServer) return StrId::STR_SYNC_SERVER_URL;
  if (index == kIdxMatch) return StrId::STR_DOCUMENT_MATCHING;
  if (index == kIdxSyncBehavior) return StrId::STR_SYNC_BEHAVIOR;
  if (index == gateDetailIndex()) {
    return showPercentDetail() ? StrId::STR_PERCENT : StrId::STR_TIME;
  }
  const int metaIdx = uploadMetaIndex();
  if (metaIdx >= 0 && index == metaIdx) return StrId::STR_SEND_METADATA;
  return StrId::STR_USERNAME;
}

void clampSelectedIndex(size_t& selectedIndex) {
  const int count = menuItemCount();
  if (count <= 0) {
    selectedIndex = 0;
    return;
  }
  if (selectedIndex >= static_cast<size_t>(count)) {
    selectedIndex = static_cast<size_t>(count - 1);
  }
}
}  // namespace

void KOReaderSettingsActivity::onEnter() {
  Activity::onEnter();
  selectedIndex = 0;
  requestUpdate();
}

void KOReaderSettingsActivity::onExit() { Activity::onExit(); }

void KOReaderSettingsActivity::openTimeIntervalPicker() {
  startActivityForResult(
      std::make_unique<IntervalSelectionActivity>(
          renderer, mappedInput, "KoUploadTimeInterval", StrId::STR_UPLOAD_TIME_INTERVAL,
          static_cast<int>(KOREADER_STORE.getAutoUploadIntervalMinutes()),
          static_cast<int>(KOReaderCredentialStore::MIN_INTERVAL_MINUTES),
          static_cast<int>(KOReaderCredentialStore::MAX_INTERVAL_MINUTES),
          /*smallStep=*/5, /*largeStep=*/60, StrId::STR_UPLOAD_TIME_MINUTES_FORMAT,
          /*readerActivity=*/false, /*ignoreInitialConfirmRelease=*/true, StrId::STR_NONE_OPT, StrId::STR_ALWAYS,
          IntervalSelectionActivity::DisplayStyle::HoursMinutes),
      [this](const ActivityResult& result) {
        if (!result.isCancelled) {
          const auto* iv = std::get_if<IntervalResult>(&result.data);
          if (iv != nullptr) {
            KOREADER_STORE.setAutoUploadIntervalMinutes(static_cast<uint16_t>(iv->value));
            KOREADER_STORE.saveToFile();
          }
        }
        requestUpdate();
      });
}

void KOReaderSettingsActivity::openPercentThresholdPicker() {
  startActivityForResult(
      std::make_unique<IntervalSelectionActivity>(
          renderer, mappedInput, "KoUploadPercent", StrId::STR_UPLOAD_PERCENT_THRESHOLD,
          static_cast<int>(KOREADER_STORE.getAutoUploadPercentThreshold()),
          static_cast<int>(KOReaderCredentialStore::MIN_PERCENT_THRESHOLD),
          static_cast<int>(KOReaderCredentialStore::MAX_PERCENT_THRESHOLD),
          /*smallStep=*/1, /*largeStep=*/5, StrId::STR_UPLOAD_PERCENT_FORMAT,
          /*readerActivity=*/false, /*ignoreInitialConfirmRelease=*/true, StrId::STR_NONE_OPT, StrId::STR_ALWAYS),
      [this](const ActivityResult& result) {
        if (!result.isCancelled) {
          const auto* iv = std::get_if<IntervalResult>(&result.data);
          if (iv != nullptr) {
            KOREADER_STORE.setAutoUploadPercentThreshold(static_cast<uint8_t>(iv->value));
            KOREADER_STORE.saveToFile();
          }
        }
        requestUpdate();
      });
}

void KOReaderSettingsActivity::loop() {
  if (optionPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return;

  auto activateSelected = [this] {
    handleSelection();
    // Always redraw after selection so OptionPopup appears on the same press.
    requestUpdate();
  };

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    activateSelected();
    return;
  }

  const int count = menuItemCount();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight =
      renderer.getScreenHeight() - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  int touchSel = static_cast<int>(selectedIndex);
  const auto listTouch = handleListTouch(touchSel, count, contentTop, contentHeight, false);
  if (listTouch != ListTouchResult::None) {
    selectedIndex = static_cast<size_t>(touchSel);
    if (listTouch == ListTouchResult::Activated) activateSelected();
    else requestUpdate();
    return;
  }

  buttonNavigator.onNext([this, count] {
    selectedIndex = (selectedIndex + 1) % count;
    requestUpdate();
  });
  buttonNavigator.onPrevious([this, count] {
    selectedIndex = (selectedIndex + count - 1) % count;
    requestUpdate();
  });
}

void KOReaderSettingsActivity::handleSelection() {
  const int detailIdx = gateDetailIndex();
  const int metaIdx = uploadMetaIndex();

  if (selectedIndex == kIdxUsername) {
    startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_KOREADER_USERNAME),
                                                                   KOREADER_STORE.getUsername(), 64, InputType::Text),
                           [this](const ActivityResult& result) {
                             if (!result.isCancelled) {
                               const auto& kb = std::get<KeyboardResult>(result.data);
                               KOREADER_STORE.setCredentials(kb.text, KOREADER_STORE.getPassword());
                               KOREADER_STORE.saveToFile();
                             }
                             requestUpdate();
                           });
  } else if (selectedIndex == kIdxPassword) {
    startActivityForResult(
        std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_KOREADER_PASSWORD),
                                                KOREADER_STORE.getPassword(), 64, InputType::Password),
        [this](const ActivityResult& result) {
          if (!result.isCancelled) {
            const auto& kb = std::get<KeyboardResult>(result.data);
            KOREADER_STORE.setCredentials(KOREADER_STORE.getUsername(), kb.text);
            KOREADER_STORE.saveToFile();
          }
          requestUpdate();
        });
  } else if (selectedIndex == kIdxSignUp) {
    if (!KOREADER_STORE.hasCredentials()) return;
    startActivityForResult(
        std::make_unique<KOReaderAuthActivity>(renderer, mappedInput, KOReaderAuthActivity::Mode::SIGN_UP),
        [](const ActivityResult&) {});
  } else if (selectedIndex == kIdxAuth) {
    if (!KOREADER_STORE.hasCredentials()) return;
    startActivityForResult(std::make_unique<KOReaderAuthActivity>(renderer, mappedInput), [](const ActivityResult&) {});
  } else if (selectedIndex == kIdxServer) {
    const std::string currentUrl = KOREADER_STORE.getServerUrl();
    const std::string prefillUrl = currentUrl.empty() ? "https://" : currentUrl;
    startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_SYNC_SERVER_URL),
                                                                   prefillUrl, 128, InputType::Url),
                           [this](const ActivityResult& result) {
                             if (!result.isCancelled) {
                               const auto& kb = std::get<KeyboardResult>(result.data);
                               const std::string urlToSave =
                                   (kb.text == "https://" || kb.text == "http://") ? "" : kb.text;
                               KOREADER_STORE.setServerUrl(urlToSave);
                               KOREADER_STORE.saveToFile();
                             }
                             requestUpdate();
                           });
  } else if (selectedIndex == kIdxMatch) {
    const auto current = KOREADER_STORE.getMatchMethod();
    const auto newMethod =
        (current == DocumentMatchMethod::FILENAME) ? DocumentMatchMethod::BINARY : DocumentMatchMethod::FILENAME;
    KOREADER_STORE.setMatchMethod(newMethod);
    KOREADER_STORE.saveToFile();
  } else if (selectedIndex == kIdxSyncBehavior) {
    const int current = static_cast<int>(syncBehaviorToDisplay(currentBehavior()));
    optionPopup.show(StrId::STR_SYNC_BEHAVIOR, syncBehaviorOptionNames, SYNC_BEHAVIOR_OPTION_COUNT, current,
                     [this](int idx) {
                       KOREADER_STORE.setSyncBehavior(syncBehaviorFromDisplay(static_cast<uint8_t>(idx)));
                       KOREADER_STORE.saveToFile();
                       clampSelectedIndex(selectedIndex);
                     });
  } else if (detailIdx >= 0 && static_cast<int>(selectedIndex) == detailIdx) {
    if (showPercentDetail()) {
      openPercentThresholdPicker();
    } else {
      openTimeIntervalPicker();
    }
  } else if (metaIdx >= 0 && static_cast<int>(selectedIndex) == metaIdx) {
    KOREADER_STORE.setSendMetadata(!KOREADER_STORE.getSendMetadata());
    KOREADER_STORE.saveToFile();
  }
}

void KOReaderSettingsActivity::render(RenderLock&&) {
  if (optionPopup.processRender(renderer, mappedInput)) return;

  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_KOREADER_SYNC));

  clampSelectedIndex(selectedIndex);
  const int count = menuItemCount();
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, count, static_cast<int>(selectedIndex),
      [](int index) { return std::string(I18N.get(menuNameAt(index))); }, nullptr, nullptr,
      [](int index) {
        if (index == kIdxUsername) {
          const auto& username = KOREADER_STORE.getUsername();
          return username.empty() ? std::string(tr(STR_NOT_SET)) : username;
        }
        if (index == kIdxPassword) {
          return KOREADER_STORE.getPassword().empty() ? std::string(tr(STR_NOT_SET)) : std::string("******");
        }
        if (index == kIdxSignUp || index == kIdxAuth) {
          return KOREADER_STORE.hasCredentials() ? std::string()
                                                 : std::string("[") + tr(STR_SET_CREDENTIALS_FIRST) + "]";
        }
        if (index == kIdxServer) {
          auto serverUrl = KOREADER_STORE.getServerUrl();
          if (!serverUrl.empty()) return serverUrl;
          std::string defaultUrl = KOREADER_STORE.getBaseUrl();
          const auto schemeEnd = defaultUrl.find("://");
          if (schemeEnd != std::string::npos) defaultUrl.erase(0, schemeEnd + 3);
          return std::string(tr(STR_DEFAULT_VALUE)) + ": " + defaultUrl;
        }
        if (index == kIdxMatch) {
          return KOREADER_STORE.getMatchMethod() == DocumentMatchMethod::FILENAME ? std::string(tr(STR_FILENAME))
                                                                                  : std::string(tr(STR_BINARY));
        }
        if (index == kIdxSyncBehavior) {
          return std::string(syncBehaviorLabel(currentBehavior()));
        }
        if (index == gateDetailIndex()) {
          if (showPercentDetail()) {
            const uint8_t thr = KOREADER_STORE.getAutoUploadPercentThreshold();
            if (thr == KOReaderCredentialStore::ALWAYS_PERCENT_THRESHOLD) {
              return std::string(tr(STR_ALWAYS));
            }
            char buf[16];
            snprintf(buf, sizeof(buf), tr(STR_UPLOAD_PERCENT_FORMAT), static_cast<unsigned>(thr));
            return std::string(buf);
          }
          char buf[48];
          formatIntervalMinutes(KOREADER_STORE.getAutoUploadIntervalMinutes(), buf, sizeof(buf));
          return std::string(buf);
        }
        if (index == uploadMetaIndex()) {
          return KOREADER_STORE.getSendMetadata() ? std::string(tr(STR_STATE_ON)) : std::string(tr(STR_STATE_OFF));
        }
        return std::string();
      },
      true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
