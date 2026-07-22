#ifdef SIMULATOR
#include "OtaUpdater.h"

bool OtaUpdater::isUpdateNewer() const { return false; }
const std::string& OtaUpdater::getLatestVersion() const { return latestVersion; }
OtaUpdater::OtaUpdaterError OtaUpdater::checkForUpdate() { return NO_UPDATE; }
OtaUpdater::OtaUpdaterError OtaUpdater::installUpdate(ProgressCallback, void*, std::atomic<bool>*) { return NO_UPDATE; }
#else
#include <Arduino.h>
#include <Logging.h>
#include <ReleaseJsonParser.h>
#include <strings.h>

#include <cstring>
#include <utility>

#include "AppVersion.h"
#include "OtaUpdater.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "mbedtls/sha256.h"
#include "network/HttpDownloader.h"
#include "network/WifiPowerSaveGuard.h"

namespace {
#ifndef CROSSINK_OTA_RELEASE_URL
#define CROSSINK_OTA_RELEASE_URL "https://api.github.com/repos/uxjulia/CrossInk/releases/latest"
#endif

constexpr char latestReleaseUrl[] = CROSSINK_OTA_RELEASE_URL;

#ifdef CROSSPOINT_FIRMWARE_VARIANT
constexpr char firmwareAssetStem[] = "firmware-" CROSSPOINT_FIRMWARE_VARIANT;
constexpr char firmwareAssetName[] = "firmware-" CROSSPOINT_FIRMWARE_VARIANT ".bin";
#else
constexpr char firmwareAssetStem[] = "firmware";
constexpr char firmwareAssetName[] = "firmware.bin";
#endif

constexpr char binSuffix[] = ".bin";
constexpr size_t VERSION_SEGMENT_COUNT = 4;
constexpr size_t OTA_PROGRESS_UPDATE_BYTES = 64 * 1024;

struct ParsedVersion {
  int segments[VERSION_SEGMENT_COUNT] = {0, 0, 0, 0};
  bool valid = false;
  bool releaseCandidate = false;
};

bool isDigit(const char c) { return c >= '0' && c <= '9'; }

bool startsWithNumberAfterOptionalV(const char* version) {
  if (version == nullptr) return false;
  if ((version[0] == 'v' || version[0] == 'V') && isDigit(version[1])) return true;
  return isDigit(version[0]);
}

bool containsRcMarker(const char* version) {
  if (version == nullptr) return false;
  for (const char* p = version; p[0] != '\0' && p[1] != '\0' && p[2] != '\0'; ++p) {
    if (p[0] == '-' && (p[1] == 'r' || p[1] == 'R') && (p[2] == 'c' || p[2] == 'C')) {
      return true;
    }
  }
  return false;
}

ParsedVersion parseVersion(const char* version) {
  ParsedVersion parsed;
  if (!startsWithNumberAfterOptionalV(version)) return parsed;

  const char* p = version;
  if (p[0] == 'v' || p[0] == 'V') ++p;

  size_t segmentIndex = 0;
  while (segmentIndex < VERSION_SEGMENT_COUNT) {
    if (!isDigit(*p)) return parsed;

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

  parsed.valid = true;
  parsed.releaseCandidate = containsRcMarker(version);
  return parsed;
}

int compareVersions(const char* latestVersion, const char* currentVersion) {
  const ParsedVersion latest = parseVersion(latestVersion);
  const ParsedVersion current = parseVersion(currentVersion);
  if (!latest.valid || !current.valid) return 0;

  for (size_t i = 0; i < VERSION_SEGMENT_COUNT; ++i) {
    if (latest.segments[i] != current.segments[i]) {
      return latest.segments[i] > current.segments[i] ? 1 : -1;
    }
  }

  if (current.releaseCandidate && !latest.releaseCandidate) return 1;
  return 0;
}

bool startsWith(const char* value, const char* prefix) {
  if (value == nullptr || prefix == nullptr) return false;
  const size_t prefixLength = strlen(prefix);
  return strncmp(value, prefix, prefixLength) == 0;
}

char lowerHex(const uint8_t value) {
  return value < 10 ? static_cast<char>('0' + value) : static_cast<char>('a' + value - 10);
}

char asciiLower(const char c) { return (c >= 'A' && c <= 'F') ? static_cast<char>(c - 'A' + 'a') : c; }

bool isHexChar(const char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

bool isSha256Hex(const char* value) {
  if (value == nullptr || strlen(value) != 64) return false;
  for (size_t i = 0; i < 64; ++i) {
    if (!isHexChar(value[i])) return false;
  }
  return true;
}

bool sha256Matches(const uint8_t digest[32], const char* expectedHex) {
  if (!isSha256Hex(expectedHex)) return false;

  for (size_t i = 0; i < 32; ++i) {
    const char high = lowerHex((digest[i] >> 4) & 0x0F);
    const char low = lowerHex(digest[i] & 0x0F);
    if (high != asciiLower(expectedHex[i * 2]) || low != asciiLower(expectedHex[i * 2 + 1])) return false;
  }
  return true;
}

void formatSha256(const uint8_t digest[32], char output[65]) {
  for (size_t i = 0; i < 32; ++i) {
    output[i * 2] = lowerHex((digest[i] >> 4) & 0x0F);
    output[i * 2 + 1] = lowerHex(digest[i] & 0x0F);
  }
  output[64] = '\0';
}

bool isHttpUrl(const std::string& url) { return url.rfind("http://", 0) == 0; }

bool endsWith(const char* value, const char* suffix) {
  if (value == nullptr || suffix == nullptr) return false;
  const size_t valueLength = strlen(value);
  const size_t suffixLength = strlen(suffix);
  if (suffixLength > valueLength) return false;
  return strcmp(value + valueLength - suffixLength, suffix) == 0;
}

bool isMatchingFirmwareAssetName(const char* assetName) {
  if (assetName == nullptr) return false;
  if (strcmp(assetName, firmwareAssetName) == 0) return true;
  if (!startsWith(assetName, firmwareAssetStem)) return false;
  if (assetName[strlen(firmwareAssetStem)] != '-') return false;
  return endsWith(assetName, binSuffix);
}

/*
 * When esp_crt_bundle.h included, it is pointing wrong header file
 * which is something under WifiClientSecure because of our framework based on arduno platform.
 * To manage this obstacle, don't include anything, just extern and it will point correct one.
 */
extern "C" {
extern esp_err_t esp_crt_bundle_attach(void* conf);
}

size_t totalBytesReceived = 0;

struct OtaInstallContext {
  size_t* processedSize = nullptr;
  size_t totalSize = 0;
  size_t lastProgressBytes = 0;
  int lastReportedPct = -1;
  OtaUpdater::ProgressCallback onProgress = nullptr;
  void* progressCtx = nullptr;
};

esp_err_t release_manifest_event_handler(esp_http_client_event_t* event) {
  if (event->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
  if (event->data_len <= 0) return ESP_OK;

  auto* parser = static_cast<ReleaseJsonParser*>(event->user_data);
  if (parser == nullptr) {
    LOG_ERR("OTA", "HTTP client parser missing");
    return ESP_ERR_INVALID_ARG;
  }

  totalBytesReceived += static_cast<size_t>(event->data_len);
  LOG_DBG("OTA", "HTTP chunk: %d bytes (total: %zu)", event->data_len, totalBytesReceived);
  parser->feed(static_cast<const char*>(event->data), event->data_len);
  return ESP_OK;
}

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
  otaSha256.clear();
  otaSize = 0;
  processedSize = 0;
  totalSize = 0;

  esp_err_t esp_err;
  ReleaseJsonParser releaseParser(isMatchingFirmwareAssetName);

  esp_http_client_config_t client_config = {
      .url = latestReleaseUrl,
      .event_handler = release_manifest_event_handler,
      // 4096 holds the API response headers; the 32KB body streams through the
      // parser in chunks so RX needn't be larger. TX only carries our GET.
      // Both free before installUpdate, so smaller leaves it less fragmentation.
      .buffer_size = 4096,
      .buffer_size_tx = 1024,
      .user_data = &releaseParser,
      .crt_bundle_attach = esp_crt_bundle_attach,
      .keep_alive_enable = true,
  };

  totalBytesReceived = 0;
  LOG_DBG("OTA", "Checking for update (current: %s)", CROSSINK_VERSION);

  esp_http_client_handle_t client_handle = esp_http_client_init(&client_config);
  if (!client_handle) {
    LOG_ERR("OTA", "HTTP Client Handle Failed");
    return INTERNAL_UPDATE_ERROR;
  }

  esp_err = esp_http_client_set_header(client_handle, "User-Agent", "Casper-ESP32-" CROSSINK_VERSION);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_http_client_set_header Failed : %s", esp_err_to_name(esp_err));
    esp_http_client_cleanup(client_handle);
    return INTERNAL_UPDATE_ERROR;
  }

  esp_err = esp_http_client_perform(client_handle);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_http_client_perform Failed : %s", esp_err_to_name(esp_err));
    esp_http_client_cleanup(client_handle);
    return HTTP_ERROR;
  }

  esp_err = esp_http_client_cleanup(client_handle);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_http_client_cleanup Failed : %s", esp_err_to_name(esp_err));
    return INTERNAL_UPDATE_ERROR;
  }

  LOG_DBG("OTA", "Response received: %zu bytes total", totalBytesReceived);
  LOG_DBG("OTA", "Parser results: tag=%s firmware=%s", releaseParser.foundTag() ? "yes" : "no",
          releaseParser.foundFirmware() ? "yes" : "no");

  if (!releaseParser.foundTag()) {
    LOG_ERR("OTA", "No tag_name in release JSON");
    return JSON_PARSE_ERROR;
  }

  latestVersion = releaseParser.getTagName();

  if (!releaseParser.foundFirmware()) {
    LOG_ERR("OTA", "No matching %s asset found for release %s", firmwareAssetStem, latestVersion.c_str());
    return NO_UPDATE;
  }

  otaUrl = releaseParser.getFirmwareUrl();
  otaSha256 = releaseParser.getFirmwareSha256();
  otaSize = releaseParser.getFirmwareSize();
  totalSize = otaSize;
  updateAvailable = true;

  LOG_DBG("OTA", "Found update: tag=%s size=%zu sha256=%s", latestVersion.c_str(), otaSize,
          otaSha256.empty() ? "missing" : "present");
  LOG_DBG("OTA", "Firmware URL: %s", otaUrl.c_str());
  return OK;
}

bool OtaUpdater::isUpdateNewer() const {
  if (!updateAvailable || latestVersion.empty() || latestVersion == CROSSINK_VERSION) {
    return false;
  }

  const int comparison = compareVersions(latestVersion.c_str(), CROSSINK_VERSION);
  LOG_DBG("OTA", "Version comparison latest=%s current=%s result=%d", latestVersion.c_str(), CROSSINK_VERSION,
          comparison);
  return comparison > 0;
}

const std::string& OtaUpdater::getLatestVersion() const { return latestVersion; }

OtaUpdater::OtaUpdaterError OtaUpdater::installUpdate(ProgressCallback onProgress, void* ctx,
                                                      std::atomic<bool>* cancelRequested) {
  const auto isCancellationRequested = [cancelRequested]() -> bool {
    return cancelRequested != nullptr && cancelRequested->load(std::memory_order_relaxed);
  };

  if (!isUpdateNewer()) {
    return UPDATE_OLDER_ERROR;
  }

  if (isCancellationRequested()) {
    return CANCELLED_ERROR;
  }
  const bool hasManifestSha256 = isSha256Hex(otaSha256.c_str());
  if (!otaSha256.empty() && !hasManifestSha256) {
    LOG_ERR("OTA", "Refusing firmware with invalid manifest sha256");
    return JSON_PARSE_ERROR;
  }
  if (isHttpUrl(otaUrl) && !hasManifestSha256) {
    LOG_ERR("OTA", "Refusing HTTP firmware URL without manifest sha256");
    return JSON_PARSE_ERROR;
  }

  processedSize = 0;

  const esp_partition_t* updatePartition = esp_ota_get_next_update_partition(nullptr);
  if (updatePartition == nullptr) {
    LOG_ERR("OTA", "No OTA update partition found");
    return INTERNAL_UPDATE_ERROR;
  }

  if (otaSize > 0 && otaSize > updatePartition->size) {
    LOG_ERR("OTA", "Firmware too large: %zu > %zu", otaSize, updatePartition->size);
    return INTERNAL_UPDATE_ERROR;
  }

  esp_ota_handle_t otaHandle = 0;
  OtaInstallContext installCtx;
  installCtx.processedSize = &processedSize;
  installCtx.totalSize = totalSize;
  installCtx.onProgress = onProgress;
  installCtx.progressCtx = ctx;

  WifiPowerSaveGuard wifiPowerSaveGuard;

  LOG_INF("OTA", "Starting firmware download: url=%s heap=%u maxAlloc=%u", otaUrl.c_str(), ESP.getFreeHeap(),
          ESP.getMaxAllocHeap());

  mbedtls_sha256_context shaCtx;
  mbedtls_sha256_init(&shaCtx);
  mbedtls_sha256_starts(&shaCtx, /*is224=*/0);
  bool otaStarted = false;
  esp_err_t otaBeginError = ESP_OK;
  esp_err_t otaWriteError = ESP_OK;

  HttpDownloader::DownloadOptions downloadOptions;
  downloadOptions.shouldCancel = isCancellationRequested;
  // wolfSSL currently has no CA bundle, so only use it when the trusted release
  // manifest supplied a digest that pins the firmware bytes. Older HTTPS
  // releases without a digest retain the verified esp_http_client path.
  if (hasManifestSha256) downloadOptions.transport = HttpDownloader::Transport::WOLFSSL;
  const auto transferResult = HttpDownloader::streamUrl(
      otaUrl,
      [&](const uint8_t* data, const size_t len) {
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

        mbedtls_sha256_update(&shaCtx, data, len);
        processedSize += len;
        notifyOtaProgress(&installCtx, false);
        return true;
      },
      nullptr, "", "", std::move(downloadOptions));

  if (transferResult != HttpDownloader::OK) {
    mbedtls_sha256_free(&shaCtx);
    if (otaStarted) esp_ota_abort(otaHandle);
    if (transferResult == HttpDownloader::ABORTED || isCancellationRequested()) {
      LOG_INF("OTA", "Update cancelled");
      return CANCELLED_ERROR;
    }
    if (otaBeginError != ESP_OK) return otaBeginError == ESP_ERR_NO_MEM ? OOM_ERROR : INTERNAL_UPDATE_ERROR;
    if (otaWriteError != ESP_OK) return INTERNAL_UPDATE_ERROR;
    LOG_ERR("OTA", "Firmware download failed after %zu/%zu bytes", processedSize, totalSize);
    return HTTP_ERROR;
  }

  if (!otaStarted || processedSize == 0) {
    LOG_ERR("OTA", "Firmware download returned no data");
    mbedtls_sha256_free(&shaCtx);
    if (otaStarted) esp_ota_abort(otaHandle);
    return HTTP_ERROR;
  }
  if (otaSize > 0 && processedSize != otaSize) {
    LOG_ERR("OTA", "Firmware size mismatch: got %zu, expected %zu", processedSize, otaSize);
    mbedtls_sha256_free(&shaCtx);
    esp_ota_abort(otaHandle);
    return INTERNAL_UPDATE_ERROR;
  }

  notifyOtaProgress(&installCtx, true);

  uint8_t computedSha256[32];
  mbedtls_sha256_finish(&shaCtx, computedSha256);
  mbedtls_sha256_free(&shaCtx);
  if (hasManifestSha256) {
    if (!sha256Matches(computedSha256, otaSha256.c_str())) {
      char computedSha256Hex[65];
      formatSha256(computedSha256, computedSha256Hex);
      LOG_ERR("OTA", "Firmware sha256 mismatch: expected=%s actual=%s", otaSha256.c_str(), computedSha256Hex);
      esp_ota_abort(otaHandle);
      return HASH_MISMATCH_ERROR;
    }
    LOG_INF("OTA", "Firmware sha256 verified");
  }

  esp_err_t esp_err = esp_ota_end(otaHandle);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_ota_end failed: %s", esp_err_to_name(esp_err));
    return INTERNAL_UPDATE_ERROR;
  }

  esp_err = esp_ota_set_boot_partition(updatePartition);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_ota_set_boot_partition failed: %s", esp_err_to_name(esp_err));
    return INTERNAL_UPDATE_ERROR;
  }

  LOG_INF("OTA", "Update completed: %zu bytes", processedSize);
  return OK;
}
#endif
