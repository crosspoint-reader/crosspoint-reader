#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>

#include "lib/Serialization/CredentialIntegrity.h"

namespace {

std::string decodeBase64(const std::string_view encoded) {
  static constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string decoded;
  decoded.reserve(encoded.size() * 3 / 4);

  uint32_t accumulator = 0;
  unsigned bits = 0;
  for (const char ch : encoded) {
    if (ch == '=') break;
    const size_t value = alphabet.find(ch);
    if (value == std::string_view::npos) return {};
    accumulator = (accumulator << 6U) | static_cast<uint32_t>(value);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      decoded.push_back(static_cast<char>((accumulator >> bits) & 0xFFU));
    }
  }
  return decoded;
}

TEST(CredentialIntegrity, RejectsSameLengthBase64Corruption) {
  const std::string encoded = "cGFzc3dvcmQ=";  // "password"
  const std::string plaintext = decodeBase64(encoded);
  const uint32_t expectedCrc32 = credential_integrity::crc32(plaintext);

  std::string corrupted = encoded;
  corrupted[2] = corrupted[2] == 'A' ? 'B' : 'A';
  const std::string corruptedPlaintext = decodeBase64(corrupted);

  ASSERT_EQ(corrupted.size(), encoded.size());
  ASSERT_EQ(corruptedPlaintext.size(), plaintext.size());
  ASSERT_NE(corruptedPlaintext, plaintext);
  EXPECT_TRUE(credential_integrity::validate(plaintext, plaintext.size(), expectedCrc32));
  EXPECT_FALSE(credential_integrity::validate(corruptedPlaintext, plaintext.size(), expectedCrc32));
}

}  // namespace
