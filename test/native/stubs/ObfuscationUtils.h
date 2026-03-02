#pragma once
// Stub for native unit tests — obfuscation is no-op (no hardware MAC available)
#include <cstddef>
#include <cstdint>
#include <string>

namespace obfuscation {

inline void xorTransform(std::string&) {}
inline void xorTransform(std::string&, const uint8_t*, size_t) {}
inline std::string obfuscateToBase64(const std::string& plaintext) { return plaintext; }
inline std::string deobfuscateFromBase64(const char* encoded, bool* ok = nullptr) {
  if (ok) *ok = true;
  return encoded ? std::string(encoded) : std::string{};
}
inline void selfTest() {}

}  // namespace obfuscation
