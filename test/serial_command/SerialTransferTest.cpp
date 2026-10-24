#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "util/SerialTransfer.h"

namespace {

using serialxfer::Access;
using serialxfer::PathKind;

// zlib's CRC-32, as esp_rom_crc32_le(0, ...) on the reader and zlib.crc32 on the host.
uint32_t crc32(uint32_t crc, const uint8_t* data, size_t size) {
  crc = ~crc;
  for (size_t i = 0; i < size; i++) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

TEST(SerialTransfer, Crc32MatchesZlib) {
  const char* check = "123456789";
  EXPECT_EQ(crc32(0, reinterpret_cast<const uint8_t*>(check), 9), 0xCBF43926u);
}

TEST(SerialTransfer, FrameHeaderRoundTrips) {
  uint8_t bytes[serialxfer::FRAME_HEADER_SIZE];
  serialxfer::encodeFrameHeader({0x01020304, 4096, 0xDEADBEEF}, bytes);
  const uint8_t expected[] = {0xFE, 0x5A, 0x04, 0x03, 0x02, 0x01, 0x00, 0x10, 0xEF, 0xBE, 0xAD, 0xDE};
  EXPECT_EQ(std::memcmp(bytes, expected, sizeof(expected)), 0);

  serialxfer::FrameHeader header;
  ASSERT_TRUE(serialxfer::decodeFrameHeader(bytes, header));
  EXPECT_EQ(header.offset, 0x01020304u);
  EXPECT_EQ(header.length, 4096u);
  EXPECT_EQ(header.crc, 0xDEADBEEFu);
}

TEST(SerialTransfer, CancelFrameDecodes) {
  uint8_t bytes[serialxfer::FRAME_HEADER_SIZE];
  serialxfer::encodeFrameHeader({serialxfer::CANCEL_OFFSET, 0, 0}, bytes);
  serialxfer::FrameHeader header;
  ASSERT_TRUE(serialxfer::decodeFrameHeader(bytes, header));
  EXPECT_EQ(header.offset, serialxfer::CANCEL_OFFSET);
  EXPECT_EQ(header.length, 0u);
}

TEST(SerialTransfer, LogTextAndOversizedFramesAreNotHeaders) {
  serialxfer::FrameHeader header;
  const uint8_t log[12] = {'[', '1', '2', ']', ' ', '[', 'I', 'N', 'F', ']', ' ', 'x'};
  EXPECT_FALSE(serialxfer::decodeFrameHeader(log, header));

  uint8_t bytes[serialxfer::FRAME_HEADER_SIZE];
  serialxfer::encodeFrameHeader({0, 4097, 0}, bytes);
  EXPECT_FALSE(serialxfer::decodeFrameHeader(bytes, header));
}

// A frame as the host tool builds it.
std::vector<uint8_t> makeFrame(uint32_t offset, const std::vector<uint8_t>& payload) {
  std::vector<uint8_t> frame(serialxfer::FRAME_HEADER_SIZE);
  serialxfer::encodeFrameHeader({offset, static_cast<uint16_t>(payload.size()), 0}, frame.data());
  uint32_t crc = crc32(0, serialxfer::crcCoveredHeader(frame.data()), serialxfer::CRC_COVERED_HEADER_SIZE);
  crc = crc32(crc, payload.data(), payload.size());
  serialxfer::encodeFrameHeader({offset, static_cast<uint16_t>(payload.size()), crc}, frame.data());
  frame.insert(frame.end(), payload.begin(), payload.end());
  return frame;
}

const char* receive(const std::vector<uint8_t>& frame, uint32_t expectedOffset, uint32_t totalSize) {
  serialxfer::FrameHeader header;
  if (!serialxfer::decodeFrameHeader(frame.data(), header)) return "header";
  const uint8_t* payload = frame.data() + serialxfer::FRAME_HEADER_SIZE;
  uint32_t crc = crc32(0, serialxfer::crcCoveredHeader(frame.data()), serialxfer::CRC_COVERED_HEADER_SIZE);
  crc = crc32(crc, payload, header.length);
  return serialxfer::checkFrame(header, crc, expectedOffset, totalSize);
}

TEST(SerialTransfer, ReceiverAcceptsOnlyTheNextIntactFrame) {
  const std::vector<uint8_t> payload(100, 0x42);
  EXPECT_EQ(receive(makeFrame(0, payload), 0, 150), nullptr);
  EXPECT_STREQ(receive(makeFrame(100, payload), 0, 150), "offset");    // an earlier frame was lost
  EXPECT_STREQ(receive(makeFrame(0, payload), 100, 150), "offset");    // a resend of a written frame
  EXPECT_STREQ(receive(makeFrame(100, payload), 100, 150), "length");  // runs past the end

  auto damaged = makeFrame(0, payload);
  damaged[serialxfer::FRAME_HEADER_SIZE + 7] ^= 0x01;
  EXPECT_STREQ(receive(damaged, 0, 150), "crc");
}

TEST(SerialTransfer, CrcBindsTheFrameToItsOffset) {
  // Same payload and CRC, header rewritten to another offset: refused.
  auto frame = makeFrame(4096, std::vector<uint8_t>(16, 7));
  frame[2] = 0;
  frame[3] = 0x20;  // offset 8192
  EXPECT_STREQ(receive(frame, 8192, 10000), "crc");
}

TEST(SerialTransfer, FontMagic) {
  const uint8_t cpfont[8] = {'C', 'P', 'F', 'O', 'N', 'T', 0, 0};
  const uint8_t ttf[8] = {0, 1, 0, 0, 0, 0x10, 0, 0x80};
  const uint8_t otf[8] = {'O', 'T', 'T', 'O', 0, 0, 0, 0};
  EXPECT_TRUE(serialxfer::fontMagicOk(cpfont, 8, false));
  EXPECT_FALSE(serialxfer::fontMagicOk(cpfont, 7, false));
  EXPECT_FALSE(serialxfer::fontMagicOk(ttf, 8, false));
  EXPECT_TRUE(serialxfer::fontMagicOk(ttf, 8, true));
  EXPECT_TRUE(serialxfer::fontMagicOk(otf, 8, true));
  EXPECT_FALSE(serialxfer::fontMagicOk(cpfont, 8, true));
}

PathKind kind(const char* path, Access access) { return serialxfer::checkPath(path, access).kind; }

TEST(SerialTransfer, ReadsSeeTheWholeCardButTheCredentialStores) {
  EXPECT_EQ(kind("/", Access::Read), PathKind::File);
  EXPECT_EQ(kind("/Books/A Book.epub", Access::Read), PathKind::File);
  EXPECT_EQ(kind("/.crosspoint/settings.json", Access::Read), PathKind::File);
  EXPECT_EQ(kind("/.crosspoint/wifi.json", Access::Read), PathKind::Invalid);
  EXPECT_EQ(kind("/CROSSP~1/wifi.json", Access::Read), PathKind::Invalid);
  EXPECT_EQ(kind("/Books/../.crosspoint/wifi.json", Access::Read), PathKind::Invalid);
  EXPECT_EQ(kind("/Books/", Access::Read), PathKind::Invalid);
  EXPECT_EQ(kind("Books", Access::Read), PathKind::Invalid);
}

TEST(SerialTransfer, WritesStayInTheVisibleCard) {
  EXPECT_EQ(kind("/Books/A Book.epub", Access::Write), PathKind::File);
  EXPECT_EQ(kind("/notes.txt", Access::Write), PathKind::File);
  EXPECT_EQ(kind("/Books/Ünïcödé 書.epub", Access::Write), PathKind::File);

  EXPECT_EQ(kind("/", Access::Write), PathKind::Invalid);
  EXPECT_EQ(kind("/.crosspoint/settings.json", Access::Write), PathKind::Invalid);
  EXPECT_EQ(kind("/Books/.hidden/a.epub", Access::Write), PathKind::Invalid);
  EXPECT_EQ(kind("/Books/../.crosspoint/x", Access::Write), PathKind::Invalid);
  EXPECT_EQ(kind("/Books/./a.epub", Access::Write), PathKind::Invalid);
  EXPECT_EQ(kind("/Books//a.epub", Access::Write), PathKind::Invalid);
  EXPECT_EQ(kind("/Books/", Access::Write), PathKind::Invalid);
  EXPECT_EQ(kind("/System Volume Information/x", Access::Write), PathKind::Invalid);
  EXPECT_EQ(kind("/books/xtcache", Access::Write), PathKind::Invalid);
}

TEST(SerialTransfer, NamesSdFatWouldOpenAsAnotherNameAreRefused) {
  // 8.3 aliases reach hidden folders without a leading dot.
  EXPECT_EQ(kind("/CROSSP~1/settings.json", Access::Write), PathKind::Invalid);
  EXPECT_STREQ(serialxfer::checkPath("/CROSSP~1/x", Access::Write).error, "8.3 short names are not accepted");
  EXPECT_EQ(kind("/FONTS~1/x.cpfont", Access::Write), PathKind::Invalid);
  // SdFat skips leading spaces and drops trailing dots and spaces.
  EXPECT_EQ(kind("/ .crosspoint/x", Access::Write), PathKind::Invalid);
  EXPECT_EQ(kind("/.crosspoint./x", Access::Write), PathKind::Invalid);
  EXPECT_EQ(kind("/Books /a.epub", Access::Write), PathKind::Invalid);
  EXPECT_EQ(kind("/a.epub.", Access::Write), PathKind::Invalid);
  EXPECT_EQ(kind("/a:b.epub", Access::Write), PathKind::Invalid);
  EXPECT_EQ(kind("/a\\b.epub", Access::Write), PathKind::Invalid);
  EXPECT_EQ(kind("/a\tb.epub", Access::Write), PathKind::Invalid);
  // A tilde alone is an ordinary name.
  EXPECT_EQ(kind("/Books/a~b.epub", Access::Write), PathKind::File);
}

TEST(SerialTransfer, LongPathsAreRefused) {
  const std::string longPath = "/" + std::string(serialxfer::MAX_PATH_LENGTH, 'a');
  EXPECT_EQ(kind(longPath.c_str(), Access::Write), PathKind::Invalid);
  EXPECT_EQ(kind(longPath.c_str(), Access::Read), PathKind::Invalid);
}

TEST(SerialTransfer, FontsGoThroughTheFontsFolders) {
  auto check = serialxfer::checkPath("/.fonts/Noto-Serif/NotoSerif_14.cpfont", Access::Write);
  EXPECT_EQ(check.kind, PathKind::FontFile);
  EXPECT_EQ(check.family, "Noto-Serif");
  EXPECT_EQ(check.fileName, "NotoSerif_14.cpfont");
  EXPECT_EQ(kind("/fonts/Noto/Noto_14.cpfont", Access::Write), PathKind::FontFile);
  EXPECT_EQ(kind("/.FONTS/Noto/Noto_14.cpfont", Access::Write), PathKind::FontFile);

  EXPECT_EQ(kind("/.fonts/Noto", Access::Write), PathKind::Invalid);
  EXPECT_EQ(kind("/.fonts/Noto/sub/x.cpfont", Access::Write), PathKind::Invalid);
  EXPECT_EQ(kind("/.fonts", Access::Remove), PathKind::Invalid);

  check = serialxfer::checkPath("/.fonts/Noto", Access::Remove);
  EXPECT_EQ(check.kind, PathKind::FontFamily);
  EXPECT_EQ(check.family, "Noto");
  EXPECT_EQ(kind("/.fonts/Noto/Noto_14.cpfont", Access::Remove), PathKind::FontFile);
}

TEST(SerialTransfer, RemovalReachesHiddenItemsButNotCredentials) {
  EXPECT_EQ(kind("/Books/._A Book.epub", Access::Remove), PathKind::File);
  EXPECT_EQ(kind("/.crosspoint/settings.json", Access::Remove), PathKind::File);
  EXPECT_EQ(kind("/.crosspoint/opds.json", Access::Remove), PathKind::Invalid);
  EXPECT_EQ(kind("/.crosspoint/WIFI.JSON", Access::Remove), PathKind::Invalid);
  EXPECT_EQ(kind("/System Volume Information", Access::Remove), PathKind::Invalid);
  EXPECT_STREQ(serialxfer::checkPath("/", Access::Remove).error, "not the root folder");
}

}  // namespace
