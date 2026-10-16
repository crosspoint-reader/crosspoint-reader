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
std::vector<Line> layout(const std::vector<const char*>& words, const bool hyphenation, const uint16_t width) {
  GfxRenderer renderer;
  BlockStyle style;
  style.alignment = CssTextAlign::Justify;
  style.textIndentDefined = true;
  ParsedText text(hyphenation, false, style, 0);
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

std::vector<std::vector<std::string>> wordsOf(const std::vector<Line>& lines) {
  std::vector<std::vector<std::string>> result;
  result.reserve(lines.size());
  for (const auto& line : lines) result.push_back(line.words);
  return result;
}

}  // namespace

TEST(EmDashLineBreaking, HyphenationOffSplitsAcrossLines) {
  // "foo—" is 4 chars * 8 px = 32 px, fits in 40 px.
  // "bar" is 3 chars * 8 px = 24 px. Total "foo—bar" is 56 px, which exceeds 40 px.
  const auto lines = layout({"foo—bar"}, /*hyphenation=*/false, 40);
  const std::vector<std::vector<std::string>> expected{{"foo—"}, {"bar"}};
  EXPECT_EQ(wordsOf(lines), expected);
}

TEST(EmDashLineBreaking, HyphenationOffKeepsTogetherOnSameLineWithoutGap) {
  // 80 px fits "foo—" (32 px) and "bar" (24 px).
  const auto lines = layout({"foo—bar"}, /*hyphenation=*/false, 80);
  const std::vector<std::vector<std::string>> expected{{"foo—", "bar"}};
  ASSERT_EQ(wordsOf(lines), expected);
  // "bar" attaches directly to "foo—" with kerning 0, so xpos is exactly 0 and 32.
  EXPECT_EQ(lines[0].xpos, (std::vector<int16_t>{0, 32}));
}

TEST(EmDashLineBreaking, MultiEmDashStaysTogether) {
  // "foo——" is 5 chars * 8 px = 40 px. Width 45 fits "foo——", but not "bar".
  const auto lines = layout({"foo——bar"}, /*hyphenation=*/false, 45);
  const std::vector<std::vector<std::string>> expected{{"foo——"}, {"bar"}};
  EXPECT_EQ(wordsOf(lines), expected);
}

TEST(EmDashLineBreaking, MultipleEmDashesInWordCanBreakAtEitherDash) {
  // "foo—" (32 px), "bar—" (32 px), "baz" (24 px). Width 40 px.
  const auto lines = layout({"foo—bar—baz"}, /*hyphenation=*/false, 40);
  const std::vector<std::vector<std::string>> expected{{"foo—"}, {"bar—"}, {"baz"}};
  EXPECT_EQ(wordsOf(lines), expected);
}

TEST(EmDashLineBreaking, HyphenationOnSplitsWithoutInsertedHyphen) {
  const auto lines = layout({"foo—bar"}, /*hyphenation=*/true, 40);
  const std::vector<std::vector<std::string>> expected{{"foo—"}, {"bar"}};
  EXPECT_EQ(wordsOf(lines), expected);
}

TEST(EmDashLineBreaking, MixedCjkAndLatinSplitsAtDash) {
  // "漢" (8 px), "字—" (16 px), "bar" (24 px). Width 30 px fits "漢字—" (24 px), but not "bar".
  const auto lines = layout({"漢字—bar"}, /*hyphenation=*/false, 30);
  const std::vector<std::vector<std::string>> expected{{"漢", "字—"}, {"bar"}};
  EXPECT_EQ(wordsOf(lines), expected);
}

TEST(EmDashLineBreaking, MixedLatinAndCjkSplitsAtDash) {
  // "foo—" (32 px), "漢" (8 px), "字" (8 px). Width 35 px fits "foo—", but wraps CJK text.
  const auto lines = layout({"foo—漢字"}, /*hyphenation=*/false, 35);
  const std::vector<std::vector<std::string>> expected{{"foo—"}, {"漢", "字"}};
  EXPECT_EQ(wordsOf(lines), expected);
}

TEST(EmDashLineBreaking, MixedCjkMultiEmDashStaysTogether) {
  // "漢" (8 px), "字——" (24 px), "bar" (24 px). Width 35 px fits "漢字——", but wraps "bar".
  const auto lines = layout({"漢字——bar"}, /*hyphenation=*/false, 35);
  const std::vector<std::vector<std::string>> expected{{"漢", "字——"}, {"bar"}};
  EXPECT_EQ(wordsOf(lines), expected);
}
