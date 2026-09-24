#include <Epub/ParsedText.h>
#include <Epub/blocks/TextBlock.h>
#include <Epub/hyphenation/Hyphenator.h>
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
  ParsedText text(false, hyphenation, false, style);
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

TEST(KoreanLineBreaking, HyphenationOffWrapsAtSpacesOnly) {
  Hyphenator::setPreferredLanguage("ko");
  // 가나 다라마바 needs 52 px; with hyphenation off the Hangul word moves down whole.
  const auto lines = layout({"가나", "다라마바", "사"}, false, 50);
  const std::vector<std::vector<std::string>> expected{{"가나"}, {"다라마바", "사"}};
  EXPECT_EQ(wordsOf(lines), expected);
}

TEST(KoreanLineBreaking, HyphenationOnSplitsBetweenSyllablesWithoutHyphen) {
  Hyphenator::setPreferredLanguage("ko");
  // 가나 + space leaves 30 px: the widest syllable prefix that fits is 다라마 (24 px).
  const auto lines = layout({"가나", "다라마바사아", "자"}, true, 50);
  const std::vector<std::vector<std::string>> expected{{"가나", "다라마"}, {"바사아", "자"}};
  ASSERT_EQ(wordsOf(lines), expected);
  // 16 + 4 + 24 = 44 px leaves 6 px, all of it on the one word space.
  EXPECT_EQ(lines[0].xpos, (std::vector<int16_t>{0, 26}));
}

TEST(KoreanLineBreaking, HyphenationOnLeavesTwoSyllablesOnEachSide) {
  Hyphenator::setPreferredLanguage("ko");
  // 가나다 + space leaves 28 px. 라마바 (24 px) would fit but leaves one syllable, so 라마 is used.
  const auto lines = layout({"가나다", "라마바사.", "자"}, true, 56);
  const std::vector<std::vector<std::string>> expected{{"가나다", "라마"}, {"바사.", "자"}};
  EXPECT_EQ(wordsOf(lines), expected);
}

TEST(KoreanLineBreaking, HyphenationOnMovesShortWordsDownWhole) {
  Hyphenator::setPreferredLanguage("ko");
  // 라마바 has no legal split, so it moves down even though 라마 would fit.
  const auto lines = layout({"가나다", "라마바", "자"}, true, 50);
  const std::vector<std::vector<std::string>> expected{{"가나다"}, {"라마바", "자"}};
  EXPECT_EQ(wordsOf(lines), expected);
}

TEST(KoreanLineBreaking, HyphenationOnBreaksAfterVisibleHyphen) {
  Hyphenator::setPreferredLanguage("ko");
  // 가 + space leaves 44 px: 대한민국- (40 px) beats the syllable split 대한 (16 px).
  const auto lines = layout({"가", "대한민국-서울"}, true, 56);
  const std::vector<std::vector<std::string>> expected{{"가", "대한민국-"}, {"서울"}};
  EXPECT_EQ(wordsOf(lines), expected);
}
