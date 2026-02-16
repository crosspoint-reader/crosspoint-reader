#include "ObfuscationUtils.h"

#include <base64.h>
#include <esp_mac.h>
#include <mbedtls/base64.h>

#include <cstring>

// JSON_DEBUG: 0 = no logging, 1 = errors only, 2 = all
#ifndef JSON_DEBUG
#define JSON_DEBUG 0
#endif

#if JSON_DEBUG >= 2
#include <Logging.h>
#define JSON_LOG_DBG(tag, ...) LOG_DBG(tag, __VA_ARGS__)
#define JSON_LOG_ERR(tag, ...) LOG_ERR(tag, __VA_ARGS__)
#elif JSON_DEBUG >= 1
#include <Logging.h>
#define JSON_LOG_DBG(tag, ...) ((void)0)
#define JSON_LOG_ERR(tag, ...) LOG_ERR(tag, __VA_ARGS__)
#else
#define JSON_LOG_DBG(tag, ...) ((void)0)
#define JSON_LOG_ERR(tag, ...) ((void)0)
#endif

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
  if (ok) *ok = true;
  if (encoded == nullptr || encoded[0] == '\0') return "";
  size_t encodedLen = strlen(encoded);
  // First call: get required output buffer size
  size_t decodedLen = 0;
  int ret = mbedtls_base64_decode(nullptr, 0, &decodedLen, reinterpret_cast<const unsigned char*>(encoded), encodedLen);
  if (ret != 0 && ret != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) {
    JSON_LOG_ERR("OBF", "Base64 decode size query failed (ret=%d)", ret);
    if (ok) *ok = false;
    return "";
  }
  std::string result(decodedLen, '\0');
  ret = mbedtls_base64_decode(reinterpret_cast<unsigned char*>(&result[0]), decodedLen, &decodedLen,
                              reinterpret_cast<const unsigned char*>(encoded), encodedLen);
  if (ret != 0) {
    JSON_LOG_ERR("OBF", "Base64 decode failed (ret=%d)", ret);
    if (ok) *ok = false;
    return "";
  }
  result.resize(decodedLen);
  xorTransform(result);
  return result;
}

void selfTest() {
  const char* testInputs[] = {"", "hello", "WiFi P@ssw0rd!", "a"};
  bool allPassed = true;
  for (const char* input : testInputs) {
    String encoded = obfuscateToBase64(std::string(input));
    std::string decoded = deobfuscateFromBase64(encoded.c_str());
    if (decoded != input) {
      JSON_LOG_ERR("OBF", "FAIL: \"%s\" -> \"%s\" -> \"%s\"", input, encoded.c_str(), decoded.c_str());
      allPassed = false;
    }
  }
  // Verify obfuscated form differs from plaintext
  String enc = obfuscateToBase64("test123");
  if (enc == "test123") {
    JSON_LOG_ERR("OBF", "FAIL: obfuscated output identical to plaintext");
    allPassed = false;
  }
  if (allPassed) {
    JSON_LOG_DBG("OBF", "Obfuscation self-test PASSED");
  }
}

}  // namespace obfuscation
