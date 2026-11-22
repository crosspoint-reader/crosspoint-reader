#include "ObfuscationUtils.h"

#include <Logging.h>
#include <base64.h>
#include <esp_mac.h>
#include <mbedtls/base64.h>

#include <array>
#include <cstring>
#include <limits>

namespace obfuscation {

namespace {
constexpr size_t HW_KEY_LEN = 6;

const uint8_t* getHwKey() {
  // Function-local static initialization is synchronized by C++, including
  // on dual-core targets. The previous hand-rolled boolean could expose a
  // partially initialized key to another task.
  static const std::array<uint8_t, HW_KEY_LEN> key = [] {
    std::array<uint8_t, HW_KEY_LEN> value{};
    esp_efuse_mac_get_default(value.data());
    return value;
  }();
  return key.data();
}
}  // namespace

void xorTransform(std::string& data) {
  const uint8_t* key = getHwKey();
  for (size_t i = 0; i < data.size(); i++) {
    data[i] ^= key[i % HW_KEY_LEN];
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

std::string deobfuscateFromBase64(const char* encoded, const size_t maxDecodedLength, bool* ok, bool* tooLong) {
  if (tooLong) *tooLong = false;
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
  if (decodedLen > maxDecodedLength) {
    if (ok) *ok = false;
    if (tooLong) *tooLong = true;
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

}  // namespace obfuscation
