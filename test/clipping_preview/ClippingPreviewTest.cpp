#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <string>

#include "clippings/ClippingPreview.h"

namespace {
struct Reader {
  std::string text;
  size_t pos = 0;
  size_t maxRequest = 0;
  int read(char* out, size_t size) {
    maxRequest = std::max(maxRequest, size);
    const size_t count = std::min(size, text.size() - pos);
    std::memcpy(out, text.data() + pos, count);
    pos += count;
    return static_cast<int>(count);
  }
};
constexpr const char* ELLIPSIS = "\xe2\x80\xa6";
}  // namespace

TEST(ClippingPreview, ShortTextNormalizesWithoutEllipsis) {
  Reader reader{" \t\r\nhello\r\nworld\t "};
  std::string out;
  ASSERT_TRUE(clippingPreview::read(reader, reader.text.size(), out));
  EXPECT_EQ(out, "hello world");
}

TEST(ClippingPreview, LongTextUsesBoundedReadsAndMarksOmission) {
  Reader reader{std::string(4096, 'x')};
  std::string out;
  ASSERT_TRUE(clippingPreview::read(reader, reader.text.size(), out));
  EXPECT_EQ(out, std::string(256, 'x') + ELLIPSIS);
  EXPECT_LE(reader.pos, 320u);
  EXPECT_LE(reader.maxRequest, 64u);
}

TEST(ClippingPreview, ExactLimitDoesNotClaimOmission) {
  Reader reader{std::string(256, 'x')};
  std::string out;
  ASSERT_TRUE(clippingPreview::read(reader, reader.text.size(), out));
  EXPECT_EQ(out, reader.text);
}

TEST(ClippingPreview, Utf8CodepointsAreNeverSplitAtLimit) {
  for (const auto& codepoint : {std::string("é"), std::string("中"), std::string("😀")}) {
    for (size_t space = 0; space < codepoint.size(); ++space) {
      const std::string prefix(256 - space, 'a');
      Reader reader{prefix + codepoint + "tail"};
      std::string out;
      ASSERT_TRUE(clippingPreview::read(reader, reader.text.size(), out));
      EXPECT_EQ(out, prefix + ELLIPSIS);
    }
  }
}

TEST(ClippingPreview, Utf8CrossesReadChunkBoundary) {
  Reader reader{std::string(63, 'a') + "😀" + "é中"};
  std::string out;
  ASSERT_TRUE(clippingPreview::read(reader, reader.text.size(), out));
  EXPECT_EQ(out, reader.text);
}

TEST(ClippingPreview, LeadingWhitespaceDoesNotConsumePreviewBudget) {
  Reader reader{std::string(600, ' ') + "\xc2\xa0\xe2\x80\x83\xe2\x80\xaf" + std::string(300, 'x')};
  std::string out;
  ASSERT_TRUE(clippingPreview::read(reader, reader.text.size(), out));
  EXPECT_EQ(out, std::string(256, 'x') + ELLIPSIS);
  EXPECT_LE(reader.maxRequest, 64u);
}

TEST(ClippingPreview, UnicodeWhitespaceCollapsesAndTrailingWhitespaceIsDropped) {
  Reader reader{"hello\xc2\xa0\xe2\x80\x83\xe2\x80\xafworld \t"};
  std::string out;
  ASSERT_TRUE(clippingPreview::read(reader, reader.text.size(), out));
  EXPECT_EQ(out, "hello world");
}

TEST(ClippingPreview, AllWhitespaceAndEmptyTextStayEmpty) {
  for (const auto& text : {std::string(), std::string(4096, ' ')}) {
    Reader reader{text};
    std::string out = "stale";
    ASSERT_TRUE(clippingPreview::read(reader, text.size(), out));
    EXPECT_TRUE(out.empty());
    EXPECT_EQ(reader.pos, text.size());
  }
}

TEST(ClippingPreview, DoesNotReadFollowingRecord) {
  Reader reader{"oneNEXT_RECORD"};
  std::string out;
  ASSERT_TRUE(clippingPreview::read(reader, 3, out));
  EXPECT_EQ(out, "one");
  EXPECT_EQ(reader.pos, 3u);
}

TEST(ClippingPreview, ShortReadClearsOldAndPartialOutput) {
  Reader reader{std::string(70, 'a')};
  std::string out = "stale";
  EXPECT_FALSE(clippingPreview::read(reader, 128, out));
  EXPECT_TRUE(out.empty());
}

TEST(ClippingPreview, InvalidOrIncompleteUtf8FailsWithoutPartialOutput) {
  for (const auto& text : {std::string("a\xff"), std::string("a\xc2"), std::string("a\xc2x")}) {
    Reader reader{text};
    std::string out;
    EXPECT_FALSE(clippingPreview::read(reader, text.size(), out));
    EXPECT_TRUE(out.empty());
  }
}

TEST(ClippingPreview, RepeatedReadsReuseReservedStorage) {
  Reader reader{std::string(4096, 'x')};
  std::string out;
  out.reserve(clippingPreview::MAX_BYTES + clippingPreview::ELLIPSIS_BYTES);
  const char* buffer = out.data();
  for (int i = 0; i < 3; ++i) {
    reader.pos = 0;
    ASSERT_TRUE(clippingPreview::read(reader, reader.text.size(), out));
    EXPECT_EQ(out.data(), buffer);
  }
}

TEST(ClippingPreview, InternalWhitespaceStillCountsAgainstReadBudget) {
  Reader reader{"hello" + std::string(4000, ' ') + "world"};
  std::string out;
  ASSERT_TRUE(clippingPreview::read(reader, reader.text.size(), out));
  EXPECT_EQ(out, std::string("hello") + ELLIPSIS);
  EXPECT_LE(reader.pos, 320u);
}
