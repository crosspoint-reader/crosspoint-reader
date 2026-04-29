#include <cstdio>
#include <string>
#include <vector>

#include "../../lib/Md/MdParser.h"

static int testsPassed = 0;
static int testsFailed = 0;

#define ASSERT_EQ(a, b)                                                         \
  do {                                                                          \
    if ((a) != (b)) {                                                           \
      fprintf(stderr, "  FAIL: %s:%d: %s != %s\n", __FILE__, __LINE__, #a, #b); \
      testsFailed++;                                                            \
      return;                                                                   \
    }                                                                           \
  } while (0)

#define PASS() testsPassed++

static std::string flattenText(const std::vector<MdParser::Span>& spans) {
  std::string result;
  for (const auto& span : spans) {
    result += span.text;
  }
  return result;
}

static bool allRegular(const std::vector<MdParser::Span>& spans) {
  for (const auto& span : spans) {
    if (span.style != EpdFontFamily::REGULAR) {
      return false;
    }
  }
  return true;
}

void testSnakeCaseUnderscoresRemainLiteral() {
  printf("testSnakeCaseUnderscoresRemainLiteral...\n");
  auto spans = MdParser::parseInline("foo_bar_baz");
  ASSERT_EQ(flattenText(spans), "foo_bar_baz");
  ASSERT_EQ(allRegular(spans), true);
  PASS();
}

void testUnderscoreWithinExpressionRemainsLiteral() {
  printf("testUnderscoreWithinExpressionRemainsLiteral...\n");
  auto spans = MdParser::parseInline("a_b + c_d");
  ASSERT_EQ(flattenText(spans), "a_b + c_d");
  ASSERT_EQ(allRegular(spans), true);
  PASS();
}

void testUnderscoreEmphasisStillWorks() {
  printf("testUnderscoreEmphasisStillWorks...\n");
  auto spans = MdParser::parseInline("foo _bar_ baz");
  ASSERT_EQ(flattenText(spans), "foo bar baz");
  ASSERT_EQ(spans.size(), 3);
  ASSERT_EQ(spans[1].style, EpdFontFamily::ITALIC);
  PASS();
}

void testAsteriskEmphasisStillWorks() {
  printf("testAsteriskEmphasisStillWorks...\n");
  auto spans = MdParser::parseInline("foo *bar* baz");
  ASSERT_EQ(flattenText(spans), "foo bar baz");
  ASSERT_EQ(spans.size(), 3);
  ASSERT_EQ(spans[1].style, EpdFontFamily::ITALIC);
  PASS();
}

void testStrikethroughWorks() {
  printf("testStrikethroughWorks...\n");
  auto spans = MdParser::parseInline("foo ~~bar~~ baz");
  ASSERT_EQ(flattenText(spans), "foo bar baz");
  ASSERT_EQ(spans.size(), 3);
  ASSERT_EQ(spans[1].style, EpdFontFamily::STRIKETHROUGH);
  PASS();
}

void testEscapedTildeDoesNotToggleStrikethrough() {
  printf("testEscapedTildeDoesNotToggleStrikethrough...\n");
  auto spans = MdParser::parseInline("foo \\~~bar~~ baz");
  ASSERT_EQ(flattenText(spans), "foo ~~bar~~ baz");
  ASSERT_EQ(spans.size(), 1);
  ASSERT_EQ(spans[0].style, EpdFontFamily::REGULAR);
  PASS();
}

void testUnderscoreBoldWorks() {
  printf("testUnderscoreBoldWorks...\n");
  auto spans = MdParser::parseInline("foo __bar__ baz");
  ASSERT_EQ(flattenText(spans), "foo bar baz");
  ASSERT_EQ(spans.size(), 3);
  ASSERT_EQ(spans[1].style, EpdFontFamily::BOLD);
  PASS();
}

void testNestedUnorderedListIndentLevel() {
  printf("testNestedUnorderedListIndentLevel...\n");
  auto parsed = MdParser::parseLine("    - nested item", false);
  ASSERT_EQ(parsed.blockType, MdParser::BlockType::UnorderedList);
  ASSERT_EQ(parsed.listPrefix, "\xe2\x80\xa2 ");
  ASSERT_EQ(parsed.indentLevel, 1);
  ASSERT_EQ(flattenText(parsed.spans), "nested item");
  PASS();
}

void testNestedOrderedListIndentLevel() {
  printf("testNestedOrderedListIndentLevel...\n");
  auto parsed = MdParser::parseLine("        1. nested ordered", false);
  ASSERT_EQ(parsed.blockType, MdParser::BlockType::OrderedList);
  ASSERT_EQ(parsed.listPrefix, "1. ");
  ASSERT_EQ(parsed.indentLevel, 2);
  ASSERT_EQ(flattenText(parsed.spans), "nested ordered");
  PASS();
}

int main() {
  printf("=== Markdown Parser Tests ===\n\n");

  testSnakeCaseUnderscoresRemainLiteral();
  testUnderscoreWithinExpressionRemainsLiteral();
  testUnderscoreEmphasisStillWorks();
  testAsteriskEmphasisStillWorks();
  testStrikethroughWorks();
  testUnderscoreBoldWorks();
  testEscapedTildeDoesNotToggleStrikethrough();
  testNestedUnorderedListIndentLevel();
  testNestedOrderedListIndentLevel();

  printf("\n=== Results: %d passed, %d failed ===\n", testsPassed, testsFailed);
  return testsFailed > 0 ? 1 : 0;
}
