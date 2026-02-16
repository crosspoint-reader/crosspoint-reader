#pragma once

#include <base64.h>
#include <mbedtls/base64.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace obfuscation {

// XOR obfuscate/deobfuscate in-place (symmetric operation)
inline void xorTransform(std::string& data, const uint8_t* key, size_t keyLen) {
  for (size_t i = 0; i < data.size(); i++) {
    data[i] ^= key[i % keyLen];
  }
}

// Obfuscate a plaintext string and return base64-encoded result for JSON storage
inline String obfuscateToBase64(const std::string& plaintext, const uint8_t* key, size_t keyLen) {
  if (plaintext.empty()) return "";
  std::string temp = plaintext;
  xorTransform(temp, key, keyLen);
  return base64::encode(reinterpret_cast<const uint8_t*>(temp.data()), temp.size());
}

// Decode base64 and de-obfuscate back to plaintext
inline std::string deobfuscateFromBase64(const char* encoded, const uint8_t* key, size_t keyLen) {
  if (encoded == nullptr || encoded[0] == '\0') return "";
  size_t encodedLen = strlen(encoded);
  // mbedtls_base64_decode: first call with NULL to get required output length
  size_t decodedLen = 0;
  mbedtls_base64_decode(nullptr, 0, &decodedLen, reinterpret_cast<const unsigned char*>(encoded), encodedLen);
  std::string result(decodedLen, '\0');
  mbedtls_base64_decode(reinterpret_cast<unsigned char*>(&result[0]), decodedLen, &decodedLen,
                        reinterpret_cast<const unsigned char*>(encoded), encodedLen);
  result.resize(decodedLen);
  xorTransform(result, key, keyLen);
  return result;
}

}  // namespace obfuscation
