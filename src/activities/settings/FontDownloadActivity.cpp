#include "FontDownloadActivity.h"

#include <Esp.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>
#include <esp_rom_crc.h>

#include <cctype>
#include <cstring>

#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/UiGhostPolicy.h"

namespace {
// Names match legacy fonts.json for families compiled into firmware (main.cpp).
// Compared case-insensitively; legacy Bitter/Lexend aliases accepted defensively.
bool isFirmwareBuiltinFamily(const char* name) {
  if (name == nullptr || name[0] == '\0') return false;
  return strcasecmp(name, "Literata") == 0 || strcasecmp(name, "SourceSerif4") == 0 ||
         strcasecmp(name, "Source Serif 4") == 0 || strcasecmp(name, "Sourcerer") == 0 ||
         strcasecmp(name, "Bitter") == 0 ||
         strcasecmp(name, "LexendDeca") == 0 || strcasecmp(name, "Lexend Deca") == 0;
}

// --- Minimal fonts.json (schema v1) scanner ---------------------------------
// Avoids ArduinoJson's full DOM (file size × ~2 plus our copies), which OOMs
// on ESP32-C3 once Wi‑Fi is up. Walks one in-memory copy of the file only.

void skipWs(const char*& p, const char* end) {
  while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) ++p;
}

bool parseJsonString(const char*& p, const char* end, std::string& out) {
  skipWs(p, end);
  if (p >= end || *p != '"') return false;
  ++p;
  out.clear();
  while (p < end) {
    const char c = *p++;
    if (c == '"') return true;
    if (c == '\\' && p < end) {
      const char e = *p++;
      switch (e) {
        case '"':
        case '\\':
        case '/':
          out.push_back(e);
          break;
        case 'b':
          out.push_back('\b');
          break;
        case 'f':
          out.push_back('\f');
          break;
        case 'n':
          out.push_back('\n');
          break;
        case 'r':
          out.push_back('\r');
          break;
        case 't':
          out.push_back('\t');
          break;
        case 'u':
          // Skip \uXXXX (manifest is ASCII for names/URLs).
          for (int i = 0; i < 4 && p < end; ++i) ++p;
          break;
        default:
          out.push_back(e);
          break;
      }
    } else {
      out.push_back(c);
    }
  }
  return false;
}

bool parseJsonUint(const char*& p, const char* end, uint32_t& out) {
  skipWs(p, end);
  if (p >= end || !std::isdigit(static_cast<unsigned char>(*p))) return false;
  uint64_t v = 0;
  while (p < end && std::isdigit(static_cast<unsigned char>(*p))) {
    v = v * 10u + static_cast<uint32_t>(*p - '0');
    if (v > 0xFFFFFFFFull) return false;
    ++p;
  }
  out = static_cast<uint32_t>(v);
  return true;
}

// Skip one JSON value (object/array/string/number/literal) without copying.
bool skipJsonValue(const char*& p, const char* end) {
  skipWs(p, end);
  if (p >= end) return false;
  if (*p == '"') {
    std::string discard;
    return parseJsonString(p, end, discard);
  }
  if (*p == '{' || *p == '[') {
    const char open = *p++;
    const char close = (open == '{') ? '}' : ']';
    int depth = 1;
    bool inStr = false;
    bool esc = false;
    while (p < end && depth > 0) {
      const char c = *p++;
      if (inStr) {
        if (esc) {
          esc = false;
        } else if (c == '\\') {
          esc = true;
        } else if (c == '"') {
          inStr = false;
        }
        continue;
      }
      if (c == '"') {
        inStr = true;
      } else if (c == open) {
        ++depth;
      } else if (c == close) {
        --depth;
      }
    }
    return depth == 0;
  }
  // number / true / false / null
  while (p < end && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\n' && *p != '\r' && *p != '\t') {
    ++p;
  }
  return true;
}

bool parseJsonKey(const char*& p, const char* end, std::string& key) { return parseJsonString(p, end, key); }

bool expectChar(const char*& p, const char* end, char c) {
  skipWs(p, end);
  if (p >= end || *p != c) return false;
  ++p;
  return true;
}

}  // namespace

FontDownloadActivity::FontDownloadActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("FontDownload", renderer, mappedInput), fontInstaller_(sdFontSystem.registry()) {}

// --- Lifecycle ---

void FontDownloadActivity::prepareHeapForNetwork() {
  // legacy: drop SD faces + catalog before Wi‑Fi. Casper also clears the
  // glyph cache — it fragments maxAlloc and competes with HTTP buffers.
  sdFontSystem.releaseForNetwork(renderer);
  if (FontCacheManager* fcm = renderer.getFontCacheManager()) {
    if (!fcm->isScanning()) fcm->clearCache();
  }
  WiFi.scanDelete();
  yield();
  LOG_INF("FONT", "Heap after releaseForNetwork: free=%u maxAlloc=%u", static_cast<unsigned>(ESP.getFreeHeap()),
          static_cast<unsigned>(ESP.getMaxAllocHeap()));
}

void FontDownloadActivity::onEnter() {
  Activity::onEnter();
  fontsChanged_ = false;
  pendingManifestFetch_ = false;
  // Free SD fonts *before* WIFI_STA — not after Wi‑Fi is already up (too late).
  prepareHeapForNetwork();
  WiFi.mode(WIFI_STA);
  auto wifi = makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput);
  if (!wifi) {
    LOG_ERR("FONT", "OOM creating WifiSelectionActivity free=%u maxAlloc=%u", static_cast<unsigned>(ESP.getFreeHeap()),
            static_cast<unsigned>(ESP.getMaxAllocHeap()));
    errorMessage_ = "Out of memory";
    state_ = ERROR;
    return;
  }
  startActivityForResult(std::move(wifi),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void FontDownloadActivity::onExit() {
  Activity::onExit();

  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    if (fontsChanged_) {
      // Installed/removed fonts: full reboot reloads SD faces cleanly (legacy).
      silentRestart();
      return;
    }
    WiFi.mode(WIFI_OFF);
  }

  // Browse-only / failed list: no reboot — restore SD selection if any.
  sdFontSystem.ensureLoaded(renderer);
}

void FontDownloadActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    finish();
    return;
  }

  // Drop residual scan tables before HTTP (can free a few KB).
  WiFi.scanDelete();

  {
    RenderLock lock(*this);
    state_ = LOADING_MANIFEST;
  }
  // Defer fetch to loop() so we are not nested under ActivityManager's result
  // callback stack when SecureHttpClient + parse run.
  pendingManifestFetch_ = true;
  requestUpdate();
}

void FontDownloadActivity::runPendingManifestFetch() {
  pendingManifestFetch_ = false;
  requestUpdateAndWait();  // paint "Loading…" first

  if (!fetchAndParseManifest()) {
    RenderLock lock(*this);
    state_ = ERROR;
    return;
  }

  {
    RenderLock lock(*this);
    state_ = FAMILY_LIST;
    selectedIndex_ = 0;
  }
  requestUpdate();
}

// --- Manifest fetching ---

bool FontDownloadActivity::parseManifestBuffer(const char* buf, const size_t len) {
  if (!buf || len == 0) return false;
  const char* p = buf;
  const char* end = buf + len;

  skipWs(p, end);
  if (!expectChar(p, end, '{')) return false;

  int version = -1;
  baseUrl_.clear();
  families_.clear();

  while (p < end) {
    skipWs(p, end);
    if (p < end && *p == '}') {
      ++p;
      break;
    }
    if (p < end && *p == ',') {
      ++p;
      continue;
    }

    std::string key;
    if (!parseJsonKey(p, end, key)) return false;
    if (!expectChar(p, end, ':')) return false;

    if (key == "version") {
      uint32_t v = 0;
      if (!parseJsonUint(p, end, v)) return false;
      version = static_cast<int>(v);
    } else if (key == "baseUrl") {
      if (!parseJsonString(p, end, baseUrl_)) return false;
    } else if (key == "families") {
      skipWs(p, end);
      if (!expectChar(p, end, '[')) return false;
      while (p < end) {
        skipWs(p, end);
        if (p < end && *p == ']') {
          ++p;
          break;
        }
        if (p < end && *p == ',') {
          ++p;
          continue;
        }
        if (!expectChar(p, end, '{')) return false;

        ManifestFamily family;
        while (p < end) {
          skipWs(p, end);
          if (p < end && *p == '}') {
            ++p;
            break;
          }
          if (p < end && *p == ',') {
            ++p;
            continue;
          }
          std::string fkey;
          if (!parseJsonKey(p, end, fkey)) return false;
          if (!expectChar(p, end, ':')) return false;

          if (fkey == "name") {
            if (!parseJsonString(p, end, family.name)) return false;
          } else if (fkey == "description") {
            if (!parseJsonString(p, end, family.description)) return false;
          } else if (fkey == "files") {
            skipWs(p, end);
            if (!expectChar(p, end, '[')) return false;
            while (p < end) {
              skipWs(p, end);
              if (p < end && *p == ']') {
                ++p;
                break;
              }
              if (p < end && *p == ',') {
                ++p;
                continue;
              }
              if (!expectChar(p, end, '{')) return false;
              ManifestFile file;
              bool sawCrc = false;
              while (p < end) {
                skipWs(p, end);
                if (p < end && *p == '}') {
                  ++p;
                  break;
                }
                if (p < end && *p == ',') {
                  ++p;
                  continue;
                }
                std::string ikey;
                if (!parseJsonKey(p, end, ikey)) return false;
                if (!expectChar(p, end, ':')) return false;
                if (ikey == "name") {
                  if (!parseJsonString(p, end, file.name)) return false;
                } else if (ikey == "size") {
                  uint32_t sz = 0;
                  if (!parseJsonUint(p, end, sz)) return false;
                  file.size = static_cast<size_t>(sz);
                } else if (ikey == "crc32") {
                  uint32_t crc = 0;
                  if (!parseJsonUint(p, end, crc)) return false;
                  file.crc32 = crc;
                  sawCrc = true;
                } else {
                  if (!skipJsonValue(p, end)) return false;
                }
              }
              if (file.name.empty() || !sawCrc) {
                errorMessage_ = "Invalid font manifest";
                return false;
              }
              family.totalSize += file.size;
              family.files.push_back(std::move(file));
            }
          } else {
            // styles[] and any future keys — skip without allocating.
            if (!skipJsonValue(p, end)) return false;
          }
        }
        if (!family.name.empty() && !family.files.empty()) {
          families_.push_back(std::move(family));
        }
      }
    } else {
      if (!skipJsonValue(p, end)) return false;
    }
  }

  if (version != FONTS_MANIFEST_VERSION) {
    LOG_ERR("FONT", "Unsupported manifest version: %d", version);
    errorMessage_ = "Unsupported manifest version";
    return false;
  }
  if (baseUrl_.empty() || families_.empty()) {
    errorMessage_ = "Invalid font manifest";
    return false;
  }
  return true;
}

void FontDownloadActivity::resolveInstalledFlags() {
  fontInstaller_.refreshRegistry();
  for (auto& family : families_) {
    const bool firmwareBuiltin = isFirmwareBuiltinFamily(family.name.c_str());
    family.installed = firmwareBuiltin || fontInstaller_.isFamilyInstalled(family.name.c_str());
    family.hasUpdate = false;

    if (family.installed && !firmwareBuiltin) {
      for (const auto& file : family.files) {
        char path[128];
        FontInstaller::buildFontPath(family.name.c_str(), file.name.c_str(), path, sizeof(path));
        HalFile f;
        if (Storage.openFileForRead("FONT", path, f)) {
          const size_t actual = f.fileSize();
          f.close();
          if (actual != file.size) {
            family.hasUpdate = true;
            break;
          }
        } else {
          family.hasUpdate = true;
          break;
        }
      }
    }
  }
}

bool FontDownloadActivity::fetchAndParseManifest() {
  static constexpr const char* MANIFEST_TMP = "/fonts_manifest.tmp";
  static constexpr int kManifestMaxAttempts = 5;
  static constexpr unsigned long kRetryDelayMs = 1500;

  baseUrl_.clear();
  families_.clear();

  // Extra headroom right before HTTP (Wi‑Fi may have allocated scan tables).
  prepareHeapForNetwork();

  LOG_INF("FONT", "Fetching font manifest: %s", FONT_MANIFEST_URL);
  Storage.remove(MANIFEST_TMP);
  HttpDownloader::DownloadError result = HttpDownloader::HTTP_ERROR;
  for (int attempt = 1; attempt <= kManifestMaxAttempts; ++attempt) {
    if (attempt > 1) {
      LOG_INF("FONT", "Retrying font manifest download (%d/%d)", attempt, kManifestMaxAttempts);
      delay(kRetryDelayMs);
    }
    result = HttpDownloader::downloadToFile(FONT_MANIFEST_URL, MANIFEST_TMP, nullptr);
    if (result == HttpDownloader::OK) break;
    LOG_ERR("FONT", "Font manifest download attempt failed (%d/%d, error=%d) free=%u maxAlloc=%u", attempt,
            kManifestMaxAttempts, static_cast<int>(result), static_cast<unsigned>(ESP.getFreeHeap()),
            static_cast<unsigned>(ESP.getMaxAllocHeap()));
  }
  if (result != HttpDownloader::OK) {
    LOG_ERR("FONT", "Failed to fetch manifest from %s", FONT_MANIFEST_URL);
    errorMessage_ = "Failed to fetch font list";
    Storage.remove(MANIFEST_TMP);
    return false;
  }

  HalFile manifestFile;
  if (!Storage.openFileForRead("FONT", MANIFEST_TMP, manifestFile)) {
    LOG_ERR("FONT", "Failed to open temp manifest");
    Storage.remove(MANIFEST_TMP);
    errorMessage_ = "Failed to read font list";
    return false;
  }

  const size_t fileSize = manifestFile.fileSize();
  // Cap: public catalog is ~30KB; reject absurd payloads.
  constexpr size_t kMaxManifestBytes = 96 * 1024;
  if (fileSize == 0 || fileSize > kMaxManifestBytes) {
    LOG_ERR("FONT", "Manifest size invalid: %zu", fileSize);
    manifestFile.close();
    Storage.remove(MANIFEST_TMP);
    errorMessage_ = "Invalid font manifest";
    return false;
  }

  LOG_INF("FONT", "Manifest on SD: %zu bytes free=%u maxAlloc=%u", fileSize, static_cast<unsigned>(ESP.getFreeHeap()),
          static_cast<unsigned>(ESP.getMaxAllocHeap()));

  if (ESP.getMaxAllocHeap() < fileSize + 4096) {
    LOG_ERR("FONT", "Not enough contiguous heap for manifest buffer");
    manifestFile.close();
    Storage.remove(MANIFEST_TMP);
    errorMessage_ = "Out of memory";
    return false;
  }

  auto buf = makeUniqueNoThrow<char[]>(fileSize + 1);
  if (!buf) {
    LOG_ERR("FONT", "OOM allocating %zu-byte manifest buffer", fileSize + 1);
    manifestFile.close();
    Storage.remove(MANIFEST_TMP);
    errorMessage_ = "Out of memory";
    return false;
  }

  size_t got = 0;
  while (got < fileSize) {
    const int n = manifestFile.read(reinterpret_cast<uint8_t*>(buf.get() + got), fileSize - got);
    if (n <= 0) break;
    got += static_cast<size_t>(n);
  }
  manifestFile.close();
  Storage.remove(MANIFEST_TMP);
  buf[got] = '\0';

  if (got != fileSize) {
    LOG_ERR("FONT", "Short read of manifest: %zu/%zu", got, fileSize);
    errorMessage_ = "Failed to read font list";
    return false;
  }

  if (!parseManifestBuffer(buf.get(), got)) {
    if (errorMessage_.empty()) errorMessage_ = "Invalid font manifest";
    LOG_ERR("FONT", "Manifest parse failed free=%u maxAlloc=%u", static_cast<unsigned>(ESP.getFreeHeap()),
            static_cast<unsigned>(ESP.getMaxAllocHeap()));
    families_.clear();
    return false;
  }
  // Drop file buffer before registry rediscover + stringy path checks.
  buf.reset();

  resolveInstalledFlags();

  LOG_INF("FONT", "Manifest loaded: %zu families free=%u maxAlloc=%u", families_.size(),
          static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
  return true;
}

// --- Download ---

void FontDownloadActivity::downloadAll() {
  cancelRequested_ = false;
  for (size_t i = 0; i < families_.size(); i++) {
    if (families_[i].installed) continue;
    downloadFamily(families_[i]);
    if (state_ == ERROR || cancelRequested_) return;
  }

  {
    RenderLock lock(*this);
    state_ = COMPLETE;
  }
}

void FontDownloadActivity::updateAll() {
  cancelRequested_ = false;
  for (size_t i = 0; i < families_.size(); i++) {
    if (!families_[i].hasUpdate) continue;
    downloadFamily(families_[i]);
    if (state_ == ERROR || cancelRequested_) return;
  }

  {
    RenderLock lock(*this);
    state_ = COMPLETE;
  }
}

bool FontDownloadActivity::showDownloadAllRow() const {
  for (const auto& f : families_) {
    if (!f.installed) return true;
  }
  return false;
}

bool FontDownloadActivity::showUpdateAllRow() const {
  for (const auto& f : families_) {
    if (f.hasUpdate) return true;
  }
  return false;
}

int FontDownloadActivity::specialRowCount() const {
  return (showDownloadAllRow() ? 1 : 0) + (showUpdateAllRow() ? 1 : 0);
}

bool FontDownloadActivity::isDownloadAllRow(int index) const { return showDownloadAllRow() && index == 0; }

bool FontDownloadActivity::isUpdateAllRow(int index) const {
  return showUpdateAllRow() && index == (showDownloadAllRow() ? 1 : 0);
}

int FontDownloadActivity::listItemCount() const {
  return families_.empty() ? 0 : static_cast<int>(families_.size()) + specialRowCount();
}

size_t FontDownloadActivity::totalDownloadSize() const {
  size_t total = 0;
  for (const auto& f : families_) {
    if (!f.installed) total += f.totalSize;
  }
  return total;
}

size_t FontDownloadActivity::totalUpdateSize() const {
  size_t total = 0;
  for (const auto& f : families_) {
    if (f.hasUpdate) total += f.totalSize;
  }
  return total;
}

// Standard CRC32 matching zlib/Python zlib.crc32().
bool FontDownloadActivity::computeFileCrc32(const char* path, uint32_t& outCrc) {
  HalFile f;
  if (!Storage.openFileForRead("FONT", path, f)) {
    return false;
  }
  constexpr size_t BUF_SIZE = 128;
  uint8_t buf[BUF_SIZE];
  uint32_t crc = 0;
  while (f.available()) {
    const int n = f.read(buf, BUF_SIZE);
    if (n <= 0) break;
    crc = esp_rom_crc32_le(crc, buf, static_cast<uint32_t>(n));
  }
  outCrc = crc;
  return true;
}

void FontDownloadActivity::downloadFamily(ManifestFamily& family) {
  {
    RenderLock lock(*this);
    state_ = DOWNLOADING;
    downloadingFamilyIndex_ = static_cast<int>(&family - families_.data());
    fileProgress_ = 0;
    fileTotal_ = 0;
    cancelRequested_ = false;
  }
  requestUpdateAndWait();

  // Free glyph cache + scan residue only — keep families_ and do not thrash
  // registry mid-catalog (list already resolved installed flags).
  if (FontCacheManager* fcm = renderer.getFontCacheManager()) {
    if (!fcm->isScanning()) fcm->clearCache();
  }
  WiFi.scanDelete();
  LOG_INF("FONT", "Heap before download %s: free=%u largest=%u", family.name.c_str(),
          static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
  delay(50);
  yield();

  if (!fontInstaller_.ensureFamilyDir(family.name.c_str())) {
    RenderLock lock(*this);
    state_ = ERROR;
    errorMessage_ = "Failed to create font directory";
    return;
  }

  for (size_t i = 0; i < family.files.size(); i++) {
    const auto& file = family.files[i];

    {
      RenderLock lock(*this);
      fileProgress_ = 0;
      fileTotal_ = file.size;
    }
    requestUpdateAndWait();

    char destPath[128];
    FontInstaller::buildFontPath(family.name.c_str(), file.name.c_str(), destPath, sizeof(destPath));

    std::string url = baseUrl_ + file.name;

    // Throttle UI refresh during download: full-screen e-ink redraws each tick
    // thrash the heap with TLS + framebuffers and have caused device crashes.
    unsigned long lastProgressUiMs = 0;
    auto result = HttpDownloader::downloadToFile(
        url, destPath,
        [this, &lastProgressUiMs](size_t downloaded, size_t total) {
          fileProgress_ = downloaded;
          fileTotal_ = total;
          mappedInput.update();
          if (mappedInput.isPressed(MappedInputManager::Button::Back) ||
              mappedInput.wasPressed(MappedInputManager::Button::Back)) {
            cancelRequested_ = true;
          }
          const unsigned long now = millis();
          if (now - lastProgressUiMs >= 750) {
            lastProgressUiMs = now;
            requestUpdate(true);
          }
        },
        &cancelRequested_);

    if (result == HttpDownloader::ABORTED) {
      fontInstaller_.deleteFamily(family.name.c_str());
      family.installed = false;
      family.hasUpdate = false;
      {
        RenderLock lock(*this);
        state_ = FAMILY_LIST;
      }
      return;
    }

    if (result != HttpDownloader::OK) {
      LOG_ERR("FONT", "Download failed: %s (%d)", file.name.c_str(), result);
      fontInstaller_.deleteFamily(family.name.c_str());
      family.installed = false;
      family.hasUpdate = false;
      RenderLock lock(*this);
      state_ = ERROR;
      errorMessage_ = "Download failed: " + file.name;
      return;
    }

    uint32_t actualCrc = 0;
    if (!computeFileCrc32(destPath, actualCrc)) {
      LOG_ERR("FONT", "Failed to open file for CRC check: %s", destPath);
      fontInstaller_.deleteFamily(family.name.c_str());
      family.installed = false;
      family.hasUpdate = false;
      RenderLock lock(*this);
      state_ = ERROR;
      errorMessage_ = "Failed to compute checksum: " + file.name;
      return;
    }
    if (actualCrc != file.crc32) {
      LOG_ERR("FONT", "CRC32 mismatch for %s: got %08x expected %08x", file.name.c_str(), actualCrc, file.crc32);
      fontInstaller_.deleteFamily(family.name.c_str());
      family.installed = false;
      family.hasUpdate = false;
      RenderLock lock(*this);
      state_ = ERROR;
      errorMessage_ = "Checksum mismatch: " + file.name;
      return;
    }
    LOG_DBG("FONT", "Downloaded %s (size=%zu crc32=%08x)", file.name.c_str(), file.size, actualCrc);

    if (!fontInstaller_.validateCpfontFile(destPath)) {
      LOG_ERR("FONT", "Invalid .cpfont: %s", destPath);
      fontInstaller_.deleteFamily(family.name.c_str());
      family.installed = false;
      family.hasUpdate = false;
      RenderLock lock(*this);
      state_ = ERROR;
      errorMessage_ = "Invalid font file: " + file.name;
      return;
    }
    currentFileIndex_++;
  }

  fontInstaller_.refreshRegistry();
  family.installed = true;
  family.hasUpdate = false;
  fontsChanged_ = true;

  {
    RenderLock lock(*this);
    state_ = COMPLETE;
  }
}

void FontDownloadActivity::promptDeleteSelectedFamily() {
  const int pendingDeleteFamilyIndex = familyIndexFromList(selectedIndex_);
  if (pendingDeleteFamilyIndex < 0 || pendingDeleteFamilyIndex >= static_cast<int>(families_.size())) {
    return;
  }

  std::string heading = tr(STR_DELETE);
  const auto& family = families_[pendingDeleteFamilyIndex];
  std::string body = family.name;
  auto confirm = makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, heading, body);
  if (!confirm) return;
  startActivityForResult(std::move(confirm),
                         [this](const ActivityResult& result) { onDeleteConfirmationResult(result); });
}

void FontDownloadActivity::onDeleteConfirmationResult(const ActivityResult& result) {
  if (result.isCancelled) {
    requestUpdate();
    return;
  }

  auto& family = families_[familyIndexFromList(selectedIndex_)];

  if (fontInstaller_.deleteFamily(family.name.c_str()) != FontInstaller::Error::OK) {
    RenderLock lock(*this);
    state_ = ERROR;
    errorMessage_ = "Failed to delete font";
  } else {
    fontInstaller_.refreshRegistry();
    family.installed = false;
    family.hasUpdate = false;
    fontsChanged_ = true;
  }

  requestUpdate();
}

bool FontDownloadActivity::isSelectedFamilyDeletable() const {
  if (isDownloadAllRow(selectedIndex_) || isUpdateAllRow(selectedIndex_)) return false;
  if (selectedIndex_ < specialRowCount() || selectedIndex_ >= listItemCount()) return false;
  const auto& family = families_[familyIndexFromList(selectedIndex_)];
  // Firmware builtins cannot be removed from flash via this UI.
  return family.installed && !family.hasUpdate && !isFirmwareBuiltinFamily(family.name.c_str());
}

// --- Input handling ---

void FontDownloadActivity::loop() {
  if (pendingManifestFetch_) {
    runPendingManifestFetch();
    return;
  }

  if (state_ == FAMILY_LIST) {
    auto activateSelected = [this] {
      if (families_.empty()) return;
      if (isDownloadAllRow(selectedIndex_)) {
        currentFileIndex_ = 0;
        currentFileTotal_ = 0;
        for (const auto& f : families_) {
          if (!f.installed) currentFileTotal_ += f.files.size();
        }
        downloadAll();
      } else if (isUpdateAllRow(selectedIndex_)) {
        currentFileIndex_ = 0;
        currentFileTotal_ = 0;
        for (const auto& f : families_) {
          if (f.hasUpdate) currentFileTotal_ += f.files.size();
        }
        updateAll();
      } else {
        auto& family = families_[familyIndexFromList(selectedIndex_)];
        if (!family.installed || family.hasUpdate) {
          currentFileIndex_ = 0;
          currentFileTotal_ = family.files.size();
          downloadFamily(family);
        } else if (isSelectedFamilyDeletable()) {
          promptDeleteSelectedFamily();
          return;
        } else {
          // Firmware builtin (or non-deletable): Confirm is a no-op.
          return;
        }
      }
      requestUpdateAndWait();
    };

    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
      return;
    }

    const int listSize = listItemCount();
    const int pageItems = UITheme::getNumberOfItemsPerPage(renderer, true, false, true, false);

    if (!families_.empty()) {
      const auto& metrics = UITheme::getInstance().getMetrics();
      const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
      const int contentHeight =
          renderer.getScreenHeight() - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
      switch (handleListTouch(selectedIndex_, listSize, contentTop, contentHeight, true)) {
        case ListTouchResult::Activated:
          activateSelected();
          return;
        case ListTouchResult::Consumed:
          return;
        case ListTouchResult::None:
          break;
      }

      const auto swipe = mappedInput.wasSwipe();
      if (swipe == MappedInputManager::SwipeDir::Up) {
        selectedIndex_ = ButtonNavigator::nextPageIndex(selectedIndex_, listSize, pageItems);
        requestUpdate();
        return;
      }
      if (swipe == MappedInputManager::SwipeDir::Down) {
        selectedIndex_ = ButtonNavigator::previousPageIndex(selectedIndex_, listSize, pageItems);
        requestUpdate();
        return;
      }
    }

    buttonNavigator_.onNextRelease([this, listSize] {
      selectedIndex_ = ButtonNavigator::nextIndex(selectedIndex_, listSize);
      requestUpdate();
    });

    buttonNavigator_.onPreviousRelease([this, listSize] {
      selectedIndex_ = ButtonNavigator::previousIndex(selectedIndex_, listSize);
      requestUpdate();
    });

    buttonNavigator_.onNextContinuous([this, listSize, pageItems] {
      selectedIndex_ = ButtonNavigator::nextPageIndex(selectedIndex_, listSize, pageItems);
      requestUpdate();
    });

    buttonNavigator_.onPreviousContinuous([this, listSize, pageItems] {
      selectedIndex_ = ButtonNavigator::previousPageIndex(selectedIndex_, listSize, pageItems);
      requestUpdate();
    });

    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      activateSelected();
      return;
    }
  } else if (state_ == COMPLETE) {
    int x = 0;
    int y = 0;
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(x, y)) {
      {
        RenderLock lock(*this);
        state_ = FAMILY_LIST;
      }
      requestUpdate();
    }
  } else if (state_ == ERROR) {
    // Manifest load failed (no list) or install failed — Back leaves the activity.
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      if (families_.empty()) {
        finish();
        return;
      }
      {
        RenderLock lock(*this);
        state_ = FAMILY_LIST;
      }
      requestUpdate();
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (families_.empty()) {
        // Retry list fetch
        {
          RenderLock lock(*this);
          state_ = LOADING_MANIFEST;
        }
        pendingManifestFetch_ = true;
        requestUpdate();
        return;
      }
      if (downloadingFamilyIndex_ >= 0 && downloadingFamilyIndex_ < static_cast<int>(families_.size())) {
        downloadFamily(families_[downloadingFamilyIndex_]);
        requestUpdateAndWait();
        return;
      }
      {
        RenderLock lock(*this);
        state_ = FAMILY_LIST;
      }
      requestUpdate();
    } else {
      int x = 0;
      int y = 0;
      if (mappedInput.wasScreenTapped(x, y)) {
        if (families_.empty()) {
          {
            RenderLock lock(*this);
            state_ = LOADING_MANIFEST;
          }
          pendingManifestFetch_ = true;
          requestUpdate();
          return;
        }
        if (downloadingFamilyIndex_ >= 0 && downloadingFamilyIndex_ < static_cast<int>(families_.size())) {
          downloadFamily(families_[downloadingFamilyIndex_]);
          requestUpdateAndWait();
          return;
        }
        {
          RenderLock lock(*this);
          state_ = FAMILY_LIST;
        }
        requestUpdate();
      }
    }
  } else if (state_ == LOADING_MANIFEST) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      cancelRequested_ = true;
      finish();
    }
  }
}

// --- Rendering ---

std::string FontDownloadActivity::formatSize(size_t bytes) {
  char buf[32];
  if (bytes >= 1024 * 1024) {
    snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
  } else if (bytes >= 1024) {
    snprintf(buf, sizeof(buf), "%.0f KB", static_cast<double>(bytes) / 1024.0);
  } else {
    snprintf(buf, sizeof(buf), "%zu B", bytes);
  }
  return buf;
}

void FontDownloadActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_FONT_BROWSER));

  const auto lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const auto contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const auto centerY = (pageHeight - lineHeight) / 2;

  if (state_ == LOADING_MANIFEST || pendingManifestFetch_) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_LOADING_FONT_LIST));
  } else if (state_ == FAMILY_LIST) {
    if (families_.empty()) {
      renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_NO_FONTS_AVAILABLE));
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    } else {
      GUI.drawList(
          renderer,
          Rect{0, contentTop, pageWidth, pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing},
          listItemCount(), selectedIndex_,
          [this](int index) -> std::string {
            if (isDownloadAllRow(index)) {
              return std::string(tr(STR_DOWNLOAD_ALL)) + " (" + formatSize(totalDownloadSize()) + ")";
            }
            if (isUpdateAllRow(index)) {
              return std::string(tr(STR_UPDATE_ALL)) + " (" + formatSize(totalUpdateSize()) + ")";
            }
            return families_[familyIndexFromList(index)].name;
          },
          [this](int index) -> std::string {
            if (isDownloadAllRow(index) || isUpdateAllRow(index)) return "";
            return families_[familyIndexFromList(index)].description;
          },
          nullptr,
          [this](int index) -> std::string {
            if (isDownloadAllRow(index) || isUpdateAllRow(index)) return "";
            const auto& f = families_[familyIndexFromList(index)];
            if (f.hasUpdate) return tr(STR_UPDATE_AVAILABLE);
            if (f.installed) return tr(STR_INSTALLED);
            return "";
          },
          true,
          [this](int index) -> bool {
            if (isDownloadAllRow(index) || isUpdateAllRow(index)) return false;
            const auto& f = families_[familyIndexFromList(index)];
            return f.installed && !f.hasUpdate;
          });

      const auto labels = mappedInput.mapLabels(tr(STR_BACK),
                                                isSelectedFamilyDeletable()      ? tr(STR_DELETE)
                                                : isUpdateAllRow(selectedIndex_) ? tr(STR_UPDATE)
                                                                                 : tr(STR_DOWNLOAD),
                                                tr(STR_DIR_UP), tr(STR_DIR_DOWN));
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    }
  } else if (state_ == DOWNLOADING) {
    const auto& family = families_[downloadingFamilyIndex_];

    std::string statusText = std::string(tr(STR_DOWNLOADING)) + " " + family.name + " (" +
                             std::to_string(currentFileIndex_ + 1) + "/" + std::to_string(currentFileTotal_) + ")";
    renderer.drawCenteredText(UI_10_FONT_ID, centerY - lineHeight, statusText.c_str());

    float progress = 0;
    if (fileTotal_ > 0) {
      progress = static_cast<float>(fileProgress_) / static_cast<float>(fileTotal_);
    }

    int barY = centerY + metrics.verticalSpacing;
    GUI.drawProgressBar(
        renderer,
        Rect{metrics.contentSidePadding, barY, pageWidth - metrics.contentSidePadding * 2, metrics.progressBarHeight},
        static_cast<int>(progress * 100), 100);

    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state_ == COMPLETE) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_FONT_INSTALLED), true, EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state_ == ERROR) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY - lineHeight, tr(STR_FONT_INSTALL_FAILED), true,
                              EpdFontFamily::BOLD);
    if (!errorMessage_.empty()) {
      renderer.drawCenteredText(UI_10_FONT_ID, centerY + metrics.verticalSpacing, errorMessage_.c_str());
    }
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_RETRY), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state_ == WIFI_SELECTION) {
    // Brief frame before Wi‑Fi UI takes over (or if Wi‑Fi activity failed to start).
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_LOADING_FONT_LIST));
  }

  UiGhostPolicy::displayMenuFrame(renderer);
}
