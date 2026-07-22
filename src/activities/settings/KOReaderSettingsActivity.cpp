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
//  2 Server URL
//  3 Document Matching
//  4 Auto Upload on Close
//  [if auto OFF]
//    5 Authenticate
//  [if auto ON]
//    5 Upload Type (Time / Percent / Adaptive)
//    6 Time Interval | Percent Threshold | Sync Behavior  (by type)
//    7 Send Document Metadata   (all upload types)
//    8 Authenticate

constexpr int kFixedBeforeAuto = 5;  // 0..4

bool autoOn() { return KOREADER_STORE.getAutoUploadOnClose(); }

int menuItemCount() {
  // Fixed 5 + either Authenticate only (1) or type+sub+metadata+auth (4).
  return autoOn() ? (kFixedBeforeAuto + 4) : (kFixedBeforeAuto + 1);
}

int uploadTypeIndex() { return 5; }

// Row under Upload Type: interval, percent, or sync behavior (Adaptive only).
int typeDetailIndex() { return 6; }

int sendMetadataIndex() { return 7; }

int authIndex() { return autoOn() ? 8 : 5; }

void formatIntervalMinutes(const uint16_t minutes, char* buf, const size_t len) {
  if (minutes == KOReaderCredentialStore::ALWAYS_INTERVAL_MINUTES) {
    snprintf(buf, len, "%s", tr(STR_ALWAYS));
    return;
  }
  const unsigned h = minutes / 60u;
  const unsigned m = minutes % 60u;
  const char* hourUnit = (h == 1) ? tr(STR_HOUR) : tr(STR_HOURS);
  const char* minUnit = (m == 1) ? tr(STR_MINUTE) : tr(STR_MINUTES);
  snprintf(buf, len, "%u %s %u %s", h, hourUnit, m, minUnit);
}

const char* uploadTypeLabel(const AutoUploadType type) {
  switch (type) {
    case AutoUploadType::PERCENT:
      return tr(STR_PERCENT);
    case AutoUploadType::ADAPTIVE:
      return tr(STR_ADAPTIVE);
    case AutoUploadType::TIME:
    default:
      return tr(STR_TIME);
  }
}

AutoUploadType nextUploadType(const AutoUploadType current) {
  switch (current) {
    case AutoUploadType::TIME:
      return AutoUploadType::PERCENT;
    case AutoUploadType::PERCENT:
      return AutoUploadType::ADAPTIVE;
    case AutoUploadType::ADAPTIVE:
    default:
      return AutoUploadType::TIME;
  }
}

StrId typeDetailName() {
  switch (KOREADER_STORE.getAutoUploadType()) {
    case AutoUploadType::PERCENT:
      return StrId::STR_UPLOAD_PERCENT_THRESHOLD;
    case AutoUploadType::ADAPTIVE:
      return StrId::STR_SYNC_BEHAVIOR;
    case AutoUploadType::TIME:
    default:
      return StrId::STR_UPLOAD_TIME_INTERVAL;
  }
}

StrId menuNameAt(const int index) {
  if (index == 0) return StrId::STR_USERNAME;
  if (index == 1) return StrId::STR_PASSWORD;
  if (index == 2) return StrId::STR_SYNC_SERVER_URL;
  if (index == 3) return StrId::STR_DOCUMENT_MATCHING;
  if (index == 4) return StrId::STR_AUTO_UPLOAD_ON_CLOSE;
  if (index == authIndex()) return StrId::STR_AUTHENTICATE;

  if (!autoOn()) {
    return StrId::STR_USERNAME;
  }

  if (index == uploadTypeIndex()) return StrId::STR_UPLOAD_TYPE;
  if (index == typeDetailIndex()) return typeDetailName();
  if (index == sendMetadataIndex()) return StrId::STR_SEND_METADATA;
  return StrId::STR_USERNAME;
}

void clampSelectedIndex(size_t& selectedIndex) {
  const int count = menuItemCount();
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

void KOReaderSettingsActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finishAfterBackPress();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    handleSelection();
    return;
  }

  const int count = menuItemCount();
  buttonNavigator.onNext([this, count] {
    selectedIndex = (selectedIndex + 1) % count;
    requestUpdate();
  });
  buttonNavigator.onPrevious([this, count] {
    selectedIndex = (selectedIndex + count - 1) % count;
    requestUpdate();
  });
}

void KOReaderSettingsActivity::openTimeIntervalPicker() {
  startActivityForResult(
      std::make_unique<IntervalSelectionActivity>(
          renderer, mappedInput, "KoUploadTimeInterval", StrId::STR_UPLOAD_TIME_INTERVAL,
          static_cast<int>(KOREADER_STORE.getAutoUploadIntervalMinutes()),
          static_cast<int>(KOReaderCredentialStore::MIN_INTERVAL_MINUTES),
          static_cast<int>(KOReaderCredentialStore::MAX_INTERVAL_MINUTES),
          /*smallStep=*/1, /*largeStep=*/60, StrId::STR_NONE_OPT,
          /*readerActivity=*/false, /*allowPowerAsConfirm=*/false, /*ignoreInitialConfirmRelease=*/true,
          /*showPercentValue=*/false, StrId::STR_NONE_OPT, /*showHoursMinutes=*/true, StrId::STR_ALWAYS),
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
          /*smallStep=*/1, /*largeStep=*/5, StrId::STR_NONE_OPT,
          /*readerActivity=*/false, /*allowPowerAsConfirm=*/false, /*ignoreInitialConfirmRelease=*/true,
          /*showPercentValue=*/true, StrId::STR_NONE_OPT, /*showHoursMinutes=*/false, StrId::STR_ALWAYS),
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

void KOReaderSettingsActivity::handleSelection() {
  const int auth = authIndex();
  const int metaIdx = autoOn() ? sendMetadataIndex() : -1;
  const int typeIdx = autoOn() ? uploadTypeIndex() : -1;
  const int detailIdx = autoOn() ? typeDetailIndex() : -1;

  if (selectedIndex == 0) {
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
  } else if (selectedIndex == 1) {
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
  } else if (selectedIndex == 2) {
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
  } else if (selectedIndex == 3) {
    const auto current = KOREADER_STORE.getMatchMethod();
    const auto newMethod =
        (current == DocumentMatchMethod::FILENAME) ? DocumentMatchMethod::BINARY : DocumentMatchMethod::FILENAME;
    KOREADER_STORE.setMatchMethod(newMethod);
    KOREADER_STORE.saveToFile();
    requestUpdate();
  } else if (selectedIndex == 4) {
    KOREADER_STORE.setAutoUploadOnClose(!KOREADER_STORE.getAutoUploadOnClose());
    KOREADER_STORE.saveToFile();
    clampSelectedIndex(selectedIndex);
    requestUpdate();
  } else if (typeIdx >= 0 && static_cast<int>(selectedIndex) == typeIdx) {
    KOREADER_STORE.setAutoUploadType(nextUploadType(KOREADER_STORE.getAutoUploadType()));
    KOREADER_STORE.saveToFile();
    clampSelectedIndex(selectedIndex);
    requestUpdate();
  } else if (detailIdx >= 0 && static_cast<int>(selectedIndex) == detailIdx) {
    const auto type = KOREADER_STORE.getAutoUploadType();
    if (type == AutoUploadType::ADAPTIVE) {
      const auto current = KOREADER_STORE.getSyncBehavior();
      const auto next = (current == KOReaderSyncBehavior::ASK_EVERY_TIME) ? KOReaderSyncBehavior::SMART
                                                                           : KOReaderSyncBehavior::ASK_EVERY_TIME;
      KOREADER_STORE.setSyncBehavior(next);
      KOREADER_STORE.saveToFile();
      requestUpdate();
    } else if (type == AutoUploadType::PERCENT) {
      openPercentThresholdPicker();
    } else {
      openTimeIntervalPicker();
    }
  } else if (metaIdx >= 0 && static_cast<int>(selectedIndex) == metaIdx) {
    KOREADER_STORE.setSendMetadata(!KOREADER_STORE.getSendMetadata());
    KOREADER_STORE.saveToFile();
    requestUpdate();
  } else if (static_cast<int>(selectedIndex) == auth) {
    if (!KOREADER_STORE.hasCredentials()) {
      return;
    }
    startActivityForResult(std::make_unique<KOReaderAuthActivity>(renderer, mappedInput), [](const ActivityResult&) {});
  }
}

void KOReaderSettingsActivity::render(RenderLock&&) {
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
        if (index == 0) {
          const auto& username = KOREADER_STORE.getUsername();
          return username.empty() ? std::string(tr(STR_NOT_SET)) : username;
        }
        if (index == 1) {
          return KOREADER_STORE.getPassword().empty() ? std::string(tr(STR_NOT_SET)) : std::string("******");
        }
        if (index == 2) {
          const auto& serverUrl = KOREADER_STORE.getServerUrl();
          return serverUrl.empty() ? std::string(tr(STR_DEFAULT_VALUE)) : serverUrl;
        }
        if (index == 3) {
          return KOREADER_STORE.getMatchMethod() == DocumentMatchMethod::FILENAME ? std::string(tr(STR_FILENAME))
                                                                                  : std::string(tr(STR_BINARY));
        }
        if (index == 4) {
          return autoOn() ? std::string(tr(STR_YES)) : std::string(tr(STR_NO));
        }
        if (index == authIndex()) {
          return KOREADER_STORE.hasCredentials() ? std::string()
                                                 : std::string("[") + tr(STR_SET_CREDENTIALS_FIRST) + "]";
        }
        if (!autoOn()) {
          return std::string();
        }
        if (index == uploadTypeIndex()) {
          return std::string(uploadTypeLabel(KOREADER_STORE.getAutoUploadType()));
        }
        if (index == typeDetailIndex()) {
          const auto type = KOREADER_STORE.getAutoUploadType();
          if (type == AutoUploadType::ADAPTIVE) {
            return KOREADER_STORE.getSyncBehavior() == KOReaderSyncBehavior::SMART
                       ? std::string(tr(STR_SMART_SYNC))
                       : std::string(tr(STR_ASK_EVERY_TIME));
          }
          if (type == AutoUploadType::PERCENT) {
            const uint8_t thr = KOREADER_STORE.getAutoUploadPercentThreshold();
            if (thr == KOReaderCredentialStore::ALWAYS_PERCENT_THRESHOLD) {
              return std::string(tr(STR_ALWAYS));
            }
            char buf[16];
            snprintf(buf, sizeof(buf), "%u%%", static_cast<unsigned>(thr));
            return std::string(buf);
          }
          char buf[24];
          formatIntervalMinutes(KOREADER_STORE.getAutoUploadIntervalMinutes(), buf, sizeof(buf));
          return std::string(buf);
        }
        if (index == sendMetadataIndex()) {
          return KOREADER_STORE.getSendMetadata() ? std::string(tr(STR_STATE_ON)) : std::string(tr(STR_STATE_OFF));
        }
        return std::string();
      },
      true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
