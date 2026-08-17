#include "KOReaderCredentialStore.h"

#include <Logging.h>
#include <MD5Builder.h>
#include <ObfuscationUtils.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace {
// Default sync server URL. Casper-sync speaks the full KOSync protocol, so
// pointing at any other kosync server (e.g. https://sync.koreader.rocks:443)
// still works via the custom server URL setting.
constexpr char DEFAULT_SERVER_URL[] = "https://sync.Casperreader.com";

// Default before config version 2. Configs saved without a version stamp and an
// empty serverUrl were implicitly syncing here — they get pinned on upgrade.
constexpr char LEGACY_DEFAULT_SERVER_URL[] = "https://sync.koreader.rocks:443";

// Bumped when a change to defaults would alter behavior for existing configs.
constexpr uint8_t CONFIG_VERSION = 2;

constexpr time_t MIN_VALID_UNIX = 1577836800;  // 2020-01-01 UTC

time_t wallClockUnix() {
  const time_t now = time(nullptr);
  return (now < MIN_VALID_UNIX) ? 0 : now;
}

// Legacy discrete interval enum (pre continuous minutes).
uint16_t legacyIntervalToMinutes(const uint8_t legacy) {
  switch (legacy) {
    case 0:  // EVERY_CLOSE
      return 1;
    case 1:
      return 60;
    case 2:
      return 3 * 60;
    case 3:
      return 6 * 60;
    case 4:
      return 12 * 60;
    case 5:
      return 24 * 60;
    default:
      return KOReaderCredentialStore::DEFAULT_INTERVAL_MINUTES;
  }
}

// FNV-1a 64-bit → 16 hex chars (stable per path, short for JSON keys).
void fnv1a64Hex(const char* s, char* out, const size_t outLen) {
  if (!out || outLen < 17) {
    if (out && outLen) out[0] = '\0';
    return;
  }
  uint64_t h = 14695981039346656037ULL;
  if (s) {
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s); *p; ++p) {
      h ^= static_cast<uint64_t>(*p);
      h *= 1099511628211ULL;
    }
  }
  snprintf(out, outLen, "%016llx", static_cast<unsigned long long>(h));
}
}  // namespace

uint16_t KOReaderCredentialStore::clampIntervalMinutes(const uint16_t minutes) {
  return std::clamp(minutes, MIN_INTERVAL_MINUTES, MAX_INTERVAL_MINUTES);
}

uint8_t KOReaderCredentialStore::clampPercentThreshold(const uint8_t percent) {
  return std::clamp(percent, MIN_PERCENT_THRESHOLD, MAX_PERCENT_THRESHOLD);
}

void KOReaderCredentialStore::makeBookKey(const char* bookPath, char* outKey, const size_t outKeyLen) {
  fnv1a64Hex(bookPath && bookPath[0] ? bookPath : "", outKey, outKeyLen);
}

const KOReaderCredentialStore::BookAutoUploadState* KOReaderCredentialStore::findBookState(const char* key) const {
  if (!key || !key[0]) return nullptr;
  for (size_t i = 0; i < bookStateCount; ++i) {
    if (strncmp(bookStates[i].key, key, kBookKeyLen) == 0) {
      return &bookStates[i];
    }
  }
  return nullptr;
}

KOReaderCredentialStore::BookAutoUploadState* KOReaderCredentialStore::findOrCreateBookState(const char* key) {
  if (!key || !key[0]) return nullptr;
  for (size_t i = 0; i < bookStateCount; ++i) {
    if (strncmp(bookStates[i].key, key, kBookKeyLen) == 0) {
      return &bookStates[i];
    }
  }
  if (bookStateCount >= kMaxBookStates) {
    for (size_t i = 1; i < kMaxBookStates; ++i) {
      bookStates[i - 1] = bookStates[i];
    }
    bookStateCount = kMaxBookStates - 1;
  }
  BookAutoUploadState& slot = bookStates[bookStateCount++];
  slot = BookAutoUploadState{};
  strncpy(slot.key, key, kBookKeyLen);
  slot.key[kBookKeyLen] = '\0';
  return &slot;
}

void KOReaderCredentialStore::toJson(JsonDocument& doc) const {
  doc["cfgVersion"] = CONFIG_VERSION;
  doc["username"] = getUsername();
  doc["password_obf"] = obfuscation::obfuscateToBase64(getPassword());
  doc["serverUrl"] = getServerUrl();
  doc["matchMethod"] = static_cast<uint8_t>(getMatchMethod());
  doc["sendMetadata"] = getSendMetadata();
  doc["syncBehavior"] = static_cast<uint8_t>(getSyncBehavior());
  doc["autoUploadOnClose"] = autoUploadOnClose;
  doc["autoUploadType"] = static_cast<uint8_t>(autoUploadType);
  doc["autoUploadIntervalMinutes"] = autoUploadIntervalMinutes;
  doc["autoUploadPercentThreshold"] = autoUploadPercentThreshold;

  if (bookStateCount > 0) {
    doc["lastAutoUploadUnix"] = bookStates[bookStateCount - 1].lastUnix;
    if (bookStates[bookStateCount - 1].lastPercent >= 0.0f) {
      doc["lastAutoUploadPercent"] = bookStates[bookStateCount - 1].lastPercent;
    }
  } else if (legacyLastAutoUploadUnix > 0) {
    doc["lastAutoUploadUnix"] = legacyLastAutoUploadUnix;
    if (legacyLastAutoUploadPercent >= 0.0f) {
      doc["lastAutoUploadPercent"] = legacyLastAutoUploadPercent;
    }
  }

  JsonObject books = doc["autoUploadBooks"].to<JsonObject>();
  for (size_t i = 0; i < bookStateCount; ++i) {
    const BookAutoUploadState& st = bookStates[i];
    if (st.key[0] == '\0') continue;
    JsonObject entry = books[st.key].to<JsonObject>();
    entry["u"] = st.lastUnix;
    if (st.lastPercent >= 0.0f) {
      entry["p"] = st.lastPercent;
    }
  }
}

bool KOReaderCredentialStore::fromJson(JsonVariantConst doc) {
  std::string user = doc["username"] | "";

  bool needsResave = false;
  std::string pass = extractPassword(doc, needsResave);

  setCredentials(user, pass);
  setServerUrl(doc["serverUrl"] | "");

  // The default server changed in config v2 (sync.koreader.rocks -> Casper-sync).
  const uint8_t cfgVersion = doc["cfgVersion"] | (uint8_t)1;
  if (cfgVersion < CONFIG_VERSION) {
    if (getServerUrl().empty() && hasCredentials()) {
      LOG_DBG("KRS", "Pre-v2 config used the old default server; pinning %s", LEGACY_DEFAULT_SERVER_URL);
      setServerUrl(LEGACY_DEFAULT_SERVER_URL);
    }
    needsResave = true;
  }

  uint8_t method = doc["matchMethod"] | (uint8_t)0;
  if (method <= static_cast<uint8_t>(DocumentMatchMethod::BINARY)) {
    setMatchMethod(static_cast<DocumentMatchMethod>(method));
  } else {
    LOG_DBG("KRS", "Invalid matchMethod %u in JSON, resetting to FILENAME", method);
    setMatchMethod(DocumentMatchMethod::FILENAME);
  }
  setSendMetadata(doc["sendMetadata"] | false);

  const JsonVariantConst behaviorValue = doc["syncBehavior"];
  const bool missingBehavior = behaviorValue.isNull();
  // Factory / missing key → OFF (disabled). Never default to Ask/Smart.
  // Existing 0–3 values keep their meaning when the key is present.
  uint8_t behaviorRaw = static_cast<uint8_t>(KOReaderSyncBehavior::OFF);
  if (!missingBehavior) {
    behaviorRaw = behaviorValue | static_cast<uint8_t>(KOReaderSyncBehavior::OFF);
  } else {
    LOG_DBG("KRS", "syncBehavior missing — default OFF (disabled)");
    needsResave = true;
  }
  if (behaviorRaw >= static_cast<uint8_t>(KOReaderSyncBehavior::COUNT)) {
    LOG_DBG("KRS", "Invalid syncBehavior %u in JSON, resetting to OFF", behaviorRaw);
    behaviorRaw = static_cast<uint8_t>(KOReaderSyncBehavior::OFF);
    needsResave = true;
  }

  // Legacy fields used only to migrate old dual Ask/Smart + Upload Type configs.
  const uint8_t legacyAutoOn = (doc["autoUploadOnClose"] | (uint8_t)0) != 0 ? 1 : 0;
  const uint8_t typeRaw = doc["autoUploadType"] | (uint8_t)0;
  AutoUploadType legacyType = AutoUploadType::TIME;
  if (typeRaw == static_cast<uint8_t>(AutoUploadType::PERCENT)) {
    legacyType = AutoUploadType::PERCENT;
  } else if (typeRaw == static_cast<uint8_t>(AutoUploadType::ADAPTIVE)) {
    legacyType = AutoUploadType::ADAPTIVE;
  }

  // If still Ask/Smart but auto-upload + Time/Percent were enabled, migrate to that mode.
  if (behaviorRaw == static_cast<uint8_t>(KOReaderSyncBehavior::ASK_EVERY_TIME) ||
      behaviorRaw == static_cast<uint8_t>(KOReaderSyncBehavior::SMART)) {
    if (legacyAutoOn != 0 && legacyType == AutoUploadType::PERCENT) {
      behaviorRaw = static_cast<uint8_t>(KOReaderSyncBehavior::PERCENT);
      needsResave = true;
    } else if (legacyAutoOn != 0 && legacyType == AutoUploadType::TIME) {
      behaviorRaw = static_cast<uint8_t>(KOReaderSyncBehavior::TIME);
      needsResave = true;
    }
  }

  setSyncBehavior(static_cast<KOReaderSyncBehavior>(behaviorRaw));
  needsResave = needsResave || missingBehavior;

  if (!doc["autoUploadIntervalMinutes"].isNull()) {
    autoUploadIntervalMinutes = clampIntervalMinutes(doc["autoUploadIntervalMinutes"] | DEFAULT_INTERVAL_MINUTES);
  } else if (!doc["autoUploadInterval"].isNull()) {
    autoUploadIntervalMinutes = clampIntervalMinutes(legacyIntervalToMinutes(doc["autoUploadInterval"] | (uint8_t)1));
    needsResave = true;
  } else {
    autoUploadIntervalMinutes = DEFAULT_INTERVAL_MINUTES;
  }

  autoUploadPercentThreshold = clampPercentThreshold(doc["autoUploadPercentThreshold"] | DEFAULT_PERCENT_THRESHOLD);

  legacyLastAutoUploadUnix = doc["lastAutoUploadUnix"] | (uint32_t)0;
  if (!doc["lastAutoUploadPercent"].isNull()) {
    legacyLastAutoUploadPercent = doc["lastAutoUploadPercent"] | -1.0f;
  } else {
    legacyLastAutoUploadPercent = -1.0f;
  }

  bookStateCount = 0;
  if (doc["autoUploadBooks"].is<JsonObjectConst>()) {
    for (JsonPairConst kv : doc["autoUploadBooks"].as<JsonObjectConst>()) {
      if (bookStateCount >= kMaxBookStates) break;
      const char* key = kv.key().c_str();
      if (!key || !key[0]) continue;
      JsonVariantConst v = kv.value();
      BookAutoUploadState& st = bookStates[bookStateCount++];
      st = BookAutoUploadState{};
      strncpy(st.key, key, kBookKeyLen);
      st.key[kBookKeyLen] = '\0';
      st.lastUnix = v["u"] | (uint32_t)0;
      if (!v["p"].isNull()) {
        st.lastPercent = v["p"] | -1.0f;
      }
    }
  } else if (legacyLastAutoUploadUnix > 0 || legacyLastAutoUploadPercent >= 0.0f) {
    needsResave = true;
  }

  if (needsResave) {
    LOG_DBG("KRS", "Resaving KOReader credentials to update format");
    requestResave();
  }

  return true;
}

void KOReaderCredentialStore::setCredentials(const std::string& user, const std::string& pass) {
  username = user;
  password = pass;
  LOG_DBG("KRS", "Set credentials for user: %s", user.c_str());
}

std::string KOReaderCredentialStore::getMd5Password() const {
  if (password.empty()) {
    return "";
  }
  MD5Builder md5;
  md5.begin();
  md5.add(password.c_str());
  md5.calculate();
  return md5.toString().c_str();
}

bool KOReaderCredentialStore::hasCredentials() const { return !username.empty() && !password.empty(); }

void KOReaderCredentialStore::clearCredentials() {
  username.clear();
  password.clear();
  saveToFile();
  LOG_DBG("KRS", "Cleared KOReader credentials");
}

void KOReaderCredentialStore::setServerUrl(const std::string& url) {
  serverUrl = url;
  LOG_DBG("KRS", "Set server URL: %s", url.empty() ? "(default)" : url.c_str());
}

std::string KOReaderCredentialStore::getBaseUrl() const {
  std::string url;
  if (serverUrl.empty()) {
    url = DEFAULT_SERVER_URL;
  } else if (serverUrl.find("://") == std::string::npos) {
    url = "http://" + serverUrl;
  } else {
    url = serverUrl;
  }
  while (!url.empty() && url.back() == '/') {
    url.pop_back();
  }
  return url;
}

void KOReaderCredentialStore::setMatchMethod(DocumentMatchMethod method) {
  matchMethod = method;
  LOG_DBG("KRS", "Set match method: %s", method == DocumentMatchMethod::FILENAME ? "Filename" : "Binary");
}

void KOReaderCredentialStore::setSendMetadata(bool enabled) {
  sendMetadata = enabled;
  LOG_DBG("KRS", "Set send metadata: %s", enabled ? "true" : "false");
}

void KOReaderCredentialStore::setSyncBehavior(KOReaderSyncBehavior behavior) {
  if (static_cast<uint8_t>(behavior) >= static_cast<uint8_t>(KOReaderSyncBehavior::COUNT)) {
    behavior = KOReaderSyncBehavior::OFF;
  }
  syncBehavior = behavior;
  // Legacy flags: Percent/Time enable gated upload-only on leave; others do not.
  if (behavior == KOReaderSyncBehavior::PERCENT) {
    autoUploadOnClose = 1;
    autoUploadType = AutoUploadType::PERCENT;
  } else if (behavior == KOReaderSyncBehavior::TIME) {
    autoUploadOnClose = 1;
    autoUploadType = AutoUploadType::TIME;
  } else {
    // Disabled / Ask / Smart: no upload-only gates (Disabled also skips interactive leave sync).
    autoUploadOnClose = 0;
    autoUploadType = AutoUploadType::ADAPTIVE;
  }
  LOG_DBG("KRS", "Set sync behavior: %u autoOn=%u", static_cast<unsigned>(behavior),
          static_cast<unsigned>(autoUploadOnClose));
}

void KOReaderCredentialStore::setAutoUploadOnClose(bool enabled) { autoUploadOnClose = enabled ? 1 : 0; }

void KOReaderCredentialStore::setAutoUploadType(AutoUploadType type) {
  if (type == AutoUploadType::PERCENT) {
    autoUploadType = AutoUploadType::PERCENT;
  } else if (type == AutoUploadType::ADAPTIVE) {
    autoUploadType = AutoUploadType::ADAPTIVE;
  } else {
    autoUploadType = AutoUploadType::TIME;
  }
}

void KOReaderCredentialStore::setAutoUploadIntervalMinutes(uint16_t minutes) {
  autoUploadIntervalMinutes = clampIntervalMinutes(minutes);
}

void KOReaderCredentialStore::setAutoUploadPercentThreshold(uint8_t percent) {
  autoUploadPercentThreshold = clampPercentThreshold(percent);
}

AutoUploadDecision KOReaderCredentialStore::evaluateAutoUpload(const char* bookPath,
                                                               const float currentBookPercent) const {
  // Upload-only gates only for Percent / Time (Ask / Smart use interactive leave sync).
  if (syncBehavior != KOReaderSyncBehavior::PERCENT && syncBehavior != KOReaderSyncBehavior::TIME) {
    return AutoUploadDecision::SkipDisabled;
  }
  if (autoUploadOnClose == 0) {
    return AutoUploadDecision::SkipDisabled;
  }
  if (!hasCredentials()) {
    return AutoUploadDecision::SkipNoCredentials;
  }
  if (!bookPath || !bookPath[0]) {
    return AutoUploadDecision::SkipInvalid;
  }

  char key[kBookKeyLen + 1];
  makeBookKey(bookPath, key, sizeof(key));
  const BookAutoUploadState* st = findBookState(key);

  // Per-book only. Never inherit a global/legacy last-upload from another book —
  // a never-uploaded title must always get a first baseline upload.
  if (!st) {
    return AutoUploadDecision::Upload;
  }

  const uint32_t lastUnix = st->lastUnix;
  const float lastPercent = st->lastPercent;

  // Percent: first stamp for this book, then require +threshold % since last upload.
  if (syncBehavior == KOReaderSyncBehavior::PERCENT) {
    if (autoUploadPercentThreshold == ALWAYS_PERCENT_THRESHOLD) {
      return AutoUploadDecision::Upload;
    }
    if (currentBookPercent < 0.0f) {
      return AutoUploadDecision::SkipInvalid;
    }
    if (lastPercent < 0.0f) {
      return AutoUploadDecision::Upload;
    }
    const float need = lastPercent + static_cast<float>(autoUploadPercentThreshold);
    if (currentBookPercent + 0.001f < need) {
      return AutoUploadDecision::SkipPercentNotMet;
    }
    return AutoUploadDecision::Upload;
  }

  // Time: first stamp for this book, then require interval since lastUnix.
  if (autoUploadIntervalMinutes == ALWAYS_INTERVAL_MINUTES) {
    return AutoUploadDecision::Upload;
  }
  if (lastUnix == 0) {
    return AutoUploadDecision::Upload;
  }
  const time_t now = wallClockUnix();
  if (now == 0 || static_cast<uint32_t>(now) < lastUnix) {
    return AutoUploadDecision::Upload;
  }
  const uint32_t needSec = static_cast<uint32_t>(autoUploadIntervalMinutes) * 60UL;
  const uint32_t elapsed = static_cast<uint32_t>(now) - lastUnix;
  if (elapsed < needSec) {
    return AutoUploadDecision::SkipTimeNotElapsed;
  }
  return AutoUploadDecision::Upload;
}

void KOReaderCredentialStore::markAutoUploadSucceeded(const char* bookPath, const float currentBookPercent) {
  if (!bookPath || !bookPath[0]) {
    LOG_ERR("KRS", "markAutoUploadSucceeded: empty path");
    return;
  }

  char key[kBookKeyLen + 1];
  makeBookKey(bookPath, key, sizeof(key));
  BookAutoUploadState* st = findOrCreateBookState(key);
  if (!st) {
    LOG_ERR("KRS", "markAutoUploadSucceeded: no slot");
    return;
  }

  const time_t now = wallClockUnix();
  if (now != 0) {
    st->lastUnix = static_cast<uint32_t>(now);
  } else if (st->lastUnix == 0) {
    st->lastUnix = 1;
  }

  if (currentBookPercent >= 0.0f) {
    st->lastPercent = currentBookPercent;
  }

  legacyLastAutoUploadUnix = 0;
  legacyLastAutoUploadPercent = -1.0f;

  saveToFile();
  LOG_DBG("KRS", "Auto-upload stamped book=%s unix=%lu percent=%.2f", key, static_cast<unsigned long>(st->lastUnix),
          static_cast<double>(st->lastPercent));
}
