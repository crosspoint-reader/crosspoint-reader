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
#include "Epub/hyphenation/Hyphenator.h"
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
  // 。 is glued to 五, so the line ends before 五 rather than leave 。 at the start of the next.
  ParsedText text(false, false, leftAligned());
  text.addWord("一二三四五。六", EpdFontFamily::REGULAR);
  EXPECT_EQ(wordsOf(layout(text, 50)),
            (std::vector<std::vector<std::string>>{{"一", "二", "三", "四"}, {"五", "。", "六"}}));
}

TEST(CjkKinsoku, AnOpeningBracketNeverEndsALine) {
  ParsedText text(false, false, leftAligned());
  text.addWord("一二三四「五」", EpdFontFamily::REGULAR);
  EXPECT_EQ(wordsOf(layout(text, 50)), (std::vector<std::vector<std::string>>{{"一", "二", "三", "四"}, {"「", "五", "」"}}));
}

namespace {

// True when a line the layout broke ends with `character`, or the line after it starts with it.
bool aBreakFallsNextTo(const std::vector<Line>& lines, const std::string& character) {
  for (size_t i = 0; i + 1 < lines.size(); ++i) {
    const std::string& end = lines[i].words.back();
    const std::string& start = lines[i + 1].words.front();
    if (end.size() >= character.size() &&
        end.compare(end.size() - character.size(), character.size(), character) == 0) {
      return true;
    }
    if (start.compare(0, character.size(), character) == 0) return true;
  }
  return false;
}

}  // namespace

TEST(CjkKinsoku, NoLineBreakFallsNextToAStraightQuote) {
  // Unbroken, 一二三四"五 would end the first line with the opening quote.
  ParsedText text(false, false, leftAligned());
  text.addWord("一二三四\"五六\"", EpdFontFamily::REGULAR);
  const auto lines = layout(text, 50);
  ASSERT_GT(lines.size(), 1u);
  EXPECT_FALSE(aBreakFallsNextTo(lines, "\"")) << testing::PrintToString(wordsOf(lines));

  // Project Gutenberg Chinese quotes with ASCII quotes: 而借"通靈"之說 at three characters a line.
  ParsedText pg(false, false, leftAligned());
  pg.addWord("而借\"通靈\"之說", EpdFontFamily::REGULAR);
  const auto pgLines = layout(pg, 30);
  ASSERT_GT(pgLines.size(), 1u);
  EXPECT_FALSE(aBreakFallsNextTo(pgLines, "\"")) << testing::PrintToString(wordsOf(pgLines));
}

TEST(CjkKinsoku, NoLineStartsWithAnIdeographicSpace) {
  // ！　 is how Japanese fiction follows an exclamation; a line starting with 　 reads as a new paragraph.
  ParsedText text(false, false, leftAligned());
  text.addWord("一二三四！　彼女は", EpdFontFamily::REGULAR);
  const auto lines = layout(text, 50);
  ASSERT_GT(lines.size(), 1u);
  for (size_t i = 1; i < lines.size(); ++i) {
    EXPECT_NE(lines[i].words.front().rfind("　", 0), 0u) << testing::PrintToString(wordsOf(lines));
  }
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

TEST(CjkSpaces, ANoBreakSpaceNextToCjkStaysGlued) {
  // The parser turns U+00A0 into a glued " " token: no break on either side of it.
  ParsedText text(false, false, leftAligned());
  text.addWord("漢字", EpdFontFamily::REGULAR);
  text.addWord(" ", EpdFontFamily::REGULAR, false, /*attachToPrevious=*/true);
  text.addWord("仮名", EpdFontFamily::REGULAR, false, /*attachToPrevious=*/true);
  const auto lines = layout(text, 25);
  EXPECT_EQ(wordsOf(lines), (std::vector<std::vector<std::string>>{{"漢"}, {"字", " ", "仮"}, {"名"}}));
}

// Korean. Ported from crosspoint-reader's KoreanLineBreaking tests (PR #3700, Sung-jin Brian Hong) and
// KoreanLayout tests (PRs #2288 and #3700), with the widths recomputed for this shim (10 px per
// glyph, 5 px per space; upstream's is 8 and 4).
namespace {

BlockStyle justified() {
  BlockStyle bs = leftAligned();
  bs.alignment = CssTextAlign::Justify;
  return bs;
}

std::vector<Line> layoutWords(const std::vector<const char*>& words, const bool hyphenation, const uint16_t width,
                              const BlockStyle& style = justified()) {
  Hyphenator::setPreferredLanguage("ko");
  ParsedText text(false, hyphenation, style);
  for (const char* word : words) text.addWord(word, EpdFontFamily::REGULAR);
  return layout(text, width);
}

}  // namespace

TEST(KoreanLineBreaking, HyphenationOffWrapsAtSpacesOnly) {
  // 가나 다라마바 needs 65 px; with hyphenation off the Hangul word moves down whole.
  EXPECT_EQ(wordsOf(layoutWords({"가나", "다라마바", "사"}, false, 62)),
            (std::vector<std::vector<std::string>>{{"가나"}, {"다라마바", "사"}}));
}

TEST(KoreanLineBreaking, HyphenationOnSplitsBetweenSyllablesWithoutHyphen) {
  // 가나 + space leaves 37 px: the widest syllable prefix that fits is 다라마 (30 px).
  const auto lines = layoutWords({"가나", "다라마바사아", "자"}, true, 62);
  ASSERT_EQ(wordsOf(lines), (std::vector<std::vector<std::string>>{{"가나", "다라마"}, {"바사아", "자"}}));
  // 20 + 5 + 30 = 55 px leaves 7 px, all of it on the one word space.
  EXPECT_EQ(lines[0].xpos, (std::vector<int16_t>{0, 32}));
}

TEST(KoreanLineBreaking, HyphenationOnKeepsTrailingPunctuationWithSyllable) {
  // 가나다 + space leaves 45 px. 라마바사 (40 px) would fit, but 사. stays together, so 라마바 is used.
  EXPECT_EQ(wordsOf(layoutWords({"가나다", "라마바사.", "자"}, true, 80)),
            (std::vector<std::vector<std::string>>{{"가나다", "라마바"}, {"사.", "자"}}));
}

TEST(KoreanLineBreaking, HyphenationOnSplitsShortWords) {
  // 가나다 + space leaves 25 px, so the three-syllable word splits after 라마 (20 px).
  EXPECT_EQ(wordsOf(layoutWords({"가나다", "라마바", "자"}, true, 60)),
            (std::vector<std::vector<std::string>>{{"가나다", "라마"}, {"바", "자"}}));
}

TEST(KoreanLineBreaking, HyphenationOnSplitsWhereHangulMeetsOtherScriptsOrBrackets) {
  // 가 + space leaves 65 px. 소신(ab) (60 px) fits; the split may fall before "(" or after ")",
  // never inside the brackets.
  EXPECT_EQ(wordsOf(layoutWords({"가", "소신(ab)이"}, true, 80)),
            (std::vector<std::vector<std::string>>{{"가", "소신(ab)"}, {"이"}}));
}

TEST(KoreanLineBreaking, HyphenationOnSplitsBetweenDigitAndHangul) {
  // 가나다 + space leaves 25 px: 12 (20 px) fits, 12월 (30 px) does not.
  EXPECT_EQ(wordsOf(layoutWords({"가나다", "12월부터", "자"}, true, 60)),
            (std::vector<std::vector<std::string>>{{"가나다", "12"}, {"월부터", "자"}}));
}

TEST(KoreanLayout, HangulWordsStayWholeAndWrapAtSpaces) {
  // 가나다 라마 is 55 px; adding 3개입니다 would need 110 px, and no break exists inside it. (Upstream's
  // 3개를 ties two layouts under our line breaker, which keeps the first it finds.)
  EXPECT_EQ(wordsOf(layoutWords({"가나다", "라마", "3개입니다", "iPhone을"}, false, 75, leftAligned())),
            (std::vector<std::vector<std::string>>{{"가나다", "라마"}, {"3개입니다"}, {"iPhone을"}}));
}

TEST(KoreanLayout, JustifiedHangulStretchesOnlyWordSpaces) {
  // 3 x 20 px words + 2 x 5 px spaces leave 4 px, split across the two spaces only.
  const auto lines = layoutWords({"가나", "다라", "마바", "사아"}, false, 74);
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_EQ(lines[0].xpos, (std::vector<int16_t>{0, 27, 54}));
}

TEST(KoreanLayout, HangulGluedAcrossInlineStyleIsUnbreakable) {
  ParsedText text(false, false, justified());
  text.addWord("가나", EpdFontFamily::REGULAR);
  text.addWord("한국", EpdFontFamily::REGULAR);
  text.addWord("어", EpdFontFamily::BOLD, false, /*attachToPrevious=*/true);
  // 가나 한국 fits in 50 px, but 어 is glued to 한국, so the whole word moves down.
  EXPECT_EQ(wordsOf(layout(text, 50)), (std::vector<std::vector<std::string>>{{"가나"}, {"한국", "어"}}));
}

// Ours: a Hangul split is a hyphenated line end, so at a page bottom the parser's retry re-lays the
// line with the word whole, as it does for a Latin hyphenation.
TEST(KoreanLayout, PageBottomRetryKeepsTheHangulWordWhole) {
  Hyphenator::setPreferredLanguage("ko");
  ParsedText text(false, /*hyphenationEnabled=*/true, justified());
  for (const char* word : {"가나", "다라마바사아", "자"}) text.addWord(word, EpdFontFamily::REGULAR);
  GfxRenderer renderer;
  std::vector<std::vector<std::string>> lines;
  bool retried = false;
  text.layoutAndExtractLines(
      renderer, kFontId, 62,
      [&](std::unique_ptr<TextBlock> line, const bool endsHyphenated, const bool suppressRetry) {
        if (endsHyphenated && !suppressRetry && !retried) {  // what the parser does at a page bottom
          retried = true;
          return ParsedText::LineProcessResult::RetryWithoutHyphenation;
        }
        std::vector<std::string> words;
        for (uint16_t i = 0; i < line->wordCount(); i++) words.emplace_back(line->wordText(i));
        lines.push_back(std::move(words));
        return ParsedText::LineProcessResult::Accepted;
      },
      /*includeLastLine=*/true);
  ASSERT_TRUE(retried);
  // The retried line takes 가나 alone; the word moves down whole and fills the next line (60 of 62 px).
  EXPECT_EQ(lines, (std::vector<std::vector<std::string>>{{"가나"}, {"다라마바사아"}, {"자"}}));
}

// Ours: with hyphenation off, a Hangul word wider than the whole line still splits between syllables,
// without a hyphen, rather than overflow the line.
TEST(KoreanLayout, AWordWiderThanTheLineSplitsBetweenSyllables) {
  const auto lines = layoutWords({"다라마바사아"}, false, 35);
  EXPECT_EQ(wordsOf(lines), (std::vector<std::vector<std::string>>{{"다라마"}, {"바사아"}}));
}
