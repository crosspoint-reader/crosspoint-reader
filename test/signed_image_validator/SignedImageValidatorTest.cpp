#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include "src/network/SignedImageValidator.h"

namespace {

// Portable CRC-32 (poly 0xEDB88320, reflected -- the standard zlib/IEEE
// 802.3 variant esp_rom_crc32_le(seed, buf, len) implements, called with
// seed 0 by validate_signature_block() in secure_boot_signatures_app.c).
// Used only to build synthetic test fixtures; FirmwareFlasher.cpp itself
// calls the real ROM function on-device.
uint32_t crc32(const uint8_t* data, size_t len) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; bit++) {
      crc = (crc >> 1) ^ (0xEDB88320u & (~(crc & 1u) + 1u));
    }
  }
  return ~crc;
}

// Builds one synthetic 1216-byte Secure Boot V2 signature block. The
// key/signature bytes are arbitrary filler -- structural validation never
// inspects them (see SignedImageValidator.h's file comment for why), so no
// real cryptographic key material, throwaway or otherwise, is needed to
// exercise this logic.
std::array<uint8_t, signed_image::kBlockSize> makeBlock(uint8_t magic, uint8_t version,
                                                         const uint8_t digest[signed_image::kDigestLen],
                                                         bool correctCrc) {
  std::array<uint8_t, signed_image::kBlockSize> block{};
  block[signed_image::kOffMagic] = magic;
  block[signed_image::kOffVersion] = version;
  std::memcpy(block.data() + signed_image::kOffImageDigest, digest, signed_image::kDigestLen);
  // Fill the key/signature region with distinguishable, non-zero filler so a
  // test can't accidentally pass via an all-zero-initialized coincidence.
  for (size_t i = signed_image::kOffImageDigest + signed_image::kDigestLen; i < signed_image::kOffBlockCrc; i++) {
    block[i] = static_cast<uint8_t>(0xA5 + i);
  }
  const uint32_t crc = correctCrc ? crc32(block.data(), signed_image::kCrcCoveredLen) : 0xDEADBEEFu;
  std::memcpy(block.data() + signed_image::kOffBlockCrc, &crc, sizeof(crc));
  return block;
}

constexpr uint8_t kDigestA[signed_image::kDigestLen] = {1, 2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13,
                                                        14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25,
                                                        26, 27, 28, 29, 30, 31, 32};
constexpr uint8_t kDigestB[signed_image::kDigestLen] = {32, 31, 30, 29, 28, 27, 26, 25, 24, 23, 22, 21, 20,
                                                        19, 18, 17, 16, 15, 14, 13, 12, 11, 10, 9,  8,
                                                        7,  6,  5,  4,  3,  2,  1};

}  // namespace

TEST(SignedImageValidator, AcceptsStructurallyValidRsaBlock) {
  const auto block = makeBlock(signed_image::kMagicByte, signed_image::kSchemeRsa, kDigestA, /*correctCrc=*/true);
  const uint32_t crc = crc32(block.data(), signed_image::kCrcCoveredLen);
  EXPECT_TRUE(signed_image::isValidBlockStructure(block.data(), crc));
}

TEST(SignedImageValidator, AcceptsStructurallyValidEcdsaBlock) {
  const auto block = makeBlock(signed_image::kMagicByte, signed_image::kSchemeEcdsa, kDigestA, /*correctCrc=*/true);
  const uint32_t crc = crc32(block.data(), signed_image::kCrcCoveredLen);
  EXPECT_TRUE(signed_image::isValidBlockStructure(block.data(), crc));
}

TEST(SignedImageValidator, RejectsBadMagicByte) {
  const auto block = makeBlock(/*magic=*/0x00, signed_image::kSchemeRsa, kDigestA, /*correctCrc=*/true);
  const uint32_t crc = crc32(block.data(), signed_image::kCrcCoveredLen);
  EXPECT_FALSE(signed_image::isValidBlockStructure(block.data(), crc));
}

TEST(SignedImageValidator, RejectsUnrecognizedVersion) {
  // Version 0 is Secure Boot V1 ECDSA -- a real, defined scheme, but not a V2
  // signature sector layout, so it must not validate here.
  const auto block = makeBlock(signed_image::kMagicByte, /*version=*/0, kDigestA, /*correctCrc=*/true);
  const uint32_t crc = crc32(block.data(), signed_image::kCrcCoveredLen);
  EXPECT_FALSE(signed_image::isValidBlockStructure(block.data(), crc));
}

TEST(SignedImageValidator, RejectsBadCrc) {
  const auto block = makeBlock(signed_image::kMagicByte, signed_image::kSchemeRsa, kDigestA, /*correctCrc=*/false);
  // Pass the CRC the block *claims* to have computed correctly (i.e. what
  // validateImageFile() would independently compute over the real bytes) --
  // it won't match the deliberately-wrong stored value.
  const uint32_t crc = crc32(block.data(), signed_image::kCrcCoveredLen);
  EXPECT_FALSE(signed_image::isValidBlockStructure(block.data(), crc));
}

TEST(SignedImageValidator, RejectsOneByteCorruptionWithinCrcCoveredRegion) {
  // A single flipped bit anywhere in the CRC-covered region (which spans the
  // full 384-byte signature field, per CRC_SIGN_BLOCK_LEN) must be caught --
  // this is what makes "corrupted RSA signature bytes" detectable without
  // ever verifying the signature cryptographically.
  auto block = makeBlock(signed_image::kMagicByte, signed_image::kSchemeRsa, kDigestA, /*correctCrc=*/true);
  const uint32_t validCrc = crc32(block.data(), signed_image::kCrcCoveredLen);
  ASSERT_TRUE(signed_image::isValidBlockStructure(block.data(), validCrc));

  block[200] ^= 0x01;  // flip one bit inside the signature field
  const uint32_t crcAfterCorruption = crc32(block.data(), signed_image::kCrcCoveredLen);
  // The corrupted block's own recomputed CRC no longer matches the value
  // still stored in the block (unchanged by the flip), so validation fails
  // regardless of which CRC a caller happens to pass in.
  uint32_t storedCrc;
  std::memcpy(&storedCrc, block.data() + signed_image::kOffBlockCrc, sizeof(storedCrc));
  EXPECT_NE(crcAfterCorruption, storedCrc);
  EXPECT_FALSE(signed_image::isValidBlockStructure(block.data(), crcAfterCorruption));
}

TEST(SignedImageValidator, DigestMatchesOnlyTheExpectedDigest) {
  const auto block = makeBlock(signed_image::kMagicByte, signed_image::kSchemeRsa, kDigestA, /*correctCrc=*/true);
  EXPECT_TRUE(signed_image::blockDigestMatches(block.data(), kDigestA));
  EXPECT_FALSE(signed_image::blockDigestMatches(block.data(), kDigestB));
}

TEST(SignedImageValidator, UnusedSlotFilledWith0xFFIsNotAValidBlock) {
  // ESP-IDF's own convention for an unpopulated signature slot (see
  // calculate_image_public_key_digests() in secure_boot_signatures_app.c,
  // and the real bytes observed past the populated block in an actual signed
  // release artifact) -- must simply fail the gate, not be treated as an
  // error.
  std::array<uint8_t, signed_image::kBlockSize> block;
  block.fill(0xFF);
  const uint32_t crc = crc32(block.data(), signed_image::kCrcCoveredLen);
  EXPECT_FALSE(signed_image::isValidBlockStructure(block.data(), crc));
}

TEST(SignedImageValidator, MultipleValidBlocksWithDifferentKeysAllCount) {
  // Real multi-key co-signing: every populated slot independently passes the
  // structural gate and shares the same image digest (they all sign the same
  // image), even though their key/signature bytes differ. The validator must
  // count each one, not assume exactly one populated slot.
  const auto block0 = makeBlock(signed_image::kMagicByte, signed_image::kSchemeRsa, kDigestA, true);
  const auto block1 = makeBlock(signed_image::kMagicByte, signed_image::kSchemeEcdsa, kDigestA, true);
  unsigned validCount = 0;
  for (const auto* block : {&block0, &block1}) {
    const uint32_t crc = crc32(block->data(), signed_image::kCrcCoveredLen);
    if (signed_image::isValidBlockStructure(block->data(), crc) && signed_image::blockDigestMatches(block->data(), kDigestA)) {
      validCount++;
    }
  }
  EXPECT_EQ(validCount, 2u);
}

// -- Size-math coverage: the two legal total-length layouts --

TEST(SignedImageValidator, SectorStartAlignsUpToNextBoundaryWhenNotAligned) {
  EXPECT_EQ(signed_image::sectorStartFor(5135520), 5136384u);  // real observed release-artifact value
  EXPECT_EQ(signed_image::sectorStartFor(1), signed_image::kSectorSize);
  EXPECT_EQ(signed_image::sectorStartFor(signed_image::kSectorSize - 1), signed_image::kSectorSize);
}

TEST(SignedImageValidator, SectorStartStaysPutWhenAlreadyAligned) {
  EXPECT_EQ(signed_image::sectorStartFor(0), 0u);
  EXPECT_EQ(signed_image::sectorStartFor(signed_image::kSectorSize), signed_image::kSectorSize);
  EXPECT_EQ(signed_image::sectorStartFor(signed_image::kSectorSize * 3), signed_image::kSectorSize * 3);
}

TEST(SignedImageValidator, SignedTotalIsExactlyOneSectorPastSectorStart) {
  const size_t unsignedTotal = 5135520;
  const size_t expectedSectorStart = 5136384;
  EXPECT_EQ(signed_image::signedTotalFor(unsignedTotal), expectedSectorStart + signed_image::kSectorSize);
  // Matches the real observed release artifact total exactly.
  EXPECT_EQ(signed_image::signedTotalFor(unsignedTotal), 5140480u);
}

TEST(SignedImageValidator, SignedTotalAlwaysExceedsUnsignedTotal) {
  for (const size_t unsignedTotal : {size_t{0}, size_t{1}, size_t{4095}, size_t{4096}, size_t{5135520}}) {
    EXPECT_GT(signed_image::signedTotalFor(unsignedTotal), unsignedTotal);
  }
}
