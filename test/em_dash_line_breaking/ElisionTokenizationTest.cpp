#include <Epub/ParsedText.h>
#include <Epub/blocks/TextBlock.h>
#include <GfxRenderer.h>
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

namespace {

struct Line {
  std::vector<std::string> words;
  std::vector<int16_t> xpos;
};

// Fixture metrics (stub renderer): every glyph is 8 px wide and a space is 4 px.
std::vector<Line> layout(const std::vector<const char*>& words, const uint16_t width, const bool focus = false) {
  GfxRenderer renderer;
  BlockStyle style;
  style.alignment = CssTextAlign::Left;
  style.textIndentDefined = true;
  ParsedText text(/*hyphenationEnabled=*/false, focus, style, 0);
  for (const char* word : words) text.addWord(word, EpdFontFamily::REGULAR);
  std::vector<Line> lines;
  text.layoutAndExtractLines(renderer, 0, width, [&](std::unique_ptr<TextBlock> block, auto) {
    auto& line = lines.emplace_back();
    for (uint16_t i = 0; i < block->wordCount(); ++i) {
      line.words.emplace_back(block->wordText(i));
      line.xpos.push_back(block->wordXpos(i));
    }
  });
  return lines;
}

using Words = std::vector<std::string>;

Words firstLineWords(const char* word, const bool focus = false) {
  const auto lines = layout({word}, 400, focus);
  return lines.empty() ? Words{} : lines.front().words;
}

}  // namespace

TEST(ElisionTokenization, ContractionSplitsIntoThreeTokens) {
  EXPECT_EQ(firstLineWords("we're"), (Words{"we", "'", "re"}));
  EXPECT_EQ(firstLineWords("you'll"), (Words{"you", "'", "ll"}));
}

TEST(ElisionTokenization, FrenchAndItalianElisionsSplit) {
  EXPECT_EQ(firstLineWords("qu'il"), (Words{"qu", "'", "il"}));
  EXPECT_EQ(firstLineWords("dell'arte"), (Words{"dell", "'", "arte"}));
  EXPECT_EQ(firstLineWords("l'home"), (Words{"l", "'", "home"}));
}

TEST(ElisionTokenization, CurlyApostropheSplits) {
  EXPECT_EQ(firstLineWords("qu\xE2\x80\x99il"), (Words{"qu", "\xE2\x80\x99", "il"}));
}

TEST(ElisionTokenization, SeveralApostrophesSplitEachOne) {
  EXPECT_EQ(firstLineWords("rock'n'roll"), (Words{"rock", "'", "n", "'", "roll"}));
}

TEST(ElisionTokenization, ApostropheWithoutLettersOnBothSidesStaysWhole) {
  EXPECT_EQ(firstLineWords("'tis"), (Words{"'tis"}));
  EXPECT_EQ(firstLineWords("dogs'"), (Words{"dogs'"}));
  EXPECT_EQ(firstLineWords("1990's"), (Words{"1990's"}));
}

TEST(ElisionTokenization, PiecesAreDrawnWithoutGaps) {
  const auto lines = layout({"we're"}, 400);
  ASSERT_EQ(lines.size(), 1u);
  ASSERT_EQ(lines[0].words.size(), 3u);
  // "we" (16 px), "'" (8 px), "re" (16 px): each piece starts where the previous one ends.
  EXPECT_EQ(lines[0].xpos[0], 0);
  EXPECT_EQ(lines[0].xpos[1], 16);
  EXPECT_EQ(lines[0].xpos[2], 24);
}

TEST(ElisionTokenization, LineNeverBreaksInsideTheWord) {
  // "aa" (16) + space (4) + "we're" (40) = 60 px does not fit in 50 px: the whole word wraps.
  const auto lines = layout({"aa", "we're"}, 50);
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_EQ(lines[0].words, (Words{"aa"}));
  EXPECT_EQ(lines[1].words, (Words{"we", "'", "re"}));
}

TEST(ElisionTokenization, HyphenStillAllowsABreakNextToAnElision) {
  // The hyphen keeps its breakable boundary; the elision stays glued.
  EXPECT_EQ(firstLineWords("well-we're"), (Words{"well-", "we", "'", "re"}));
}

TEST(ElisionTokenization, SplitsWithFocusReadingToo) {
  EXPECT_EQ(firstLineWords("we're", true), (Words{"we", "'", "re"}));
  EXPECT_EQ(firstLineWords("l'agent", true), (Words{"l", "'", "agent"}));
  EXPECT_EQ(firstLineWords("'tis", true), (Words{"'tis"}));  // not between two letters
}
