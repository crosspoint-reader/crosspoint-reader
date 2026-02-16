#pragma once

#include <Logging.h>
#include <base64.h>
#include <esp_mac.h>
#include <mbedtls/base64.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace obfuscation {

// Hardware-unique XOR key derived from the ESP32 eFuse MAC address (6 bytes).
// Cached on first use so esp_efuse_mac_get_default() is only called once.
inline const uint8_t* getHwKey() {
  static uint8_t key[6] = {};
  static bool initialized = false;
  if (!initialized) {
    esp_efuse_mac_get_default(key);
    initialized = true;
  }
  return key;
}
inline constexpr size_t HW_KEY_LEN = 6;

// XOR obfuscate/deobfuscate in-place using hardware key (symmetric operation)
inline void xorTransform(std::string& data) {
  const uint8_t* key = getHwKey();
  for (size_t i = 0; i < data.size(); i++) {
    data[i] ^= key[i % HW_KEY_LEN];
  }
}

// Legacy overload for binary migration (uses the old per-store keys)
inline void xorTransform(std::string& data, const uint8_t* key, size_t keyLen) {
  for (size_t i = 0; i < data.size(); i++) {
    data[i] ^= key[i % keyLen];
  }
}

// Obfuscate a plaintext string and return base64-encoded result for JSON storage
inline String obfuscateToBase64(const std::string& plaintext) {
  if (plaintext.empty()) return "";
  std::string temp = plaintext;
  xorTransform(temp);
  return base64::encode(reinterpret_cast<const uint8_t*>(temp.data()), temp.size());
}

// Decode base64 and de-obfuscate back to plaintext
inline std::string deobfuscateFromBase64(const char* encoded) {
  if (encoded == nullptr || encoded[0] == '\0') return "";
  size_t encodedLen = strlen(encoded);
  size_t decodedLen = 0;
  mbedtls_base64_decode(nullptr, 0, &decodedLen, reinterpret_cast<const unsigned char*>(encoded), encodedLen);
  std::string result(decodedLen, '\0');
  mbedtls_base64_decode(reinterpret_cast<unsigned char*>(&result[0]), decodedLen, &decodedLen,
                        reinterpret_cast<const unsigned char*>(encoded), encodedLen);
  result.resize(decodedLen);
  xorTransform(result);
  return result;
}

// Self-test: round-trip obfuscation with hardware key. Logs PASS/FAIL.
inline void selfTest() {
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
  if (allPassed) {
    LOG_DBG("OBF", "Obfuscation self-test PASSED");
  }
}

}  // namespace obfuscation
