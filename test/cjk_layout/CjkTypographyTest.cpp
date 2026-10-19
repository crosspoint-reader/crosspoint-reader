// CJK punctuation widths, line-edge widths, flush justification and the whole-em grid (spec sections 5.1, 5.2,
// 5.5), through the real ParsedText and TextBlock against the fixed-width shim of test/word_size_layout: every
// codepoint 10 px (so 一 is a 10 px em and every CJK mark is full-width), a space 5 px. The shim draws a CJK
// opening bracket's ink on the right half of its advance, a closing bracket, 。 and 、 on the left half.
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "Epub/ParsedText.h"
#include "Epub/TypographyProfile.h"
#include "Epub/blocks/TextBlock.h"
#include "Epub/hyphenation/Hyphenator.h"
#include "GfxRenderer.h"

namespace {

constexpr int kFontId = 1;

struct Line {
  std::vector<std::string> words;
  std::vector<int16_t> xpos;
  std::vector<bool> continues;
};

BlockStyle aligned(const CssTextAlign alignment, const int16_t indent = 0) {
  BlockStyle bs;
  bs.alignment = alignment;
  bs.textIndent = indent;
  bs.textIndentDefined = true;
  return bs;
}

std::vector<Line> layout(ParsedText& text, const uint16_t width) {
  GfxRenderer renderer;
  std::vector<Line> lines;
  text.layoutAndExtractLines(
      renderer, kFontId, width,
      [&](std::unique_ptr<TextBlock> block, bool, bool) {
        Line& line = lines.emplace_back();
        for (uint16_t i = 0; i < block->wordCount(); ++i) {
          line.words.emplace_back(block->wordText(i));
          line.xpos.push_back(block->wordXpos(i));
          line.continues.push_back(block->wordContinues(i));
        }
        return ParsedText::LineProcessResult::Accepted;
      },
      /*includeLastLine=*/true);
  return lines;
}

std::vector<std::vector<std::string>> wordsOf(const std::vector<Line>& lines) {
  std::vector<std::vector<std::string>> result;
  for (const auto& line : lines) result.push_back(line.words);
  return result;
}

using Words = std::vector<std::vector<std::string>>;
using X = std::vector<int16_t>;

}  // namespace

TEST(CjkPunctuationWidths, EachMarkIsATokenGluedWhereKinsokuHoldsIt) {
  ParsedText text(false, false, aligned(CssTextAlign::Left));
  text.addWord("字」。次", EpdFontFamily::REGULAR);
  const auto lines = layout(text, 400);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0].words, (std::vector<std::string>{"字", "」", "。", "次"}));
  EXPECT_EQ(lines[0].continues, (std::vector<bool>{false, true, true, false}));
}
TEST(CjkPunctuationWidths, ALatinWordKeepsItsCurlyQuotes) {
  ParsedText text(false, false, aligned(CssTextAlign::Left));
  text.addWord("\xE2\x80\x9CHello\xE2\x80\x9D", EpdFontFamily::REGULAR);  // “Hello”
  const auto lines = layout(text, 400);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0].words.size(), 1u);
}
