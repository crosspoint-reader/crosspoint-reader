#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// Pure, host-testable structural checks mirroring the on-flash layout of
// ESP-IDF's Secure Boot V2 signature sector -- esp/rom/secure_boot.h's
// ets_secure_boot_signature_t / ets_secure_boot_sig_block_t -- and the same
// structural gate secure_boot_signatures_app.c's validate_signature_block()
// applies (magic byte, CRC32, scheme version) before ever touching RSA/ECDSA
// math. FirmwareFlasher.cpp, which includes the real SDK headers, static_asserts
// every offset and size constant below against the actual struct layout on
// every ESP32 build, so this file never invents a format offset independently
// -- it just stays free of ESP-IDF includes so host unit tests can exercise it
// directly, the same way test/list_hit_test-style pure-logic extractions do
// elsewhere in this codebase.
//
// These are structural/integrity checks only, never cryptographic trust
// verification: nothing here checks the RSA/ECDSA signature bytes against any
// public key, so this cannot distinguish "signed with the right key" from
// "signed with any key that produces a structurally well-formed block."
namespace signed_image {

// ets_secure_boot_sig_block_t layout (esp/rom/secure_boot.h):
//   uint8_t  magic_byte;        offset 0
//   uint8_t  version;           offset 1
//   uint8_t  _reserved1;        offset 2
//   uint8_t  _reserved2;        offset 3
//   uint8_t  image_digest[32];  offset 4
//   ets_rsa_pubkey_t key;       offset 36
//   uint8_t  signature[384];
//   uint32_t block_crc;         offset 1196
//   uint8_t  _padding[16];
// } == 1216 bytes total.
inline constexpr size_t kBlockSize = 1216;          // sizeof(ets_secure_boot_sig_block_t)
inline constexpr size_t kSectorSize = 4096;          // sizeof(ets_secure_boot_signature_t) / SIG_BLOCK_PADDING
inline constexpr unsigned kNumBlocks = 3;            // SECURE_BOOT_NUM_BLOCKS
inline constexpr size_t kCrcCoveredLen = 1196;       // CRC_SIGN_BLOCK_LEN
inline constexpr size_t kDigestLen = 32;             // ESP_SECURE_BOOT_KEY_DIGEST_SHA_256_LEN / SHA-256 length
inline constexpr uint8_t kMagicByte = 0xE7;          // ETS_SECURE_BOOT_V2_SIGNATURE_MAGIC
inline constexpr uint8_t kSchemeRsa = 2;             // ESP_SECURE_BOOT_V2_RSA
inline constexpr uint8_t kSchemeEcdsa = 3;           // ESP_SECURE_BOOT_V2_ECDSA
inline constexpr size_t kOffMagic = 0;
inline constexpr size_t kOffVersion = 1;
inline constexpr size_t kOffImageDigest = 4;
inline constexpr size_t kOffBlockCrc = 1196;

// True if the `kBlockSize`-byte block at `block` passes the same structural
// gate ESP-IDF's own verifier applies: correct magic byte, `blockCrc32`
// (caller-supplied, computed with whatever CRC32 implementation is available
// in that build -- e.g. esp_rom_crc32_le(0, block, kCrcCoveredLen) on-device)
// matching the stored block_crc field, and a recognized Secure Boot V2 scheme
// in the version field.
inline bool isValidBlockStructure(const uint8_t* block, uint32_t blockCrc32) {
  if (block[kOffMagic] != kMagicByte) return false;
  uint32_t storedCrc;
  std::memcpy(&storedCrc, block + kOffBlockCrc, sizeof(storedCrc));
  if (storedCrc != blockCrc32) return false;
  const uint8_t version = block[kOffVersion];
  return version == kSchemeRsa || version == kSchemeEcdsa;
}

// True if `block`'s embedded image digest matches `expectedDigest`
// (kDigestLen bytes each).
inline bool blockDigestMatches(const uint8_t* block, const uint8_t* expectedDigest) {
  return std::memcmp(block + kOffImageDigest, expectedDigest, kDigestLen) == 0;
}

// Given the length of the plain image (header + segments + checksum padding +
// optional 32-byte hash trailer -- i.e. what validateImageFile() computes as
// `unsignedTotal`), returns the offset at which a trailing Secure Boot V2
// signature sector would start: the next kSectorSize-aligned boundary.
inline size_t sectorStartFor(size_t unsignedTotal) {
  return (unsignedTotal + (kSectorSize - 1)) & ~(kSectorSize - 1);
}

// The one legal total file length for the "plain image + one signature
// sector" layout, given the plain-image length.
inline size_t signedTotalFor(size_t unsignedTotal) { return sectorStartFor(unsignedTotal) + kSectorSize; }

}  // namespace signed_image
