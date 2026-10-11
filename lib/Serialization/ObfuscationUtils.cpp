#include "ObfuscationUtils.h"

#include <Logging.h>
#include <base64.h>
#include <esp_mac.h>
#include <wolfssl/wolfcrypt/coding.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

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
  return deobfuscateFromBase64(encoded, std::numeric_limits<size_t>::max(), ok, nullptr);
}

std::string deobfuscateFromBase64(const char* encoded, const size_t maxDecodedLength, bool* ok, bool* tooLong,
                                  const uint8_t* writerKey) {
  if (tooLong) *tooLong = false;
  if (encoded == nullptr || encoded[0] == '\0') {
    if (ok) *ok = false;
    return "";
  }
  if (ok) *ok = true;
  const size_t encodedLen = strlen(encoded);
  // Reject oversized input before allocating: a corrupt wifi.json with a multi-KB
  // blob would otherwise cost ~3/4 of it in contiguous heap. Test the *minimum*
  // the encoding could decode to (4 encoded chars -> 3 bytes, minus up to 2 bytes
  // of padding), so a value that would legitimately have fit is never rejected.
  if (maxDecodedLength != std::numeric_limits<size_t>::max()) {
    size_t minDecodedLen = (encodedLen / 4) * 3;
    if (minDecodedLen >= 2) minDecodedLen -= 2;
    if (minDecodedLen > maxDecodedLength) {
      if (ok) *ok = false;
      if (tooLong) *tooLong = true;
      return "";
    }
  }
  // Base64 decodes to at most 3/4 of the input length; +3 for rounding slack.
  word32 decodedLen = static_cast<word32>((encodedLen / 4) * 3 + 3);
  std::string result(decodedLen, '\0');
  const int ret = Base64_Decode(reinterpret_cast<const byte*>(encoded), static_cast<word32>(encodedLen),
                                reinterpret_cast<byte*>(&result[0]), &decodedLen);
  if (ret != 0) {
    LOG_ERR("OBF", "Base64 decode failed (ret=%d)", ret);
    if (ok) *ok = false;
    return "";
  }
  result.resize(decodedLen);
  // The pre-allocation check is on the minimum, so re-check the true length here.
  if (result.size() > maxDecodedLength) {
    if (ok) *ok = false;
    if (tooLong) *tooLong = true;
    return "";
  }
  if (writerKey) {
    xorTransform(result, writerKey, HW_KEY_LEN);
  } else {
    xorTransform(result);
  }
  return result;
}

std::string deviceKeyId() {
  const uint8_t* key = getHwKey();
  char id[18];
  snprintf(id, sizeof(id), "%02x-%02x-%02x-%02x-%02x-%02x", key[0], key[1], key[2], key[3], key[4], key[5]);
  return std::string(id);
}

bool parseKeyId(const std::string& id, uint8_t key[6]) {
  if (id.size() != 17) return false;
  for (size_t i = 0; i < HW_KEY_LEN; i++) {
    const size_t at = i * 3;
    if (i > 0 && id[at - 1] != '-') return false;
    if (!std::isxdigit(static_cast<unsigned char>(id[at])) || !std::isxdigit(static_cast<unsigned char>(id[at + 1]))) {
      return false;
    }
    key[i] = static_cast<uint8_t>(std::strtoul(id.substr(at, 2).c_str(), nullptr, 16));
  }
  return true;
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
  if (allPassed) {
    LOG_DBG("OBF", "Obfuscation self-test PASSED");
  }
}

}  // namespace obfuscation
