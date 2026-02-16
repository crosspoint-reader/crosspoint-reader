#pragma once

#include <Arduino.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace obfuscation {

// XOR obfuscate/deobfuscate in-place using hardware key (symmetric operation)
void xorTransform(std::string& data);

// Legacy overload for binary migration (uses the old per-store keys)
void xorTransform(std::string& data, const uint8_t* key, size_t keyLen);

// Obfuscate a plaintext string and return base64-encoded result for JSON storage
String obfuscateToBase64(const std::string& plaintext);

// Decode base64 and de-obfuscate back to plaintext.
// Returns empty string on invalid base64 input; sets ok to false if provided.
std::string deobfuscateFromBase64(const char* encoded, bool* ok = nullptr);

// Self-test: round-trip obfuscation with hardware key. Logs PASS/FAIL.
void selfTest();

}  // namespace obfuscation
