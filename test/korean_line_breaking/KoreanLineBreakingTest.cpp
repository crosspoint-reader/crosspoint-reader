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

TEST(KoreanLineBreaking, HyphenationOnKeepsTrailingPunctuationWithSyllable) {
  Hyphenator::setPreferredLanguage("ko");
  // 가나다 + space leaves 36 px. 라마바사 (32 px) would fit, but 사. stays together, so 라마바 is used.
  const auto lines = layout({"가나다", "라마바사.", "자"}, true, 64);
  const std::vector<std::vector<std::string>> expected{{"가나다", "라마바"}, {"사.", "자"}};
  EXPECT_EQ(wordsOf(lines), expected);
}

TEST(KoreanLineBreaking, HyphenationOnSplitsShortWords) {
  Hyphenator::setPreferredLanguage("ko");
  // 가나다 + space leaves 22 px, so the three-syllable word splits after 라마 (16 px).
  const auto lines = layout({"가나다", "라마바", "자"}, true, 50);
  const std::vector<std::vector<std::string>> expected{{"가나다", "라마"}, {"바", "자"}};
  EXPECT_EQ(wordsOf(lines), expected);
}

TEST(KoreanLineBreaking, HyphenationOnSplitsWhereHangulMeetsOtherScriptsOrBrackets) {
  Hyphenator::setPreferredLanguage("ko");
  // 가 + space leaves 52 px. 소신(ab) (48 px) fits; the split may fall before "(" or after ")",
  // never inside the brackets.
  const auto lines = layout({"가", "소신(ab)이"}, true, 64);
  const std::vector<std::vector<std::string>> expected{{"가", "소신(ab)"}, {"이"}};
  EXPECT_EQ(wordsOf(lines), expected);
}

TEST(KoreanLineBreaking, HyphenationOnSplitsBetweenDigitAndHangul) {
  Hyphenator::setPreferredLanguage("ko");
  // 가나다 + space leaves 22 px: 12 (16 px) fits, 12월 (24 px) does not.
  const auto lines = layout({"가나다", "12월부터", "자"}, true, 50);
  const std::vector<std::vector<std::string>> expected{{"가나다", "12"}, {"월부터", "자"}};
  EXPECT_EQ(wordsOf(lines), expected);
}

TEST(LineBreakCost, MeasuresEachWordGapOnceInTheBreakSearch) {
  // The optimal-break search reaches the gap before word j from every line start
  // within a line of it. Measuring it there each time costs ~words x words-per-line
  // lookups (a UTF-8 decode and an advance/kerning lookup each, SD-backed on device).
  GfxRenderer renderer;
  BlockStyle style;
  ParsedText text(false, false, style, 0);
  constexpr int kWords = 400;
  for (int i = 0; i < kWords; ++i) text.addWord("word", EpdFontFamily::REGULAR);
  GfxRenderer::spaceAdvanceCalls = 0;
  size_t lines = 0;
  text.layoutAndExtractLines(renderer, 0, 480, [&](std::unique_ptr<TextBlock>, auto) { ++lines; });
  EXPECT_GT(lines, 20U);
  EXPECT_LE(GfxRenderer::spaceAdvanceCalls, 3 * kWords) << "gap measured more than once per word";
}

namespace {
// Independent oracle for plain space-separated words (no indent, hyphenation or attached
// tokens): minimum sum of squared slack over all lines but the last, ties to the longer line,
// with every gap measured directly from the renderer.
std::vector<uint16_t> referenceWordsPerLine(const GfxRenderer& renderer, const std::vector<std::string>& words,
                                            const int pageWidth) {
  const size_t n = words.size();
  const auto gapBefore = [&](const size_t j) {
    return renderer.getSpaceAdvance(0, static_cast<unsigned char>(words[j - 1].back()),
                                    static_cast<unsigned char>(words[j].front()), EpdFontFamily::REGULAR);
  };
  std::vector<long long> cost(n + 1, 0);
  std::vector<size_t> last(n, 0);
  for (size_t i = n; i-- > 0;) {
    long long best = -1;
    int length = 0;
    for (size_t j = i; j < n; ++j) {
      length += static_cast<int>(words[j].size()) * 8 + (j > i ? gapBefore(j) : 0);
      if (length > pageWidth) break;
      const long long slack = pageWidth - length;
      const long long candidate = j == n - 1 ? 0 : slack * slack + cost[j + 1];
      if (best < 0 || candidate <= best) {
        best = candidate;
        last[i] = j;
      }
    }
    cost[i] = best;
  }
  std::vector<uint16_t> lines;
  for (size_t i = 0; i < n; i = last[i] + 1) lines.push_back(static_cast<uint16_t>(last[i] - i + 1));
  return lines;
}
}  // namespace

TEST(LineBreakCost, CachedGapsMatchDirectMeasurement) {
  // The fixture's uniform 4 px space cannot tell one boundary's gap from another's, so make
  // gaps depend on the letter pair and compare with the oracle above. Narrow pages keep every
  // line inside the 32-entry gap window, 600 px straddles it and wider pages go well past it.
  GfxRenderer::pairDependentGaps = true;
  GfxRenderer renderer;
  uint32_t state = 20261001;
  const auto next = [&state](const uint32_t n) {
    state = state * 1664525u + 1013904223u;
    return (state >> 8) % n;
  };
  for (const int pageWidth : {150, 480, 600, 800, 2000}) {
    for (int paragraph = 0; paragraph < 40; ++paragraph) {
      BlockStyle style;
      style.alignment = CssTextAlign::Left;
      ParsedText text(false, false, style, 0);
      std::vector<std::string> words;
      const int count = 1 + static_cast<int>(next(400));
      const int maxLength = pageWidth >= 600 ? 2 : 6;
      for (int i = 0; i < count; ++i) {
        std::string word;
        for (int k = 1 + static_cast<int>(next(maxLength)); k > 0; --k)
          word.push_back(static_cast<char>('a' + next(26)));
        text.addWord(word, EpdFontFamily::REGULAR);
        words.push_back(std::move(word));
      }
      std::vector<uint16_t> wordsPerLine;
      text.layoutAndExtractLines(
          renderer, 0, static_cast<uint16_t>(pageWidth),
          [&](std::unique_ptr<TextBlock> block, auto) { wordsPerLine.push_back(block->wordCount()); });
      ASSERT_EQ(wordsPerLine, referenceWordsPerLine(renderer, words, pageWidth))
          << "page width " << pageWidth << ", paragraph " << paragraph;
    }
  }
  GfxRenderer::pairDependentGaps = false;
}
