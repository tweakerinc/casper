#ifdef SIMULATOR
#include "OtaUpdater.h"

bool OtaUpdater::isUpdateNewer() const { return false; }
const std::string& OtaUpdater::getLatestVersion() const { return latestVersion; }
OtaUpdater::OtaUpdaterError OtaUpdater::checkForUpdate() { return NO_UPDATE; }
OtaUpdater::OtaUpdaterError OtaUpdater::installUpdate(ProgressCallback, void*) { return NO_UPDATE; }
#else
#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>
#include <ReleaseJsonParser.h>
#include <strings.h>

#include <cstring>

#include "FirmwareFlasher.h"
#include "HttpDownloader.h"
#include "OtaUpdater.h"
#include "esp_ota_ops.h"
#include "network/WifiPowerSaveGuard.h"

// Casper OTA: GitHub Releases on the Casper fork only.
// Override at build time if the repo moves:
//   -DCASPER_OTA_RELEASE_URL=\"https://api.github.com/repos/OWNER/REPO/releases/latest\"
#ifndef CASPER_OTA_RELEASE_URL
#define CASPER_OTA_RELEASE_URL "https://api.github.com/repos/TweakerInc/casper/releases/latest"
#endif

#ifndef CASPER_VERSION
#define CASPER_VERSION "v0.1.0"
#endif

namespace {
constexpr char latestReleaseUrl[] = CASPER_OTA_RELEASE_URL;
// Download to a hidden cache, then flash via the same path as SD-card update
// (raw partition write + otadata switch). Avoids esp_ota_end / esp_image_verify
// which reject our patched images on X4, and keeps TLS + flash erase from
// competing for the same internal-heap arena at once.
constexpr char kOtaCachePath[] = "/.crosspoint/ota-firmware.bin";
constexpr size_t VERSION_SEGMENT_COUNT = 4;
constexpr size_t OTA_PROGRESS_UPDATE_BYTES = 64 * 1024;

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
    if (latestVersion && currentVersion && strcmp(latestVersion, currentVersion) != 0) {
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

void removeOtaCache() {
  if (Storage.exists(kOtaCachePath)) {
    Storage.remove(kOtaCachePath);
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

  // Stream GitHub release JSON via HttpDownloader (wolfSSL when enabled). Do NOT
  // use a separate esp_http_client + mbedTLS session here: a dual TLS stack
  // fragments the ~320KB internal heap and the following firmware download OOMs.
  LOG_INF("OTA", "Checking for update (current: %s) heap=%u maxAlloc=%u url=%s", CASPER_VERSION, ESP.getFreeHeap(),
          ESP.getMaxAllocHeap(), latestReleaseUrl);

  size_t totalBytesReceived = 0;
  const bool ok = HttpDownloader::fetchUrl(latestReleaseUrl, [&](const uint8_t* data, const size_t len) -> bool {
    totalBytesReceived += len;
    releaseParser.feed(reinterpret_cast<const char*>(data), len);
    return true;
  });

  if (!ok) {
    LOG_ERR("OTA", "Release manifest fetch failed after %zu bytes heap=%u maxAlloc=%u", totalBytesReceived,
            ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    return HTTP_ERROR;
  }

  LOG_DBG("OTA", "Release JSON %zu bytes tag=%s firmware=%s heap=%u", totalBytesReceived,
          releaseParser.foundTag() ? "yes" : "no", releaseParser.foundFirmware() ? "yes" : "no", ESP.getFreeHeap());

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

  LOG_INF("OTA", "Found release %s size=%zu heap=%u maxAlloc=%u", latestVersion.c_str(), otaSize, ESP.getFreeHeap(),
          ESP.getMaxAllocHeap());
  return OK;
}

bool OtaUpdater::isUpdateNewer() const {
  if (!updateAvailable || latestVersion.empty()) {
    return false;
  }
  if (latestVersion == CASPER_VERSION) {
    return false;
  }
  const int comparison = compareVersions(latestVersion.c_str(), CASPER_VERSION);
  LOG_DBG("OTA", "Version compare latest=%s current=%s result=%d", latestVersion.c_str(), CASPER_VERSION, comparison);
  if (comparison > 0) return true;
  if (comparison == 0 && latestVersion != CASPER_VERSION) {
    const ParsedVersion latest = parseVersion(latestVersion.c_str());
    const ParsedVersion current = parseVersion(CASPER_VERSION);
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

  // Progress spans download + flash so the UI does not jump backward.
  // Phase 1: 0 .. half (download to SD). Phase 2: half .. total (flash).
  size_t downloadDenom = otaSize > 0 ? otaSize : (512 * 1024);  // provisional if Content-Length missing
  totalSize = downloadDenom * 2;

  OtaInstallContext installCtx;
  installCtx.processedSize = &processedSize;
  installCtx.totalSize = totalSize;
  installCtx.onProgress = onProgress;
  installCtx.progressCtx = ctx;

  LOG_INF("OTA", "Downloading firmware to SD: %s heap=%u maxAlloc=%u", otaUrl.c_str(), ESP.getFreeHeap(),
          ESP.getMaxAllocHeap());

  Storage.mkdir("/.crosspoint");
  removeOtaCache();

  // Stream TLS download to SD only — do not call esp_ota_write while the TLS
  // session is open. Measured OOM floor on C3 was ~2–8KB free contiguous when
  // erase/write + wolfSSL shared the arena for multi-minute GitHub CDN downloads.
  const auto dlErr =
      HttpDownloader::downloadToFile(otaUrl, kOtaCachePath, [&](size_t downloaded, size_t reportedTotal) {
        if (reportedTotal > 0 && reportedTotal != downloadDenom) {
          downloadDenom = reportedTotal;
          totalSize = downloadDenom * 2;
          installCtx.totalSize = totalSize;
        } else if (downloaded > downloadDenom) {
          downloadDenom = downloaded + 64 * 1024;
          totalSize = downloadDenom * 2;
          installCtx.totalSize = totalSize;
        }
        processedSize = downloaded;
        notifyOtaProgress(&installCtx, false);
      });

  if (dlErr != HttpDownloader::OK) {
    LOG_ERR("OTA", "Firmware download failed err=%d after %zu bytes heap=%u maxAlloc=%u", static_cast<int>(dlErr),
            processedSize, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    removeOtaCache();
    if (dlErr == HttpDownloader::ABORTED) return HTTP_ERROR;
    return HTTP_ERROR;
  }

  {
    HalFile check;
    if (!Storage.openFileForRead("OTA", kOtaCachePath, check) || !check) {
      LOG_ERR("OTA", "Downloaded file missing: %s", kOtaCachePath);
      removeOtaCache();
      return HTTP_ERROR;
    }
    const size_t got = check.fileSize();
    check.close();
    if (got == 0) {
      LOG_ERR("OTA", "Downloaded file empty");
      removeOtaCache();
      return HTTP_ERROR;
    }
    if (otaSize > 0 && got != otaSize) {
      LOG_ERR("OTA", "Size mismatch: got %zu expected %zu", got, otaSize);
      removeOtaCache();
      return INTERNAL_UPDATE_ERROR;
    }
    // Anchor end of download phase.
    processedSize = downloadDenom;
    notifyOtaProgress(&installCtx, true);
  }

  LOG_INF("OTA", "Flashing from SD heap=%u maxAlloc=%u", ESP.getFreeHeap(), ESP.getMaxAllocHeap());

  // Drop WiFi modem work during flash — flash path needs stable SPI + heap.
  // Parent activity will disconnect/silentRestart on exit; stay associated for now.
  const auto flashRes = firmware_flash::flashFromSdPath(
      kOtaCachePath,
      [](size_t written, size_t flashTotal, void* user) {
        auto* c = static_cast<OtaInstallContext*>(user);
        if (!c || !c->processedSize) return;
        // Map flash 0..flashTotal into second half of progress bar.
        const size_t half = c->totalSize / 2;
        if (flashTotal == 0) {
          *c->processedSize = half;
        } else {
          *c->processedSize = half + (written * half) / flashTotal;
        }
        notifyOtaProgress(c, false);
      },
      &installCtx, /*alreadyValidated=*/false);

  removeOtaCache();

  if (flashRes != firmware_flash::Result::OK) {
    LOG_ERR("OTA", "Flash failed: %s heap=%u maxAlloc=%u", firmware_flash::resultName(flashRes), ESP.getFreeHeap(),
            ESP.getMaxAllocHeap());
    if (flashRes == firmware_flash::Result::OOM) return OOM_ERROR;
    if (flashRes == firmware_flash::Result::BAD_CHIP) {
      LOG_ERR("OTA", "Firmware install aborted: wrong device (chip_id mismatch)");
      return INTERNAL_UPDATE_ERROR;
    }
    return INTERNAL_UPDATE_ERROR;
  }

  processedSize = totalSize;
  notifyOtaProgress(&installCtx, true);
  LOG_INF("OTA", "Update complete → reboot into new app");
  return OK;
}
#endif
