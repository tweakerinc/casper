#include "KOReaderCredentialStore.h"

#include <Logging.h>
#include <MD5Builder.h>
#include <ObfuscationUtils.h>
#include <cstdio>
#include <cstring>
#include <time.h>

#include <algorithm>

namespace {
constexpr char DEFAULT_SERVER_URL[] = "https://sync.koreader.rocks:443";
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
  // Evict oldest (index 0) when full — shift left and append.
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

  // Keep legacy fields so older builds still load something sensible.
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
  obfuscation::DecodeStatus status = obfuscation::DecodeStatus::INVALID;
  std::string pass = obfuscation::deobfuscateFromBase64(doc["password_obf"] | "", &status);
  if (status == obfuscation::DecodeStatus::LEGACY && !pass.empty()) {
    needsResave = true;
  }
  if (status == obfuscation::DecodeStatus::INVALID || status == obfuscation::DecodeStatus::EMPTY || pass.empty()) {
    pass = doc["password"] | "";
    if (!pass.empty()) needsResave = true;
  }
  if (status == obfuscation::DecodeStatus::INVALID && pass.empty()) {
    LOG_ERR("KRS", "Ignoring unreadable KOReader password");
  }

  setCredentials(user, pass);
  setServerUrl(doc["serverUrl"] | "");

  uint8_t method = doc["matchMethod"] | (uint8_t)0;
  if (method <= static_cast<uint8_t>(DocumentMatchMethod::BINARY)) {
    setMatchMethod(static_cast<DocumentMatchMethod>(method));
  } else {
    setMatchMethod(DocumentMatchMethod::FILENAME);
  }
  setSendMetadata(doc["sendMetadata"] | false);

  const JsonVariantConst behaviorValue = doc["syncBehavior"];
  const bool missingBehavior = behaviorValue.isNull();
  uint8_t behavior = behaviorValue | static_cast<uint8_t>(KOReaderSyncBehavior::ASK_EVERY_TIME);
  if (behavior <= static_cast<uint8_t>(KOReaderSyncBehavior::SMART)) {
    setSyncBehavior(static_cast<KOReaderSyncBehavior>(behavior));
    needsResave = needsResave || missingBehavior;
  } else {
    LOG_DBG("KRS", "Invalid syncBehavior %u in JSON, resetting to ASK_EVERY_TIME", behavior);
    setSyncBehavior(KOReaderSyncBehavior::ASK_EVERY_TIME);
    needsResave = true;
  }

  autoUploadOnClose = (doc["autoUploadOnClose"] | (uint8_t)0) != 0 ? 1 : 0;

  const uint8_t typeRaw = doc["autoUploadType"] | (uint8_t)0;
  if (typeRaw == static_cast<uint8_t>(AutoUploadType::PERCENT)) {
    autoUploadType = AutoUploadType::PERCENT;
  } else if (typeRaw == static_cast<uint8_t>(AutoUploadType::ADAPTIVE)) {
    autoUploadType = AutoUploadType::ADAPTIVE;
  } else {
    autoUploadType = AutoUploadType::TIME;
  }

  if (!doc["autoUploadIntervalMinutes"].isNull()) {
    autoUploadIntervalMinutes = clampIntervalMinutes(doc["autoUploadIntervalMinutes"] | DEFAULT_INTERVAL_MINUTES);
  } else if (!doc["autoUploadInterval"].isNull()) {
    autoUploadIntervalMinutes = clampIntervalMinutes(legacyIntervalToMinutes(doc["autoUploadInterval"] | (uint8_t)1));
    needsResave = true;
  } else {
    autoUploadIntervalMinutes = DEFAULT_INTERVAL_MINUTES;
  }

  autoUploadPercentThreshold =
      clampPercentThreshold(doc["autoUploadPercentThreshold"] | DEFAULT_PERCENT_THRESHOLD);

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
    // Will apply legacy stamp lazily to the first book that is closed.
    needsResave = true;
  }

  if (needsResave) {
    saveToFile();
  }
  return true;
}

void KOReaderCredentialStore::setCredentials(const std::string& user, const std::string& pass) {
  username = user;
  password = pass;
}

std::string KOReaderCredentialStore::getMd5Password() const {
  if (password.empty()) return "";
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
}

void KOReaderCredentialStore::setServerUrl(const std::string& url) { serverUrl = url; }

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

void KOReaderCredentialStore::setMatchMethod(DocumentMatchMethod method) { matchMethod = method; }

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

  // Migrate legacy global stamp for this book only if we have no per-book entry yet.
  uint32_t lastUnix = st ? st->lastUnix : 0;
  float lastPercent = st ? st->lastPercent : -1.0f;
  if (!st && (legacyLastAutoUploadUnix > 0 || legacyLastAutoUploadPercent >= 0.0f)) {
    lastUnix = legacyLastAutoUploadUnix;
    lastPercent = legacyLastAutoUploadPercent;
  }

  // Adaptive mode (and Always under Time/Percent) upload on every book exit.
  if (autoUploadType == AutoUploadType::ADAPTIVE) {
    return AutoUploadDecision::Upload;
  }

  if (autoUploadType == AutoUploadType::PERCENT) {
    // 0% threshold = Always.
    if (autoUploadPercentThreshold == ALWAYS_PERCENT_THRESHOLD) {
      return AutoUploadDecision::Upload;
    }
    if (currentBookPercent < 0.0f) {
      return AutoUploadDecision::SkipInvalid;
    }
    // Never uploaded this book → always establish a baseline.
    if (lastPercent < 0.0f) {
      LOG_DBG("KRS", "Auto-upload percent: first time for book (progress=%.2f)",
              static_cast<double>(currentBookPercent));
      return AutoUploadDecision::Upload;
    }
    // last=80, threshold=1 → need current >= 81
    const float need = lastPercent + static_cast<float>(autoUploadPercentThreshold);
    if (currentBookPercent + 0.001f < need) {
      LOG_DBG("KRS", "Auto-upload percent not met (now=%.2f last=%.2f need>=%.2f thr=%u)",
              static_cast<double>(currentBookPercent), static_cast<double>(lastPercent), static_cast<double>(need),
              static_cast<unsigned>(autoUploadPercentThreshold));
      return AutoUploadDecision::SkipPercentNotMet;
    }
    return AutoUploadDecision::Upload;
  }

  // TIME mode: 0 minutes = Always.
  if (autoUploadIntervalMinutes == ALWAYS_INTERVAL_MINUTES) {
    return AutoUploadDecision::Upload;
  }

  // Upload if never uploaded this book, or interval elapsed since that stamp.
  if (lastUnix == 0) {
    return AutoUploadDecision::Upload;
  }

  const time_t now = wallClockUnix();
  if (now == 0) {
    // No reliable wall clock: do not permanently block auto-upload.
    return AutoUploadDecision::Upload;
  }

  const uint32_t needSec = static_cast<uint32_t>(autoUploadIntervalMinutes) * 60UL;
  if (static_cast<uint32_t>(now) < lastUnix) {
    return AutoUploadDecision::Upload;  // clock rollback
  }
  const uint32_t elapsed = static_cast<uint32_t>(now) - lastUnix;
  if (elapsed < needSec) {
    LOG_DBG("KRS", "Auto-upload time not elapsed (%lu / %lu s) for this book", static_cast<unsigned long>(elapsed),
            static_cast<unsigned long>(needSec));
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
    // Placeholder so we know a successful upload happened without a real clock.
    st->lastUnix = 1;
  }

  if (currentBookPercent >= 0.0f) {
    st->lastPercent = currentBookPercent;
  }

  // Clear legacy global once we have per-book state.
  legacyLastAutoUploadUnix = 0;
  legacyLastAutoUploadPercent = -1.0f;

  saveToFile();
  LOG_DBG("KRS", "Auto-upload stamped book=%s unix=%lu percent=%.2f", key, static_cast<unsigned long>(st->lastUnix),
          static_cast<double>(st->lastPercent));
}

void KOReaderCredentialStore::setSendMetadata(bool enabled) {
  sendMetadata = enabled;
  LOG_DBG("KRS", "Set send metadata: %s", enabled ? "true" : "false");
}

void KOReaderCredentialStore::setSyncBehavior(KOReaderSyncBehavior behavior) {
  if (static_cast<uint8_t>(behavior) > static_cast<uint8_t>(KOReaderSyncBehavior::SMART)) {
    behavior = KOReaderSyncBehavior::ASK_EVERY_TIME;
  }
  syncBehavior = behavior;
  LOG_DBG("KRS", "Set sync behavior: %s", behavior == KOReaderSyncBehavior::SMART ? "Smart" : "Ask");
}
