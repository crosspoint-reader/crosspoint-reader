#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "../../lib/Md/MdParser.h"

namespace {
std::string flattenText(const std::vector<MdParser::Span>& spans) {
  std::string result;
  for (const auto& span : spans) {
    result += span.text;
  }
  return result;
}

bool allRegular(const std::vector<MdParser::Span>& spans) {
  for (const auto& span : spans) {
    if (span.style != EpdFontFamily::REGULAR) {
      return false;
    }
  }
  return true;
}
}  // namespace

TEST(MdParser, SnakeCaseUnderscoresRemainLiteral) {
  auto spans = MdParser::parseInline("foo_bar_baz");
  ASSERT_EQ(flattenText(spans), "foo_bar_baz");
  ASSERT_EQ(allRegular(spans), true);
}

TEST(MdParser, UnderscoreWithinExpressionRemainsLiteral) {
  auto spans = MdParser::parseInline("a_b + c_d");
  ASSERT_EQ(flattenText(spans), "a_b + c_d");
  ASSERT_EQ(allRegular(spans), true);
}

TEST(MdParser, UnderscoreEmphasisStillWorks) {
  auto spans = MdParser::parseInline("foo _bar_ baz");
  ASSERT_EQ(flattenText(spans), "foo bar baz");
  ASSERT_EQ(spans.size(), static_cast<size_t>(3));
  ASSERT_EQ(spans[1].style, EpdFontFamily::ITALIC);
}

TEST(MdParser, AsteriskEmphasisStillWorks) {
  auto spans = MdParser::parseInline("foo *bar* baz");
  ASSERT_EQ(flattenText(spans), "foo bar baz");
  ASSERT_EQ(spans.size(), static_cast<size_t>(3));
  ASSERT_EQ(spans[1].style, EpdFontFamily::ITALIC);
}

TEST(MdParser, StrikethroughWorks) {
  auto spans = MdParser::parseInline("foo ~~bar~~ baz");
  ASSERT_EQ(flattenText(spans), "foo bar baz");
  ASSERT_EQ(spans.size(), static_cast<size_t>(3));
  ASSERT_EQ(spans[1].style, EpdFontFamily::STRIKETHROUGH);
}

TEST(MdParser, EscapedTildeDoesNotToggleStrikethrough) {
  auto spans = MdParser::parseInline("foo \\~~bar~~ baz");
  ASSERT_EQ(flattenText(spans), "foo ~~bar~~ baz");
  ASSERT_EQ(spans.size(), static_cast<size_t>(1));
  ASSERT_EQ(spans[0].style, EpdFontFamily::REGULAR);
}

TEST(MdParser, UnderscoreBoldWorks) {
  auto spans = MdParser::parseInline("foo __bar__ baz");
  ASSERT_EQ(flattenText(spans), "foo bar baz");
  ASSERT_EQ(spans.size(), static_cast<size_t>(3));
  ASSERT_EQ(spans[1].style, EpdFontFamily::BOLD);
}

TEST(MdParser, NestedUnorderedListIndentLevel) {
  auto parsed = MdParser::parseLine("    - nested item", false);
  ASSERT_EQ(parsed.blockType, MdParser::BlockType::UnorderedList);
  ASSERT_EQ(parsed.listPrefix, "\xe2\x80\xa2 ");
  ASSERT_EQ(parsed.indentLevel, 1);
  ASSERT_EQ(flattenText(parsed.spans), "nested item");
}

TEST(MdParser, NestedOrderedListIndentLevel) {
  auto parsed = MdParser::parseLine("        1. nested ordered", false);
  ASSERT_EQ(parsed.blockType, MdParser::BlockType::OrderedList);
  ASSERT_EQ(parsed.listPrefix, "1. ");
  ASSERT_EQ(parsed.indentLevel, 2);
  ASSERT_EQ(flattenText(parsed.spans), "nested ordered");
}

TEST(MdParser, TableRowSplitsCellsWithInlineStyles) {
  auto parsed = MdParser::parseLine("| plain | **bold** | `code` |", false);
  ASSERT_EQ(parsed.blockType, MdParser::BlockType::TableRow);
  ASSERT_EQ(parsed.tableCells.size(), static_cast<size_t>(3));
  ASSERT_EQ(flattenText(parsed.tableCells[0]), "plain");
  ASSERT_EQ(flattenText(parsed.tableCells[1]), "bold");
  ASSERT_EQ(parsed.tableCells[1][0].style, EpdFontFamily::BOLD);
  ASSERT_EQ(flattenText(parsed.tableCells[2]), "code");
}

TEST(MdParser, TableSeparatorDetection) {
  ASSERT_TRUE(MdParser::isTableSeparator("| --- | --- |"));
  ASSERT_TRUE(MdParser::isTableSeparator("| :--- | :---: | ---: |"));
  ASSERT_FALSE(MdParser::isTableSeparator("| a | b |"));
  ASSERT_FALSE(MdParser::isTableSeparator("plain text"));
}

TEST(MdParser, TableAlignmentsFromSeparator) {
  auto aligns = MdParser::parseTableAlignments("| :--- | :---: | ---: | --- |");
  ASSERT_EQ(aligns.size(), static_cast<size_t>(4));
  ASSERT_EQ(aligns[0], MdParser::TableAlign::Left);
  ASSERT_EQ(aligns[1], MdParser::TableAlign::Center);
  ASSERT_EQ(aligns[2], MdParser::TableAlign::Right);
  ASSERT_EQ(aligns[3], MdParser::TableAlign::Left);
}

TEST(MdParser, TableAlignmentsWithoutLeadingPipeOrSpaces) {
  auto aligns = MdParser::parseTableAlignments("  :---: | ---:");
  ASSERT_EQ(aligns.size(), static_cast<size_t>(2));
  ASSERT_EQ(aligns[0], MdParser::TableAlign::Center);
  ASSERT_EQ(aligns[1], MdParser::TableAlign::Right);
}

TEST(MdParser, TableRowEscapedPipeStaysInCell) {
  auto parsed = MdParser::parseLine("| a \\| b | c |", false);
  ASSERT_EQ(parsed.blockType, MdParser::BlockType::TableRow);
  ASSERT_EQ(parsed.tableCells.size(), static_cast<size_t>(2));
}
