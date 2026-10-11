#pragma once

#include <Arduino.h>

#include <cstddef>
#include <cstdint>
#include <string>

/**
 * Credential obfuscation utilities using the ESP32's unique hardware MAC address.
 *
 * XOR-based obfuscation with the 6-byte eFuse MAC as key. Not cryptographically
 * secure, but prevents casual reading of credentials on the SD card and ties
 * obfuscated data to the specific device (cannot be decoded on another chip or PC).
 *
 */
namespace obfuscation {

// XOR obfuscate/deobfuscate in-place using hardware MAC key (symmetric operation)
void xorTransform(std::string& data);

// Legacy overload for binary migration (uses the old per-store hardcoded keys)
void xorTransform(std::string& data, const uint8_t* key, size_t keyLen);

// Obfuscate a plaintext string: XOR with hardware key, then base64-encode for JSON storage
String obfuscateToBase64(const std::string& plaintext);

// Decode base64 and de-obfuscate back to plaintext.
// Returns empty string on invalid base64 input; sets *ok to false if decode fails.
std::string deobfuscateFromBase64(const char* encoded, bool* ok = nullptr);

// Bounded variant. Rejects input whose decoded form would exceed maxDecodedLength
// *before* allocating the result buffer, so a corrupt or hand-edited JSON blob
// cannot make us reserve kilobytes of contiguous heap. Reports that case through
// tooLong (which also sets *ok to false).
//
// writerKey, when given, is the 6-byte key of the device that obfuscated the
// value (see parseKeyId()), for data written on another reader.
std::string deobfuscateFromBase64(const char* encoded, size_t maxDecodedLength, bool* ok, bool* tooLong = nullptr,
                                  const uint8_t* writerKey = nullptr);

// This device's key as text, lowercase and dashed ("aa-bb-cc-dd-ee-ff"): the form
// wifi.json records beside the passwords, so a file carried to another reader
// still says which key its passwords need.
std::string deviceKeyId();

// Parses a dashed key id back into its 6 bytes. False on anything malformed.
bool parseKeyId(const std::string& id, uint8_t key[6]);

// Self-test: verifies round-trip obfuscation with hardware key. Logs PASS/FAIL.
void selfTest();

}  // namespace obfuscation
