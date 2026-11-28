#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "util/IdxCursor.h"

namespace {

// Memory-backed stand-in for HalFile: the cursor only ever calls read().
class MemFile {
 public:
  explicit MemFile(std::vector<uint8_t> bytes) : data(std::move(bytes)) {}

  int read(void* dest, size_t count) {
    if (pos >= data.size()) return 0;
    const size_t take = std::min(count, data.size() - pos);
    memcpy(dest, data.data() + pos, take);
    pos += take;
    reads++;
    return static_cast<int>(take);
  }

  void seekSet(size_t offset) { pos = offset; }

  int reads = 0;

 private:
  std::vector<uint8_t> data;
  size_t pos = 0;
};

// One .idx entry: headword, NUL, then an 8-byte big-endian offset/size pair.
void appendEntry(std::vector<uint8_t>& out, const std::string& word, uint32_t offset, uint32_t size) {
  out.insert(out.end(), word.begin(), word.end());
  out.push_back(0);
  for (int i = 3; i >= 0; i--) out.push_back(static_cast<uint8_t>(offset >> (i * 8)));
  for (int i = 3; i >= 0; i--) out.push_back(static_cast<uint8_t>(size >> (i * 8)));
}

uint32_t readBe32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

TEST(IdxCursorTest, WalksEntriesAndTracksUnbufferedPosition) {
  std::vector<uint8_t> bytes;
  appendEntry(bytes, "alpha", 10, 20);
  appendEntry(bytes, "beta", 30, 40);

  MemFile file(bytes);
  IdxCursor<MemFile> cursor(file, 0);

  char word[256];
  uint8_t suffix[8];

  ASSERT_EQ(cursor.readWord(word, sizeof(word)), 5);
  EXPECT_STREQ(word, "alpha");
  ASSERT_TRUE(cursor.readBytes(suffix, 8));
  EXPECT_EQ(readBe32(suffix), 10u);
  EXPECT_EQ(readBe32(suffix + 4), 20u);
  // 5 chars + NUL + 8 suffix bytes: the cursor reports the offset an unbuffered
  // reader would be at, not how far the window read ahead.
  EXPECT_EQ(cursor.position(), 14u);

  ASSERT_EQ(cursor.readWord(word, sizeof(word)), 4);
  EXPECT_STREQ(word, "beta");
  ASSERT_TRUE(cursor.readBytes(suffix, 8));
  EXPECT_EQ(readBe32(suffix), 30u);
  EXPECT_EQ(cursor.position(), bytes.size());

  // Whole file consumed in a single window fill.
  EXPECT_EQ(file.reads, 1);
}

TEST(IdxCursorTest, StartsFromAMidFileSeekPoint) {
  std::vector<uint8_t> bytes;
  appendEntry(bytes, "alpha", 1, 2);
  const uint32_t second = static_cast<uint32_t>(bytes.size());
  appendEntry(bytes, "beta", 3, 4);

  MemFile file(bytes);
  file.seekSet(second);
  IdxCursor<MemFile> cursor(file, second);

  char word[256];
  ASSERT_EQ(cursor.readWord(word, sizeof(word)), 4);
  EXPECT_STREQ(word, "beta");
  EXPECT_EQ(cursor.position(), second + 5);
}

TEST(IdxCursorTest, ReadsWordsAndSuffixesAcrossWindowBoundaries) {
  // Enough entries to span several 256-byte windows, with word lengths chosen
  // so words and 8-byte suffixes straddle the boundaries at varying offsets.
  std::vector<uint8_t> bytes;
  std::vector<std::string> words;
  for (int i = 0; i < 200; i++) {
    std::string w = "word" + std::to_string(i) + std::string(i % 17, 'x');
    words.push_back(w);
    appendEntry(bytes, w, static_cast<uint32_t>(i), static_cast<uint32_t>(i * 2));
  }

  MemFile file(bytes);
  IdxCursor<MemFile> cursor(file, 0);

  char word[256];
  uint8_t suffix[8];
  uint32_t expectedPos = 0;
  for (size_t i = 0; i < words.size(); i++) {
    ASSERT_EQ(cursor.readWord(word, sizeof(word)), static_cast<int>(words[i].size())) << "entry " << i;
    EXPECT_EQ(word, words[i]) << "entry " << i;
    ASSERT_TRUE(cursor.readBytes(suffix, 8)) << "entry " << i;
    EXPECT_EQ(readBe32(suffix), static_cast<uint32_t>(i));
    EXPECT_EQ(readBe32(suffix + 4), static_cast<uint32_t>(i * 2));
    expectedPos += static_cast<uint32_t>(words[i].size()) + 1 + 8;
    EXPECT_EQ(cursor.position(), expectedPos) << "entry " << i;
  }
  EXPECT_GT(file.reads, 1);
}

TEST(IdxCursorTest, TruncatesOverlongWordsButStaysEntryAligned) {
  std::vector<uint8_t> bytes;
  appendEntry(bytes, std::string(40, 'a'), 7, 8);
  appendEntry(bytes, "next", 9, 10);

  MemFile file(bytes);
  IdxCursor<MemFile> cursor(file, 0);

  char small[8];
  // Returns the truncated length, and the caller's buffer stays NUL-terminated.
  EXPECT_EQ(cursor.readWord(small, sizeof(small)), 7);
  EXPECT_STREQ(small, "aaaaaaa");

  // The skipped tail was still consumed, so the suffix and next entry line up.
  uint8_t suffix[8];
  ASSERT_TRUE(cursor.readBytes(suffix, 8));
  EXPECT_EQ(readBe32(suffix), 7u);

  char word[256];
  ASSERT_EQ(cursor.readWord(word, sizeof(word)), 4);
  EXPECT_STREQ(word, "next");
}

TEST(IdxCursorTest, ReportsEofRatherThanReturningPartialData) {
  std::vector<uint8_t> bytes;
  appendEntry(bytes, "only", 1, 2);
  // Truncated trailer: a headword with no NUL and no suffix.
  const std::string tail = "trunc";
  bytes.insert(bytes.end(), tail.begin(), tail.end());

  MemFile file(bytes);
  IdxCursor<MemFile> cursor(file, 0);

  char word[256];
  uint8_t suffix[8];
  ASSERT_EQ(cursor.readWord(word, sizeof(word)), 4);
  ASSERT_TRUE(cursor.readBytes(suffix, 8));

  // Unterminated headword at EOF.
  EXPECT_EQ(cursor.readWord(word, sizeof(word)), -1);
  // And a short suffix read fails rather than handing back junk.
  EXPECT_FALSE(cursor.readBytes(suffix, 8));
}

TEST(IdxCursorTest, HandlesEmptyHeadwords) {
  std::vector<uint8_t> bytes;
  appendEntry(bytes, "", 5, 6);
  appendEntry(bytes, "after", 7, 8);

  MemFile file(bytes);
  IdxCursor<MemFile> cursor(file, 0);

  char word[256];
  uint8_t suffix[8];
  EXPECT_EQ(cursor.readWord(word, sizeof(word)), 0);
  EXPECT_STREQ(word, "");
  ASSERT_TRUE(cursor.readBytes(suffix, 8));
  EXPECT_EQ(readBe32(suffix), 5u);
  EXPECT_EQ(cursor.readWord(word, sizeof(word)), 5);
  EXPECT_STREQ(word, "after");
}

}  // namespace
