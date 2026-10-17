// CJK text through the real ParsedText and TextBlock, against the fixed-width GfxRenderer shim of
// test/word_size_layout (every codepoint 10 px, a space 5 px, no kerning): a word with CJK characters
// becomes one token per allowed break, lines break between those tokens without a hyphen, and
// kinsoku pairs stay on one line.
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "Epub/ParsedText.h"
#include "Epub/blocks/TextBlock.h"
#include "GfxRenderer.h"

namespace {

constexpr int kFontId = 1;

struct Line {
  std::vector<std::string> words;
  std::vector<int16_t> xpos;
  std::vector<bool> continues;
  std::vector<EpdFontFamily::Style> styles;
  uint32_t visibleOffset = 0;
};

BlockStyle leftAligned() {
  BlockStyle bs;
  bs.alignment = CssTextAlign::Left;
  bs.textIndent = 0;
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
          line.styles.push_back(block->wordStyle(i));
        }
        line.visibleOffset = text.lastLineVisibleOffset();
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

}  // namespace

TEST(CjkTokens, EachHanCharacterIsATokenWithNoGapBetween) {
  ParsedText text(false, false, leftAligned());
  text.addWord("漢字仮名", EpdFontFamily::REGULAR);
  const auto lines = layout(text, 400);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0].words, (std::vector<std::string>{"漢", "字", "仮", "名"}));
  EXPECT_EQ(lines[0].xpos, (std::vector<int16_t>{0, 10, 20, 30}));
  // A CJK gap is a break, not a continuation: the dictionary selects one character run at a time.
  EXPECT_EQ(lines[0].continues, (std::vector<bool>{false, false, false, false}));
}

TEST(CjkTokens, LinesBreakBetweenCharactersWithoutAHyphen) {
  ParsedText text(false, false, leftAligned());
  text.addWord("一二三四五六七八九十百千", EpdFontFamily::REGULAR);
  const auto lines = layout(text, 55);
  EXPECT_EQ(wordsOf(lines), (std::vector<std::vector<std::string>>{
                                {"一", "二", "三", "四", "五"}, {"六", "七", "八", "九", "十"}, {"百", "千"}}));
}

TEST(CjkTokens, HyphenationOnStillBreaksWithoutAHyphen) {
  ParsedText text(false, /*hyphenationEnabled=*/true, leftAligned());
  text.addWord("一二三四五六七八九十百千", EpdFontFamily::REGULAR);
  for (const auto& line : layout(text, 55)) {
    for (const auto& word : line.words) EXPECT_EQ(word.find('-'), std::string::npos) << word;
  }
}

TEST(CjkKinsoku, ClosingPunctuationNeverStartsALine) {
  // 五。 is one token, so the line ends before 五 rather than leave 。 at the start of the next.
  ParsedText text(false, false, leftAligned());
  text.addWord("一二三四五。六", EpdFontFamily::REGULAR);
  EXPECT_EQ(wordsOf(layout(text, 50)),
            (std::vector<std::vector<std::string>>{{"一", "二", "三", "四"}, {"五。", "六"}}));
}

TEST(CjkKinsoku, AnOpeningBracketNeverEndsALine) {
  ParsedText text(false, false, leftAligned());
  text.addWord("一二三四「五」", EpdFontFamily::REGULAR);
  EXPECT_EQ(wordsOf(layout(text, 50)), (std::vector<std::vector<std::string>>{{"一", "二", "三", "四"}, {"「五」"}}));
}

TEST(CjkSpaces, AWordGapInTheSourceStaysASpace) {
  ParsedText text(false, false, leftAligned());
  text.addWord("漢字", EpdFontFamily::REGULAR);
  text.addWord("仮名", EpdFontFamily::REGULAR);
  const auto lines = layout(text, 400);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0].xpos, (std::vector<int16_t>{0, 10, 25, 35}));
}

TEST(CjkSpaces, CjkGluedAcrossAnInlineStyleIsABreakableZeroGap) {
  ParsedText text(false, false, leftAligned());
  text.addWord("漢字", EpdFontFamily::REGULAR);
  text.addWord("仮名", EpdFontFamily::BOLD, false, /*attachToPrevious=*/true);
  auto lines = layout(text, 400);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0].xpos, (std::vector<int16_t>{0, 10, 20, 30}));

  ParsedText narrow(false, false, leftAligned());
  narrow.addWord("漢字", EpdFontFamily::REGULAR);
  narrow.addWord("仮名", EpdFontFamily::BOLD, false, /*attachToPrevious=*/true);
  EXPECT_EQ(wordsOf(layout(narrow, 25)), (std::vector<std::vector<std::string>>{{"漢", "字"}, {"仮", "名"}}));
}

TEST(CjkSpaces, AGluedLatinWordAfterADashStaysGlued) {
  // Upstream's dash split is not ported: foo— + bar glued in the source stays one unbreakable unit.
  ParsedText text(false, false, leftAligned());
  text.addWord("foo—", EpdFontFamily::REGULAR);
  text.addWord("bar", EpdFontFamily::ITALIC, false, /*attachToPrevious=*/true);
  const auto lines = layout(text, 400);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0].continues, (std::vector<bool>{false, true}));
  EXPECT_EQ(lines[0].xpos, (std::vector<int16_t>{0, 40}));
}

TEST(CjkTokens, EachLineStartsAtItsFirstTokensVisibleOffset) {
  // 漢 (3 bytes) | 字。 (6 bytes) | 仮: tokens start at 100, 103 and 109.
  ParsedText text(false, false, leftAligned());
  text.addWord("漢字。仮", EpdFontFamily::REGULAR, false, false, ParsedText::DEFAULT_WORD_SIZE_PCT, 100);
  const auto lines = layout(text, 25);
  ASSERT_EQ(lines.size(), 3u);
  EXPECT_EQ(lines[0].visibleOffset, 100u);
  EXPECT_EQ(lines[1].visibleOffset, 103u);
  EXPECT_EQ(lines[2].visibleOffset, 109u);
}

TEST(CjkTokens, BionicReadingLeavesCjkTokensAlone) {
  ParsedText text(false, false, leftAligned(), /*bionicReadingEnabled=*/true);
  text.addWord("漢字", EpdFontFamily::REGULAR);
  text.addWord("reading", EpdFontFamily::REGULAR);
  const auto lines = layout(text, 400);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0].words, (std::vector<std::string>{"漢", "字", "read", "ing"}));
  EXPECT_EQ(lines[0].styles[0], EpdFontFamily::REGULAR);
  EXPECT_EQ(lines[0].styles[1], EpdFontFamily::REGULAR);
  EXPECT_EQ(lines[0].styles[2], EpdFontFamily::BOLD);
}
