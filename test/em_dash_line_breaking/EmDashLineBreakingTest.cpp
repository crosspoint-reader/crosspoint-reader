#include <Epub/ParsedText.h>
#include <Epub/blocks/TextBlock.h>
#include <Epub/hyphenation/Hyphenator.h>
#include <GfxRenderer.h>
#include <gtest/gtest.h>

#include <algorithm>
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

TEST(PartialLineLayout, WiderRemainderDoesNotKeepSpeculativeHyphens) {
  Hyphenator::setPreferredLanguage("en");
  for (const bool hyphenation : {false, true}) {
    for (const bool includeLastLine : {false, true}) {
      for (const size_t limit : {0u, 1u, 3u}) {
        SCOPED_TRACE(::testing::Message() << hyphenation << ", " << includeLastLine << ", " << limit);
        GfxRenderer renderer;
        ParsedText text(hyphenation, false, BlockStyle{}, 0);
        for (const char* word : {"nd",  "as",    "Jesus", "times",  "it",  "had", "laid",  "hold",  "him", "and",  "he",
                                 "was", "bound", "with",  "chains", "but", "he",  "would", "break", "the", "bonds"}) {
          text.addWord(word, EpdFontFamily::REGULAR);
        }
        const char* lastWord = hyphenation ? "asunder." : "abcdefghijklmnopqrstuvwxyz";
        text.addWord(lastWord, EpdFontFamily::REGULAR);
        size_t narrowLines = 0;
        text.layoutAndExtractLines(
            renderer, 0, 203,
            [&](std::unique_ptr<TextBlock>, auto) {
              ++narrowLines;
              EXPECT_EQ(text.linesAfterCurrentLine(), -1);
            },
            includeLastLine, 0, 100, limit);
        EXPECT_EQ(narrowLines, limit);

        std::vector<std::string> remainingWords;
        text.layoutAndExtractLines(renderer, 0, 240, [&](std::unique_ptr<TextBlock> block, auto) {
          for (uint16_t i = 0; i < block->wordCount(); ++i) {
            if (block->wordHasDiscretionaryHyphen(i)) {
              EXPECT_EQ(i + 1, block->wordCount());
            }
            remainingWords.emplace_back(block->wordText(i));
          }
        });
        EXPECT_NE(std::find(remainingWords.begin(), remainingWords.end(), lastWord), remainingWords.end());
        EXPECT_TRUE(text.isEmpty());
      }
    }
  }
}

TEST(PartialLineLayout, HoldsTheParagraphLastLineUntilItIsComplete) {
  GfxRenderer renderer;
  ParsedText text(true, false, BlockStyle{}, 0);
  for (const char* word : {"one", "two", "three"}) text.addWord(word, EpdFontFamily::REGULAR);
  size_t lines = 0;
  const auto collect = [&](std::unique_ptr<TextBlock>, auto) { ++lines; };
  text.layoutAndExtractLines(renderer, 0, 60, collect, false, 0, 100, 1);
  EXPECT_EQ(lines, 1u);
  text.layoutAndExtractLines(renderer, 0, 200, collect, false, 0, 100, 3);
  EXPECT_EQ(lines, 1u);
  text.layoutAndExtractLines(renderer, 0, 200, collect);
  EXPECT_EQ(lines, 2u);
  EXPECT_TRUE(text.isEmpty());
}

TEST(PartialLineLayout, LastEmittedLineStillJustifiesWhenMoreTextRemains) {
  for (const bool hyphenation : {false, true}) {
    GfxRenderer renderer;
    ParsedText text(hyphenation, false, BlockStyle{}, 0);
    for (const char* word : {"one", "two", "three"}) text.addWord(word, EpdFontFamily::REGULAR);
    text.layoutAndExtractLines(
        renderer, 0, 80,
        [&](std::unique_ptr<TextBlock> line, auto) {
          ASSERT_EQ(line->wordCount(), 2);
          EXPECT_EQ(line->wordXpos(1), 56);
        },
        true, 0, 100, 1);
  }
}

TEST(PartialLineLayout, BoundedLinesReserveRubyOverhangAtBothMargins) {
  for (const bool hyphenation : {false, true}) {
    for (const bool rubyFirst : {false, true}) {
      GfxRenderer renderer;
      ParsedText text(hyphenation, false, BlockStyle{}, 0);
      text.addWord(rubyFirst ? "a" : "word", EpdFontFamily::REGULAR);
      text.addWord(rubyFirst ? "word" : "a", EpdFontFamily::REGULAR);
      text.addWord("tail", EpdFontFamily::REGULAR);
      text.setRubyForWordAt(rubyFirst ? 0 : 1, "ruby");
      text.layoutAndExtractLines(
          renderer, 0, 52, [&](std::unique_ptr<TextBlock> line, auto) { EXPECT_EQ(line->wordCount(), 1); }, true, 0,
          100, 1);
    }
  }
}

TEST(PartialLineLayout, FollowingTextCanAbsorbRubyOverhang) {
  for (const bool hyphenation : {false, true}) {
    for (const bool attached : {false, true}) {
      for (const size_t limit : {size_t{1}, SIZE_MAX}) {
        GfxRenderer renderer;
        ParsedText text(hyphenation, false, BlockStyle{}, 0);
        text.addWord("a", EpdFontFamily::REGULAR);
        text.setRubyForWordAt(0, attached ? "ruby" : "longer");
        text.addWord("b", EpdFontFamily::REGULAR, false, attached);
        text.layoutAndExtractLines(
            renderer, 0, attached ? 38 : 58,
            [&](std::unique_ptr<TextBlock> line, auto) { EXPECT_EQ(line->wordCount(), 2); }, true, 0, 100, limit);
        EXPECT_TRUE(text.isEmpty());
      }
    }
  }
}
