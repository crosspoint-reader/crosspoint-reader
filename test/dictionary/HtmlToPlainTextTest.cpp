// Host tests for the plain-text fallback used when a dictionary definition's
// HTML cannot be laid out as styled pages.

#include <gtest/gtest.h>

#include <cstdlib>
#include <new>
#include <string>

#include "HtmlToPlainText.h"

namespace {
std::size_t allocations = 0;
}  // namespace

// Counts every plain operator new in the binary, so a test can read the delta
// across the conversion: the point of converting in place is that a definition
// of tens of KB never gets a second buffer that size.
void* operator new(const std::size_t size) {
  ++allocations;
  if (void* block = std::malloc(size == 0 ? 1 : size)) return block;
  throw std::bad_alloc();
}
void operator delete(void* block) noexcept { std::free(block); }
void operator delete(void* block, std::size_t) noexcept { std::free(block); }

namespace {

// The conversion is in place; the cases below read better as input -> output.
std::string plainText(std::string text) {
  htmlToPlainTextInPlace(text);
  return text;
}

TEST(HtmlToPlainText, ConvertsInTheInputBuffer) {
  const std::string body(60 * 1024, 'a');
  std::string text = "<p>" + body + "</p>";

  // The hook does see a copy of this size, so the zero below means something.
  const std::size_t beforeCopy = allocations;
  const std::string copy = text;
  ASSERT_GT(allocations, beforeCopy);

  const char* buffer = text.data();
  const std::size_t capacity = text.capacity();
  const std::size_t before = allocations;
  htmlToPlainTextInPlace(text);
  EXPECT_EQ(allocations, before);
  EXPECT_EQ(text.data(), buffer);
  EXPECT_EQ(text.capacity(), capacity);
  EXPECT_EQ(text, body);
}

// The output is written over the input, so a replacement that left no slack
// would overwrite input not yet read. References at every UTF-8 length, packed
// back to back from the first byte, are where that would show.
TEST(HtmlToPlainText, DecodesPackedReferencesWithoutOverrun) {
  // Shortest source per output length: 1 byte from 4, 2 from 6, 3 from 7, 4 from 8.
  EXPECT_EQ(plainText("&#9;&#99;"), "\tc");
  EXPECT_EQ(plainText("&#128;&#x80;&#x7FF;"), "\xC2\x80\xC2\x80\xDF\xBF");
  EXPECT_EQ(plainText("&#2048;&#x800;&#xFFFF;"), "\xE0\xA0\x80\xE0\xA0\x80\xEF\xBF\xBF");
  EXPECT_EQ(plainText("&#65536;&#x10000;&#x10FFFF;"), "\xF0\x90\x80\x80\xF0\x90\x80\x80\xF4\x8F\xBF\xBF");
  EXPECT_EQ(plainText("&#x1F600;&#128512;"), "\xF0\x9F\x98\x80\xF0\x9F\x98\x80");
  // The tightest named ones: four bytes of reference for three of UTF-8.
  std::string packed;
  std::string expected;
  for (int i = 0; i < 1000; i++) {
    packed += "&ge;&le;&ne;&mu;";
    expected += "\xE2\x89\xA5\xE2\x89\xA4\xE2\x89\xA0\xCE\xBC";
  }
  EXPECT_EQ(plainText(packed), expected);
  // Dropped code points consume their reference and emit nothing.
  EXPECT_EQ(plainText("a&#0;b&#xD800;c"), "abc");
  // Out of range: not a reference, left as written.
  EXPECT_EQ(plainText("&#x110000;"), "&#x110000;");
}

TEST(HtmlToPlainText, BreaksAndEntitiesInterleaveWithoutOverrun) {
  // Each <p> is three bytes for its two newlines; nothing between them.
  EXPECT_EQ(plainText("a<p>b<p>c<p>&amp;<p>&#x1F600;"),
            "a\n\nb\n\nc\n\n&\n\n"
            "\xF0\x9F\x98\x80");
  EXPECT_EQ(plainText("<p>a</p><p>b</p><br>&lt;<li>&gt;"), "a\n\nb\n\n<\n>");
  EXPECT_EQ(plainText("x\r\n\ty\n\nz"), "x\ny\nz");
}

TEST(HtmlToPlainText, StripsTagsAndKeepsText) {
  EXPECT_EQ(plainText("<b>quixotic</b> <i>adj.</i>"), "quixotic adj.");
  EXPECT_EQ(plainText("plain"), "plain");
  EXPECT_EQ(plainText(""), "");
  EXPECT_EQ(plainText("<span class=\"x\">in span</span>"), "in span");
}

TEST(HtmlToPlainText, BlockElementsBecomeBreaks) {
  EXPECT_EQ(plainText("a<br>b"), "a\nb");
  EXPECT_EQ(plainText("<div>a</div><div>b</div>"), "a\nb");
  // A paragraph is worth a blank line; consecutive breaks do not stack up.
  EXPECT_EQ(plainText("<p>a</p><p>b</p>"), "a\n\nb");
  EXPECT_EQ(plainText("<h1>title</h1>body"), "title\n\nbody");
  EXPECT_EQ(plainText("a<br><br><br>b"), "a\nb");
}

TEST(HtmlToPlainText, DecodesEntities) {
  EXPECT_EQ(plainText("Tom &amp; Jerry"), "Tom & Jerry");
  EXPECT_EQ(plainText("a&nbsp;b"),
            "a\xC2\xA0"
            "b");
  EXPECT_EQ(plainText("&#65;&#66;"), "AB");
  EXPECT_EQ(plainText("&#x2014;"), "\xE2\x80\x94");
  // Not entities: left as written rather than swallowed.
  EXPECT_EQ(plainText("&notanentity;"), "&notanentity;");
  EXPECT_EQ(plainText("100% & up"), "100% & up");
}

TEST(HtmlToPlainText, TrimsSurroundingWhitespace) {
  EXPECT_EQ(plainText("<p>only</p>"), "only");
  EXPECT_EQ(plainText("text   "), "text");
  EXPECT_EQ(plainText("a\tb"), "a b");
  // A leading break has nothing to separate, so it is dropped.
  EXPECT_EQ(plainText("<br>a"), "a");
}

TEST(HtmlToPlainText, SurvivesMalformedMarkup) {
  // An unterminated tag is text, not a tag that eats the rest of the entry.
  EXPECT_EQ(plainText("a <b"), "a <b");
  EXPECT_EQ(plainText("x < y"), "x < y");
  EXPECT_EQ(plainText("<!-- comment -->kept"), "kept");
}

}  // namespace
