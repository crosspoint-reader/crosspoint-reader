// FontBytes: big-endian reads, every one bounds-checked against the file; in SD mode, the outlines read from the
// card through the page cache.
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "FontBytes.h"

namespace {
const uint8_t kData[] = {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC};
}

TEST(FontBytes, ReadsBigEndian) {
  FontBytes bytes(kData, sizeof kData);
  EXPECT_EQ(0x12, bytes.u8(0));
  EXPECT_EQ(0x3456, bytes.u16(1));
  EXPECT_EQ(-0x6544, bytes.i16(4));  // 0x9ABC
  EXPECT_EQ(0x3456789Au, bytes.u32(1));
  EXPECT_FALSE(bytes.outOfRange());
  EXPECT_FALSE(bytes.ioError());
}

TEST(FontBytes, TheLastBytesAreInRangeAndOneMoreIsNot) {
  FontBytes bytes(kData, sizeof kData);
  EXPECT_EQ(0xBC, bytes.u8(5));
  EXPECT_EQ(0x9ABC, bytes.u16(4));
  EXPECT_EQ(0x56789ABCu, bytes.u32(2));
  EXPECT_FALSE(bytes.outOfRange());
  EXPECT_EQ(0u, bytes.u32(3));
  EXPECT_TRUE(bytes.outOfRange());
}

TEST(FontBytes, EveryKindOfReadPastTheEndReturnsZeroAndSetsTheFlag) {
  for (int kind = 0; kind < 4; ++kind) {
    FontBytes bytes(kData, sizeof kData);
    switch (kind) {
      case 0:
        EXPECT_EQ(0, bytes.u8(6));
        break;
      case 1:
        EXPECT_EQ(0, bytes.u16(5));
        break;
      case 2:
        EXPECT_EQ(0, bytes.i16(0xFFFFFFFFu));  // the offset wraps: still refused
        break;
      default:
        EXPECT_EQ(0u, bytes.u32(0xFFFFFFFEu));
    }
    EXPECT_TRUE(bytes.outOfRange()) << kind;
  }
}

TEST(FontBytes, ReadCopiesAllOrNothing) {
  FontBytes bytes(kData, sizeof kData);
  uint8_t out[4] = {0xEE, 0xEE, 0xEE, 0xEE};
  ASSERT_TRUE(bytes.read(2, 4, out));
  EXPECT_EQ(0x56, out[0]);
  EXPECT_EQ(0xBC, out[3]);
  uint8_t untouched[4] = {0xEE, 0xEE, 0xEE, 0xEE};
  EXPECT_FALSE(bytes.read(3, 4, untouched));
  EXPECT_EQ(0xEE, untouched[0]);
  EXPECT_TRUE(bytes.outOfRange());
  EXPECT_TRUE(bytes.read(6, 0, untouched));  // nothing, at the very end
}

TEST(FontBytes, ClearErrorsForgetsTheFlag) {
  FontBytes bytes(kData, sizeof kData);
  bytes.u8(99);
  ASSERT_TRUE(bytes.outOfRange());
  bytes.clearErrors();
  EXPECT_FALSE(bytes.outOfRange());
  EXPECT_EQ(0x12, bytes.u8(0));
}

TEST(FontBytes, AnEmptySourceReadsNothing) {
  FontBytes bytes;
  EXPECT_EQ(0u, bytes.size());
  EXPECT_EQ(0, bytes.u8(0));
  EXPECT_TRUE(bytes.outOfRange());
}

// --- SD mode: the mapped tables in place, the outlines read from the card through the page cache ---

namespace {

/// A file on the "card": byte i is i * 7 + 3, every read recorded, a failure on demand.
struct CardFile {
  std::vector<uint8_t> bytes;
  std::vector<uint32_t> reads;  // the offset of each read
  bool fail = false;

  explicit CardFile(const uint32_t size) : bytes(size) {
    for (uint32_t i = 0; i < size; ++i) bytes[i] = static_cast<uint8_t>(i * 7 + 3);
  }
  ttf::FileSource source() { return {static_cast<uint32_t>(bytes.size()), &CardFile::read, this}; }
  static bool read(void* ctx, const uint32_t offset, void* dst, const uint32_t len) {
    auto* file = static_cast<CardFile*>(ctx);
    if (file->fail || offset > file->bytes.size() || len > file->bytes.size() - offset) return false;
    file->reads.push_back(offset);
    std::memcpy(dst, file->bytes.data() + offset, len);
    return true;
  }
  uint8_t at(const uint32_t i) const { return bytes[i]; }
};

constexpr uint32_t kOutlines = 100;    // where the font has its outlines, past the six mapped bytes
constexpr uint32_t kFileOffset = 700;  // where they are in the file
constexpr uint32_t kLength = 3000;

}  // namespace

TEST(FontBytesSd, TablesReadInPlaceAndOutlinesFromTheCard) {
  CardFile file(4096);
  ttf::SdPageCache cache;
  const ttf::SdRange sd{file.source(), kOutlines, kFileOffset, kLength, &cache};
  FontBytes bytes(kData, sizeof kData, &sd);
  EXPECT_EQ(kOutlines + kLength, bytes.size());
  EXPECT_EQ(0x3456, bytes.u16(1));  // the mapped tables
  EXPECT_TRUE(file.reads.empty());
  EXPECT_EQ(file.at(kFileOffset + 5) << 8 | file.at(kFileOffset + 6), bytes.u16(kOutlines + 5));
  EXPECT_EQ(1u, file.reads.size());
  EXPECT_EQ(nullptr, bytes.span(kOutlines, 2));  // never in place
  EXPECT_FALSE(bytes.outOfRange());
  EXPECT_FALSE(bytes.ioError());
}

TEST(FontBytesSd, BetweenTheTablesAndTheOutlinesAndPastThemIsOutOfRange) {
  CardFile file(4096);
  ttf::SdPageCache cache;
  const ttf::SdRange sd{file.source(), kOutlines, kFileOffset, kLength, &cache};
  for (const uint32_t offset : {uint32_t{6}, kOutlines - 1, kOutlines + kLength - 1, kOutlines + kLength}) {
    FontBytes bytes(kData, sizeof kData, &sd);
    EXPECT_EQ(0u, bytes.u16(offset)) << offset;
    EXPECT_TRUE(bytes.outOfRange()) << offset;
    EXPECT_FALSE(bytes.ioError()) << offset;
  }
  EXPECT_TRUE(file.reads.empty());  // past the range is never asked of the card, though the file goes on
}

TEST(FontBytesSd, AReadAcrossPagesIsWholeAndARepeatComesFromTheCache) {
  CardFile file(4096);
  ttf::SdPageCache cache;
  const ttf::SdRange sd{file.source(), kOutlines, kFileOffset, kLength, &cache};
  FontBytes bytes(kData, sizeof kData, &sd);
  // File offset 1022 to 1025: the last two bytes of page 1 and the first two of page 2.
  const uint32_t at = kOutlines + (1022 - kFileOffset);
  const uint32_t want = static_cast<uint32_t>(file.at(1022)) << 24 | static_cast<uint32_t>(file.at(1023)) << 16 |
                        static_cast<uint32_t>(file.at(1024)) << 8 | file.at(1025);
  EXPECT_EQ(want, bytes.u32(at));
  EXPECT_EQ((std::vector<uint32_t>{512, 1024}), file.reads);  // whole pages, on 512-byte offsets
  EXPECT_EQ(want, bytes.u32(at));
  uint8_t copy[600];
  ASSERT_TRUE(bytes.read(kOutlines + (1000 - kFileOffset), sizeof copy, copy));  // those two pages and the next
  EXPECT_EQ(file.at(1000), copy[0]);
  EXPECT_EQ(file.at(1599), copy[599]);
  EXPECT_EQ(3u, file.reads.size());
  EXPECT_EQ(3u, cache.misses());
}

TEST(FontBytesSd, EightPagesAreKeptAndTheLeastRecentlyUsedGoesFirst) {
  CardFile file(16384);
  ttf::SdPageCache cache;
  const ttf::SdRange sd{file.source(), 0, 0, 16384, &cache};
  FontBytes bytes(nullptr, 0, &sd);
  for (uint32_t page = 0; page < ttf::SdPageCache::kPages; ++page) bytes.u8(page * 512);
  bytes.u8(0);  // page 0 is the most recent now, page 1 the least
  EXPECT_EQ(8u, cache.misses());
  bytes.u8(8 * 512);  // a ninth page takes page 1's place
  EXPECT_EQ(9u, cache.misses());
  bytes.u8(0);
  bytes.u8(2 * 512);
  EXPECT_EQ(9u, cache.misses());
  bytes.u8(1 * 512);
  EXPECT_EQ(10u, cache.misses());
  EXPECT_FALSE(bytes.outOfRange());
}

TEST(FontBytesSd, AFailedCardReadIsAnIoErrorAndLeavesNothingBehind) {
  CardFile file(4096);
  ttf::SdPageCache cache;
  const ttf::SdRange sd{file.source(), kOutlines, kFileOffset, kLength, &cache};
  FontBytes bytes(kData, sizeof kData, &sd);
  file.fail = true;
  EXPECT_EQ(0, bytes.u8(kOutlines));
  EXPECT_TRUE(bytes.ioError());
  EXPECT_FALSE(bytes.outOfRange());
  bytes.clearErrors();
  file.fail = false;
  EXPECT_EQ(file.at(kFileOffset), bytes.u8(kOutlines));  // read again, not served from a page that failed
  EXPECT_FALSE(bytes.ioError());

  const ttf::SdRange noCache{file.source(), kOutlines, kFileOffset, kLength, nullptr};
  FontBytes uncached(kData, sizeof kData, &noCache);
  EXPECT_EQ(0, uncached.u8(kOutlines));
  EXPECT_TRUE(uncached.ioError());
}

TEST(FontBytesSd, TwoFilesKeepTheirOwnPages) {
  CardFile a(2048);
  CardFile b(2048);
  for (uint8_t& byte : b.bytes) byte = static_cast<uint8_t>(~byte);
  ttf::SdPageCache cache;
  const ttf::SdRange sdA{a.source(), 0, 0, 2048, &cache};
  const ttf::SdRange sdB{b.source(), 0, 0, 2048, &cache};
  FontBytes bytesA(nullptr, 0, &sdA);
  FontBytes bytesB(nullptr, 0, &sdB);
  EXPECT_EQ(a.at(10), bytesA.u8(10));
  EXPECT_EQ(b.at(10), bytesB.u8(10));
  EXPECT_EQ(a.at(11), bytesA.u8(11));
  EXPECT_EQ(2u, cache.misses());
}
