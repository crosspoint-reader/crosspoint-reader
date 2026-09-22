#include <gtest/gtest.h>

#include <algorithm>
#include <array>
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
struct PreviewOutput {
  std::array<char, clippingPreview::BUFFER_BYTES> buffer{};
  size_t length = 0;

  std::string text() const { return std::string(buffer.data(), length); }
};

bool readPreview(Reader& reader, const size_t remaining, PreviewOutput& out) {
  return clippingPreview::read(reader, remaining, out.buffer.data(), out.buffer.size(), out.length);
}
constexpr const char* ELLIPSIS = "\xe2\x80\xa6";
}  // namespace

TEST(ClippingPreview, ShortTextNormalizesWithoutEllipsis) {
  Reader reader{" \t\r\nhello\r\nworld\t "};
  PreviewOutput out;
  ASSERT_TRUE(readPreview(reader, reader.text.size(), out));
  EXPECT_EQ(out.text(), "hello world");
}

TEST(ClippingPreview, LongTextUsesBoundedReadsAndMarksOmission) {
  Reader reader{std::string(4096, 'x')};
  PreviewOutput out;
  ASSERT_TRUE(readPreview(reader, reader.text.size(), out));
  EXPECT_EQ(out.text(), std::string(256, 'x') + ELLIPSIS);
  EXPECT_LE(reader.pos, 320u);
  EXPECT_LE(reader.maxRequest, 64u);
}

TEST(ClippingPreview, ExactLimitDoesNotClaimOmission) {
  Reader reader{std::string(256, 'x')};
  PreviewOutput out;
  ASSERT_TRUE(readPreview(reader, reader.text.size(), out));
  EXPECT_EQ(out.text(), reader.text);
}

TEST(ClippingPreview, Utf8CodepointsAreNeverSplitAtLimit) {
  for (const auto& codepoint : {std::string("é"), std::string("中"), std::string("😀")}) {
    for (size_t space = 0; space < codepoint.size(); ++space) {
      const std::string prefix(256 - space, 'a');
      Reader reader{prefix + codepoint + "tail"};
      PreviewOutput out;
      ASSERT_TRUE(readPreview(reader, reader.text.size(), out));
      EXPECT_EQ(out.text(), prefix + ELLIPSIS);
    }
  }
}

TEST(ClippingPreview, Utf8CrossesReadChunkBoundary) {
  Reader reader{std::string(63, 'a') + "😀" + "é中"};
  PreviewOutput out;
  ASSERT_TRUE(readPreview(reader, reader.text.size(), out));
  EXPECT_EQ(out.text(), reader.text);
}

TEST(ClippingPreview, LeadingWhitespaceDoesNotConsumePreviewBudget) {
  Reader reader{std::string(600, ' ') + "\xc2\xa0\xe2\x80\x83\xe2\x80\xaf" + std::string(300, 'x')};
  PreviewOutput out;
  ASSERT_TRUE(readPreview(reader, reader.text.size(), out));
  EXPECT_EQ(out.text(), std::string(256, 'x') + ELLIPSIS);
  EXPECT_LE(reader.maxRequest, 64u);
}

TEST(ClippingPreview, UnicodeWhitespaceCollapsesAndTrailingWhitespaceIsDropped) {
  Reader reader{"hello\xc2\xa0\xe2\x80\x83\xe2\x80\xafworld \t"};
  PreviewOutput out;
  ASSERT_TRUE(readPreview(reader, reader.text.size(), out));
  EXPECT_EQ(out.text(), "hello world");
}

TEST(ClippingPreview, AllWhitespaceAndEmptyTextStayEmpty) {
  for (const auto& text : {std::string(), std::string(4096, ' ')}) {
    Reader reader{text};
    PreviewOutput out;
    std::memcpy(out.buffer.data(), "stale", 6);
    out.length = 5;
    ASSERT_TRUE(readPreview(reader, text.size(), out));
    EXPECT_EQ(out.length, 0u);
    EXPECT_EQ(out.buffer[0], '\0');
    EXPECT_EQ(reader.pos, text.size());
  }
}

TEST(ClippingPreview, DoesNotReadFollowingRecord) {
  Reader reader{"oneNEXT_RECORD"};
  PreviewOutput out;
  ASSERT_TRUE(readPreview(reader, 3, out));
  EXPECT_EQ(out.text(), "one");
  EXPECT_EQ(reader.pos, 3u);
}

TEST(ClippingPreview, ShortReadClearsOldAndPartialOutput) {
  Reader reader{std::string(70, 'a')};
  PreviewOutput out;
  std::memcpy(out.buffer.data(), "stale", 6);
  out.length = 5;
  EXPECT_FALSE(readPreview(reader, 128, out));
  EXPECT_EQ(out.length, 0u);
  EXPECT_EQ(out.buffer[0], '\0');
}

TEST(ClippingPreview, InvalidOrIncompleteUtf8FailsWithoutPartialOutput) {
  for (const auto& text : {std::string("a\xff"), std::string("a\xc2"), std::string("a\xc2x")}) {
    Reader reader{text};
    PreviewOutput out;
    EXPECT_FALSE(readPreview(reader, text.size(), out));
    EXPECT_EQ(out.length, 0u);
    EXPECT_EQ(out.buffer[0], '\0');
  }
}

TEST(ClippingPreview, RepeatedReadsReuseFixedStorage) {
  Reader reader{std::string(4096, 'x')};
  PreviewOutput out;
  const char* buffer = out.buffer.data();
  for (int i = 0; i < 3; ++i) {
    reader.pos = 0;
    ASSERT_TRUE(readPreview(reader, reader.text.size(), out));
    EXPECT_EQ(out.buffer.data(), buffer);
  }
}

TEST(ClippingPreview, InternalWhitespaceStillCountsAgainstReadBudget) {
  Reader reader{"hello" + std::string(4000, ' ') + "world"};
  PreviewOutput out;
  ASSERT_TRUE(readPreview(reader, reader.text.size(), out));
  EXPECT_EQ(out.text(), std::string("hello") + ELLIPSIS);
  EXPECT_LE(reader.pos, 320u);
}
