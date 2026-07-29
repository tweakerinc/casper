#ifdef SIMULATOR
#include "OtaUpdater.h"

bool OtaUpdater::isUpdateNewer() const { return false; }
const std::string& OtaUpdater::getLatestVersion() const { return latestVersion; }
OtaUpdater::OtaUpdaterError OtaUpdater::checkForUpdate() { return NO_UPDATE; }
OtaUpdater::OtaUpdaterError OtaUpdater::installUpdate(ProgressCallback, void*) { return NO_UPDATE; }
#else
#include <Arduino.h>
#include <Logging.h>
#include <ReleaseJsonParser.h>
#include <strings.h>

#include <cstring>

#include "HttpDownloader.h"
#include "OtaUpdater.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "network/WifiPowerSaveGuard.h"

// Casper OTA: GitHub Releases on the Casper fork only (never CrossPoint stock).
// Override at build time if the repo moves:
//   -DCASPER_OTA_RELEASE_URL=\"https://api.github.com/repos/OWNER/REPO/releases/latest\"
#ifndef CASPER_OTA_RELEASE_URL
#define CASPER_OTA_RELEASE_URL "https://api.github.com/repos/TweakerInc/casper/releases/latest"
#endif

#ifndef CROSSPOINT_VERSION
#define CROSSPOINT_VERSION "v0.1.0"
#endif

namespace {
constexpr char latestReleaseUrl[] = CASPER_OTA_RELEASE_URL;
// Preferred GitHub asset names: Casper-v0.1.0 / Casper-v0.1.0.bin (also firmware.bin).
constexpr size_t VERSION_SEGMENT_COUNT = 4;
constexpr size_t OTA_PROGRESS_UPDATE_BYTES = 64 * 1024;
constexpr int HTTP_TIMEOUT_MS = 30000;

struct ParsedVersion {
  int segments[VERSION_SEGMENT_COUNT] = {0, 0, 0, 0};
  bool valid = false;
};

bool isDigit(const char c) { return c >= '0' && c <= '9'; }

bool startsWithNumberAfterOptionalV(const char* version) {
  if (version == nullptr) return false;
  if ((version[0] == 'v' || version[0] == 'V') && isDigit(version[1])) return true;
  return isDigit(version[0]);
}

ParsedVersion parseVersion(const char* version) {
  ParsedVersion parsed;
  if (!startsWithNumberAfterOptionalV(version)) return parsed;

  const char* p = version;
  if (p[0] == 'v' || p[0] == 'V') ++p;

  size_t segmentIndex = 0;
  while (segmentIndex < VERSION_SEGMENT_COUNT) {
    if (!isDigit(*p)) {
      // Allow trailing build metadata after numbers we already parsed.
      if (segmentIndex > 0) {
        parsed.valid = true;
      }
      return parsed;
    }

    int value = 0;
    while (isDigit(*p)) {
      value = value * 10 + (*p - '0');
      ++p;
    }
    parsed.segments[segmentIndex] = value;
    ++segmentIndex;

    if (*p != '.') break;
    ++p;
  }

  parsed.valid = segmentIndex > 0;
  return parsed;
}

// Returns 1 if latest > current, -1 if latest < current, 0 if equal/incomparable.
int compareVersions(const char* latestVersion, const char* currentVersion) {
  const ParsedVersion latest = parseVersion(latestVersion);
  const ParsedVersion current = parseVersion(currentVersion);
  if (!latest.valid || !current.valid) {
    // Fall back to string inequality only when both look like tags.
    if (latestVersion && currentVersion && strcmp(latestVersion, currentVersion) != 0) {
      // Prefer latest if current is a long dev string (branch+sha).
      if (strchr(currentVersion, '-') != nullptr && parseVersion(latestVersion).valid) {
        return 1;
      }
    }
    return 0;
  }

  for (size_t i = 0; i < VERSION_SEGMENT_COUNT; ++i) {
    if (latest.segments[i] != current.segments[i]) {
      return latest.segments[i] > current.segments[i] ? 1 : -1;
    }
  }
  return 0;
}

/*
 * When esp_crt_bundle.h is included under Arduino, wrong headers can be pulled.
 * Extern the bundle attach the same way CrossInk does.
 */
extern "C" {
extern esp_err_t esp_crt_bundle_attach(void* conf);
}

size_t totalBytesReceived = 0;

esp_err_t release_manifest_event_handler(esp_http_client_event_t* event) {
  if (event->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
  if (event->data_len <= 0) return ESP_OK;

  auto* parser = static_cast<ReleaseJsonParser*>(event->user_data);
  if (parser == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }

  totalBytesReceived += static_cast<size_t>(event->data_len);
  parser->feed(static_cast<const char*>(event->data), event->data_len);
  return ESP_OK;
}

struct OtaInstallContext {
  size_t* processedSize = nullptr;
  size_t totalSize = 0;
  size_t lastProgressBytes = 0;
  int lastReportedPct = -1;
  OtaUpdater::ProgressCallback onProgress = nullptr;
  void* progressCtx = nullptr;
};

void notifyOtaProgress(OtaInstallContext* ctx, const bool force) {
  if (ctx == nullptr || ctx->onProgress == nullptr || ctx->processedSize == nullptr || ctx->totalSize == 0) return;

  const size_t processed = *ctx->processedSize;
  const int pct = static_cast<int>(static_cast<uint64_t>(processed) * 100 / ctx->totalSize);
  if (force || pct != ctx->lastReportedPct || processed - ctx->lastProgressBytes >= OTA_PROGRESS_UPDATE_BYTES) {
    ctx->lastReportedPct = pct;
    ctx->lastProgressBytes = processed;
    ctx->onProgress(ctx->progressCtx);
  }
}

}  // namespace

OtaUpdater::OtaUpdaterError OtaUpdater::checkForUpdate() {
  WifiPowerSaveGuard wifiPowerSaveGuard;

  updateAvailable = false;
  latestVersion.clear();
  otaUrl.clear();
  otaSize = 0;
  processedSize = 0;
  totalSize = 0;

  ReleaseJsonParser releaseParser;

  esp_http_client_config_t client_config = {};
  client_config.url = latestReleaseUrl;
  client_config.event_handler = release_manifest_event_handler;
  client_config.buffer_size = 4096;
  client_config.buffer_size_tx = 1024;
  client_config.user_data = &releaseParser;
  client_config.crt_bundle_attach = esp_crt_bundle_attach;
  client_config.timeout_ms = HTTP_TIMEOUT_MS;
  client_config.keep_alive_enable = true;

  totalBytesReceived = 0;
  LOG_INF("OTA", "Checking for update (current: %s) url=%s", CROSSPOINT_VERSION, latestReleaseUrl);

  esp_http_client_handle_t client_handle = esp_http_client_init(&client_config);
  if (!client_handle) {
    LOG_ERR("OTA", "HTTP client init failed");
    return INTERNAL_UPDATE_ERROR;
  }

  esp_err_t esp_err = esp_http_client_set_header(client_handle, "User-Agent", "Casper-ESP32-" CROSSPOINT_VERSION);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "set_header failed: %s", esp_err_to_name(esp_err));
    esp_http_client_cleanup(client_handle);
    return INTERNAL_UPDATE_ERROR;
  }

  esp_err = esp_http_client_perform(client_handle);
  const int status = esp_http_client_get_status_code(client_handle);
  esp_http_client_cleanup(client_handle);

  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "HTTP perform failed: %s", esp_err_to_name(esp_err));
    return HTTP_ERROR;
  }
  if (status < 200 || status >= 300) {
    LOG_ERR("OTA", "HTTP status %d", status);
    return HTTP_ERROR;
  }

  LOG_DBG("OTA", "Release JSON %zu bytes tag=%s firmware=%s", totalBytesReceived,
          releaseParser.foundTag() ? "yes" : "no", releaseParser.foundFirmware() ? "yes" : "no");

  if (!releaseParser.foundTag()) {
    LOG_ERR("OTA", "No tag_name in release JSON");
    return JSON_PARSE_ERROR;
  }

  latestVersion = releaseParser.getTagName();

  if (!releaseParser.foundFirmware()) {
    LOG_ERR("OTA", "No Casper-v* / firmware.bin asset on release %s", latestVersion.c_str());
    return NO_UPDATE;
  }

  otaUrl = releaseParser.getFirmwareUrl();
  otaSize = releaseParser.getFirmwareSize();
  totalSize = otaSize;
  updateAvailable = true;

  LOG_INF("OTA", "Found release %s size=%zu", latestVersion.c_str(), otaSize);
  return OK;
}

bool OtaUpdater::isUpdateNewer() const {
  if (!updateAvailable || latestVersion.empty()) {
    return false;
  }
  if (latestVersion == CROSSPOINT_VERSION) {
    return false;
  }
  const int comparison = compareVersions(latestVersion.c_str(), CROSSPOINT_VERSION);
  LOG_DBG("OTA", "Version compare latest=%s current=%s result=%d", latestVersion.c_str(), CROSSPOINT_VERSION,
          comparison);
  // Dev builds (branch+sha) parse poorly; if release is a clean tag and current is not equal, allow update
  // when compare is non-negative and latest looks like a version tag.
  if (comparison > 0) return true;
  if (comparison == 0 && latestVersion != CROSSPOINT_VERSION) {
    // e.g. current v0.1-dev-... vs latest v0.1.0 — treat clean release as newer when base matches.
    const ParsedVersion latest = parseVersion(latestVersion.c_str());
    const ParsedVersion current = parseVersion(CROSSPOINT_VERSION);
    if (latest.valid && current.valid) {
      return false;  // truly equal numeric versions
    }
    if (latest.valid) {
      return true;  // clean release vs messy dev string
    }
  }
  return false;
}

const std::string& OtaUpdater::getLatestVersion() const { return latestVersion; }

OtaUpdater::OtaUpdaterError OtaUpdater::installUpdate(ProgressCallback onProgress, void* ctx) {
  if (!isUpdateNewer()) {
    return UPDATE_OLDER_ERROR;
  }
  if (otaUrl.empty()) {
    return INTERNAL_UPDATE_ERROR;
  }

  processedSize = 0;

  const esp_partition_t* updatePartition = esp_ota_get_next_update_partition(nullptr);
  if (updatePartition == nullptr) {
    LOG_ERR("OTA", "No OTA update partition found");
    return INTERNAL_UPDATE_ERROR;
  }

  if (otaSize > 0 && otaSize > updatePartition->size) {
    LOG_ERR("OTA", "Firmware too large: %zu > %u", otaSize, static_cast<unsigned>(updatePartition->size));
    return INTERNAL_UPDATE_ERROR;
  }

  WifiPowerSaveGuard wifiPowerSaveGuard;

  if (otaSize > 0) {
    totalSize = otaSize;
  }

  LOG_INF("OTA", "Downloading firmware: %s heap=%u maxAlloc=%u", otaUrl.c_str(), ESP.getFreeHeap(),
          ESP.getMaxAllocHeap());

  // Stream via HttpDownloader (wolfSSL when FREEINK_NET_WOLFSSL=1). Manual redirect
  // handling there avoids the esp_http_client auto-redirect crash:
  //   assert failed: http_utils_append_string ... (old_str)
  // which GitHub release CDN hops hit on open()+auto-redirect.
  esp_ota_handle_t otaHandle = 0;
  bool otaStarted = false;
  esp_err_t otaBeginError = ESP_OK;
  esp_err_t otaWriteError = ESP_OK;

  OtaInstallContext installCtx;
  installCtx.processedSize = &processedSize;
  installCtx.totalSize = totalSize > 0 ? totalSize : 1;
  installCtx.onProgress = onProgress;
  installCtx.progressCtx = ctx;

  const bool ok = HttpDownloader::fetchUrl(
      otaUrl,
      [&](const uint8_t* data, const size_t len) -> bool {
        if (!otaStarted) {
          const size_t firmwareSize = otaSize > 0 ? otaSize : OTA_SIZE_UNKNOWN;
          LOG_INF("OTA", "Writing firmware to %s @0x%x size=%zu heap=%u maxAlloc=%u", updatePartition->label,
                  static_cast<unsigned>(updatePartition->address), firmwareSize, ESP.getFreeHeap(),
                  ESP.getMaxAllocHeap());
          otaBeginError = esp_ota_begin(updatePartition, firmwareSize, &otaHandle);
          if (otaBeginError != ESP_OK) {
            LOG_ERR("OTA", "esp_ota_begin failed: %s (heap=%u maxAlloc=%u)", esp_err_to_name(otaBeginError),
                    ESP.getFreeHeap(), ESP.getMaxAllocHeap());
            return false;
          }
          otaStarted = true;
        }

        otaWriteError = esp_ota_write(otaHandle, data, len);
        if (otaWriteError != ESP_OK) {
          LOG_ERR("OTA", "esp_ota_write failed after %zu bytes: %s", processedSize, esp_err_to_name(otaWriteError));
          return false;
        }

        processedSize += len;
        if (totalSize == 0 || totalSize < processedSize) {
          // Content-Length unknown: grow progress denominator so UI doesn't spike.
          totalSize = processedSize + 1024 * 64;
          installCtx.totalSize = totalSize;
        }
        notifyOtaProgress(&installCtx, false);
        return true;
      });

  if (!ok) {
    if (otaStarted) {
      esp_ota_abort(otaHandle);
    }
    if (otaBeginError != ESP_OK) {
      return otaBeginError == ESP_ERR_NO_MEM ? OOM_ERROR : INTERNAL_UPDATE_ERROR;
    }
    if (otaWriteError != ESP_OK) {
      return INTERNAL_UPDATE_ERROR;
    }
    LOG_ERR("OTA", "Firmware download failed after %zu bytes", processedSize);
    return HTTP_ERROR;
  }

  if (!otaStarted || processedSize == 0) {
    LOG_ERR("OTA", "Firmware download returned no data");
    if (otaStarted) {
      esp_ota_abort(otaHandle);
    }
    return HTTP_ERROR;
  }
  if (otaSize > 0 && processedSize != otaSize) {
    LOG_ERR("OTA", "Size mismatch: got %zu expected %zu", processedSize, otaSize);
    esp_ota_abort(otaHandle);
    return INTERNAL_UPDATE_ERROR;
  }

  totalSize = processedSize;
  installCtx.totalSize = totalSize > 0 ? totalSize : 1;
  notifyOtaProgress(&installCtx, true);

  esp_err_t err = esp_ota_end(otaHandle);
  if (err != ESP_OK) {
    LOG_ERR("OTA", "esp_ota_end failed: %s", esp_err_to_name(err));
    return INTERNAL_UPDATE_ERROR;
  }

  err = esp_ota_set_boot_partition(updatePartition);
  if (err != ESP_OK) {
    LOG_ERR("OTA", "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
    return INTERNAL_UPDATE_ERROR;
  }

  LOG_INF("OTA", "Update complete: %zu bytes → reboot into new app", processedSize);
  return OK;
}
#endif
