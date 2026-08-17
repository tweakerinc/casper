#include "ObfuscationUtils.h"

#include <Logging.h>
#include <base64.h>
#include <esp_mac.h>
#include <esp_random.h>
#include <mbedtls/base64.h>

#include <cstring>

namespace obfuscation {

namespace {
constexpr size_t HW_KEY_LEN = 6;

// Simple lazy init — no thread-safety concern on single-core ESP32-C3.
const uint8_t* getHwKey() {
  static uint8_t key[HW_KEY_LEN] = {};
  static bool initialized = false;
  if (!initialized) {
    esp_efuse_mac_get_default(key);
    initialized = true;
  }
  return key;
}
}  // namespace

void xorTransform(std::string& data) {
  const uint8_t* key = getHwKey();
  for (size_t i = 0; i < data.size(); i++) {
    data[i] ^= key[i % HW_KEY_LEN];
  }
}

void xorTransform(std::string& data, const uint8_t* key, size_t keyLen) {
  if (keyLen == 0 || key == nullptr) return;
  for (size_t i = 0; i < data.size(); i++) {
    data[i] ^= key[i % keyLen];
  }
}

String obfuscateToBase64(const std::string& plaintext) {
  if (plaintext.empty()) return "";
  std::string temp = plaintext;
  xorTransform(temp);
  return base64::encode(reinterpret_cast<const uint8_t*>(temp.data()), temp.size());
}

std::string deobfuscateFromBase64(const char* encoded, bool* ok) {
  if (encoded == nullptr || encoded[0] == '\0') {
    if (ok) *ok = false;
    return "";
  }
  if (ok) *ok = true;
  size_t encodedLen = strlen(encoded);
  // First call: get required output buffer size
  size_t decodedLen = 0;
  int ret = mbedtls_base64_decode(nullptr, 0, &decodedLen, reinterpret_cast<const unsigned char*>(encoded), encodedLen);
  if (ret != 0 && ret != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) {
    LOG_ERR("OBF", "Base64 decode size query failed (ret=%d)", ret);
    if (ok) *ok = false;
    return "";
  }
  std::string result(decodedLen, '\0');
  ret = mbedtls_base64_decode(reinterpret_cast<unsigned char*>(&result[0]), decodedLen, &decodedLen,
                              reinterpret_cast<const unsigned char*>(encoded), encodedLen);
  if (ret != 0) {
    LOG_ERR("OBF", "Base64 decode failed (ret=%d)", ret);
    if (ok) *ok = false;
    return "";
  }
  result.resize(decodedLen);
  xorTransform(result);
  return result;
}

namespace {
// Shared with legacy / upstream Casper wifi.json on SD.
constexpr char kWifiMagic[] = "CPV1";
constexpr size_t kWifiMagicLen = 4;
constexpr size_t kWifiSaltLen = 4;
}  // namespace

String obfuscateWifiPasswordToBase64(const std::string& plaintext) {
  // Envelope: CPV1 || 4-byte salt || password  (same layout legacy writes).
  std::string blob;
  blob.reserve(kWifiMagicLen + kWifiSaltLen + plaintext.size());
  blob.append(kWifiMagic, kWifiMagicLen);
  for (size_t i = 0; i < kWifiSaltLen; ++i) {
    blob.push_back(static_cast<char>(esp_random() & 0xFF));
  }
  blob.append(plaintext);
  xorTransform(blob);
  return base64::encode(reinterpret_cast<const uint8_t*>(blob.data()), blob.size());
}

std::string unwrapWifiPassword(std::string deobfuscated) {
  // Accept both envelope (legacy / Casper v0.1.5+) and bare password (older Casper).
  if (deobfuscated.size() <= kWifiMagicLen || deobfuscated.compare(0, kWifiMagicLen, kWifiMagic) != 0) {
    return deobfuscated;
  }
  deobfuscated.erase(0, kWifiMagicLen);
  if (deobfuscated.size() > kWifiSaltLen) {
    bool saltLooksBinary = false;
    for (size_t i = 0; i < kWifiSaltLen; ++i) {
      const unsigned char c = static_cast<unsigned char>(deobfuscated[i]);
      if (c < 32 || c > 126) {
        saltLooksBinary = true;
        break;
      }
    }
    if (saltLooksBinary) {
      deobfuscated.erase(0, kWifiSaltLen);
    }
  }
  return deobfuscated;
}

void selfTest() {
  const char* testInputs[] = {"", "hello", "WiFi P@ssw0rd!", "a"};
  bool allPassed = true;
  for (const char* input : testInputs) {
    String encoded = obfuscateToBase64(std::string(input));
    std::string decoded = deobfuscateFromBase64(encoded.c_str());
    if (decoded != input) {
      LOG_ERR("OBF", "FAIL: \"%s\" -> \"%s\" -> \"%s\"", input, encoded.c_str(), decoded.c_str());
      allPassed = false;
    }
  }
  // Verify obfuscated form differs from plaintext
  String enc = obfuscateToBase64("test123");
  if (enc == "test123") {
    LOG_ERR("OBF", "FAIL: obfuscated output identical to plaintext");
    allPassed = false;
  }
  // WiFi CPV1 envelope round-trip (non-empty passwords only; empty stays empty on disk).
  {
    const std::string pw = "IoT-test-pass";
    String wEnc = obfuscateWifiPasswordToBase64(pw);
    bool ok = false;
    std::string wDec = unwrapWifiPassword(deobfuscateFromBase64(wEnc.c_str(), &ok));
    if (!ok || wDec != pw) {
      LOG_ERR("OBF", "FAIL: wifi envelope round-trip");
      allPassed = false;
    }
  }
  if (allPassed) {
    LOG_DBG("OBF", "Obfuscation self-test PASSED");
  }
}

}  // namespace obfuscation
