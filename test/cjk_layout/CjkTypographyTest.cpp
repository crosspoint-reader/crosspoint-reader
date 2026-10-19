// CJK punctuation widths, line-edge widths, flush justification and the whole-em grid (spec sections 5.1, 5.2,
// 5.5), through the real ParsedText and TextBlock against the fixed-width shim of test/word_size_layout: every
// codepoint 10 px (so 一 is a 10 px em and every CJK mark is full-width), a space 5 px. The shim draws a CJK
// opening bracket's ink on the right half of its advance, a closing bracket, 。 and 、 on the left half.
#include <gtest/gtest.h>

#include <cstdlib>
#include <memory>
#include <new>
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

std::vector<Line> layout(ParsedText& text, const uint16_t width, const int fontId = kFontId) {
  GfxRenderer renderer;
  std::vector<Line> lines;
  text.layoutAndExtractLines(
      renderer, fontId, width,
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
  // 」 before 。 is half-width (all but the last of a run), 。 before text keeps its width.
  EXPECT_EQ(lines[0].xpos, (X{0, 10, 15, 25}));
}
TEST(CjkPunctuationWidths, ALatinWordKeepsItsCurlyQuotes) {
  ParsedText text(false, false, aligned(CssTextAlign::Left));
  text.addWord("\xE2\x80\x9CHello\xE2\x80\x9D", EpdFontFamily::REGULAR);  // “Hello”
  const auto lines = layout(text, 400);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0].words.size(), 1u);
}
TEST(CjkPunctuationWidths, ACurlyQuotePairInCjkTextSetsAQuarterEmEachSide) {
  // ”“ in Chinese dialogue, at 160 % (16 px glyphs; the shim draws curly quotes full-width): ” before an opening
  // mark and “ after a closing one are 6/8 each, 12 px. The “ judges ” by its full width: narrowed to 12 px, ” is
  // below the 4/5 em a curly quote needs to count as CJK punctuation.
  ParsedText text(false, false, aligned(CssTextAlign::Left));
  text.addWord("\xE5\xA5\xBD\xE2\x80\x9D\xE2\x80\x9C\xE5\xAD\x97", EpdFontFamily::REGULAR, false, false,
               160);  // 好”“字
  const auto lines = layout(text, 400);
  ASSERT_EQ(lines.size(), 1u);
  // “ keeps its ink (the right half): drawn 4 px left of its 12 px box at 28.
  EXPECT_EQ(lines[0].xpos, (X{0, 16, 24, 40}));
}
TEST(CjkPunctuationWidths, AMarkEndingALineIsHalfWidthSoTheLineHoldsIt) {
  // 字字字字。 is 45 px with the line-end 。 at half width: it fits a 45 px line. Measured at full width the line
  // would end before 字。.
  for (const bool hyphenation : {false, true}) {
    ParsedText text(false, hyphenation, aligned(CssTextAlign::Left));
    text.addWord("字字字字。字字", EpdFontFamily::REGULAR);
    EXPECT_EQ(wordsOf(layout(text, 45)), (Words{{"字", "字", "字", "字", "。"}, {"字", "字"}}))
        << (hyphenation ? "greedy breaker" : "DP breaker");
  }
}
TEST(CjkPunctuationWidths, AnOpeningBracketStartingALineIsHalfWidthAndDrawnByItsInk) {
  ParsedText text(false, false, aligned(CssTextAlign::Left));
  text.addWord("字字字字「字」", EpdFontFamily::REGULAR);
  const auto lines = layout(text, 40);
  ASSERT_EQ(wordsOf(lines), (Words{{"字", "字", "字", "字"}, {"「", "字", "」"}}));
  // 「 keeps only its ink (the right half of its em): drawn 5 px left of the line start, so the ink starts there.
  EXPECT_EQ(lines[1].xpos, (X{-5, 5, 15}));
}
TEST(CjkPunctuationWidths, TraditionalChineseKeepsItsCentredMarksFullWidth) {
  ParsedText text(false, false, aligned(CssTextAlign::Left));
  text.setTypography(TypographyProfile::forLanguage("zh-TW"));
  text.addWord("字」。次", EpdFontFamily::REGULAR);
  EXPECT_EQ(layout(text, 400)[0].xpos, (X{0, 10, 20, 30}));
}
TEST(CjkPunctuationWidths, JapaneseLetsAFullStopEndingALineStayFull) {
  // JLREQ: 。 at a line end is half or full, and it stays full unless push-in needs it (PR A3b), so 字。 does not
  // fit after 字字字.
  ParsedText text(false, false, aligned(CssTextAlign::Left));
  text.setTypography(TypographyProfile::forLanguage("ja"));
  text.addWord("字字字字。字字", EpdFontFamily::REGULAR);
  EXPECT_EQ(wordsOf(layout(text, 45)), (Words{{"字", "字", "字"}, {"字", "。", "字", "字"}}));
}
TEST(CjkPunctuationWidths, APageBottomRetryKeepsTheMarksWidths) {
  // The retry re-lays a hyphenated line with its words whole: it merges tokens back and measures again, so the
  // line-edge widths must stay in step with the tokens. The last line's 」 is half-width before 。 as without a retry.
  Hyphenator::setPreferredLanguage("ko");
  ParsedText text(false, /*hyphenationEnabled=*/true, aligned(CssTextAlign::Justify));
  for (const char* word : {"가나", "다라마바사아", "자」。차"}) text.addWord(word, EpdFontFamily::REGULAR);
  GfxRenderer renderer;
  std::vector<Line> lines;
  bool retried = false;
  text.layoutAndExtractLines(
      renderer, kFontId, 62,
      [&](std::unique_ptr<TextBlock> block, const bool endsHyphenated, const bool suppressRetry) {
        if (endsHyphenated && !suppressRetry && !retried) {  // what the parser does at a page bottom
          retried = true;
          return ParsedText::LineProcessResult::RetryWithoutHyphenation;
        }
        Line& line = lines.emplace_back();
        for (uint16_t i = 0; i < block->wordCount(); ++i) {
          line.words.emplace_back(block->wordText(i));
          line.xpos.push_back(block->wordXpos(i));
        }
        return ParsedText::LineProcessResult::Accepted;
      },
      /*includeLastLine=*/true);
  ASSERT_TRUE(retried);
  ASSERT_EQ(wordsOf(lines), (Words{{"가나"}, {"다라마바사아"}, {"자", "」", "。", "차"}}));
  EXPECT_EQ(lines[2].xpos, (X{0, 10, 15, 25}));
}

// Live allocations of this test binary, counted by the replacements of the global operator new and delete below.
namespace {
long gLiveAllocations = 0;
}
void* operator new(const std::size_t size) {
  if (void* p = std::malloc(size ? size : 1)) {
    ++gLiveAllocations;
    return p;
  }
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept {
  if (p != nullptr) --gLiveAllocations;
  std::free(p);
}
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }

TEST(CjkPunctuationWidths, ReleasingTheLayoutScratchGivesBackTheMarksScratchToo) {
  // An incremental build gives the layout scratch back when it yields a slice: a laid-out block of text with marks
  // then holds nothing more than before its first word.
  ParsedText text(false, false, aligned(CssTextAlign::Left));
  text.setTypography(TypographyProfile::forLanguage("zh"));
  const long before = gLiveAllocations;
  text.addWord("字」。次", EpdFontFamily::REGULAR);
  layout(text, 400);
  text.releaseLayoutScratch();
  EXPECT_EQ(gLiveAllocations, before);
}
TEST(CjkJustification, AJustifiedCjkLineEndsFlush) {
  // 4 characters on a 47 px line: 7 px over 3 gaps, 2 px each and the last pixel to one of them.
  ParsedText text(false, false, aligned(CssTextAlign::Justify));
  text.addWord("一二三四五六七八", EpdFontFamily::REGULAR);
  const auto lines = layout(text, 47);
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_EQ(lines[0].xpos.back() + 10, 47);
  EXPECT_EQ(lines[0].xpos, (X{0, 12, 24, 37}));
}
TEST(CjkJustification, LatinLinesKeepTheirRounding) {
  // aa bb cc on 81 px: 11 px over 2 word gaps, 5 each; the last pixel stays at the end as before.
  ParsedText text(false, false, aligned(CssTextAlign::Justify));
  for (const char* word : {"aa", "bb", "cc", "dd"}) text.addWord(word, EpdFontFamily::REGULAR);
  const auto lines = layout(text, 81);
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_EQ(lines[0].xpos, (X{0, 30, 60}));
}
TEST(CjkGrid, AJapaneseBookRoundsTheWidthDownToWholeEmsAndCentresIt) {
  // 47 px holds 4 ems of 10 px: the lines are 40 px wide, 3 px in from the left.
  ParsedText text(false, false, aligned(CssTextAlign::Left));
  text.setTypography(TypographyProfile::forLanguage("ja"));
  text.addWord("一二三四五", EpdFontFamily::REGULAR);
  const auto lines = layout(text, 47);
  EXPECT_EQ(wordsOf(lines), (Words{{"一", "二", "三", "四"}, {"五"}}));
  EXPECT_EQ(lines[0].xpos, (X{3, 13, 23, 33}));
  EXPECT_EQ(lines[1].xpos, (X{3}));
}
TEST(CjkGrid, AnIndentRoundsDownToWholeEmsAndNoIndentStaysNone) {
  for (const auto& [indent, firstX] : std::vector<std::pair<int16_t, int16_t>>{{15, 13}, {5, 13}, {0, 3}}) {
    ParsedText text(false, false, aligned(CssTextAlign::Left, indent));
    text.setTypography(TypographyProfile::forLanguage("zh"));
    text.addWord("一二三", EpdFontFamily::REGULAR);
    EXPECT_EQ(layout(text, 47)[0].xpos.front(), firstX) << "indent " << indent;
  }
}
TEST(CjkGrid, AFontWithoutCjkGlyphsGetsNoGridAndNoNarrowedMarks) {
  // A Latin reader font draws every CJK character as its replacement glyph, all at one advance. Measured by that
  // glyph, 一 would make a grid and the marks would pass as full-width; the font has no 一, so neither applies.
  ParsedText text(false, false, aligned(CssTextAlign::Left, 15));
  text.setTypography(TypographyProfile::forLanguage("ja"));
  text.addWord("字」。次", EpdFontFamily::REGULAR);
  const auto lines = layout(text, 47, GfxRenderer::LATIN_FONT);
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_EQ(lines[0].xpos, (X{15, 25, 35}));
  EXPECT_EQ(lines[1].xpos, (X{0}));
}
TEST(CjkGrid, OtherBooksHaveNoGrid) {
  ParsedText text(false, false, aligned(CssTextAlign::Left));
  text.addWord("一二三四五", EpdFontFamily::REGULAR);
  EXPECT_EQ(layout(text, 47)[0].xpos, (X{0, 10, 20, 30}));
}
TEST(CjkGrid, AHeadingsGridIsInItsOwnEms) {
  // A block at 1.2 times the body size has 12 px ems: 47 px holds 3 of them, 5 px in from the left.
  BlockStyle heading = aligned(CssTextAlign::Left);
  heading.fontSizeMultiplier = 1.2f;
  ParsedText text(false, false, heading);
  text.setTypography(TypographyProfile::forLanguage("ja"));
  text.addWord("一二三四", EpdFontFamily::REGULAR);
  const auto lines = layout(text, 47);
  EXPECT_EQ(wordsOf(lines), (Words{{"一", "二", "三"}, {"四"}}));
  EXPECT_EQ(lines[0].xpos, (X{5, 17, 29}));
}
TEST(CjkGrid, ABlockBesideAFloatKeepsItsOwnWidth) {
  BlockStyle beside = aligned(CssTextAlign::Left);
  beside.floatZoneCount = 1;
  beside.floatZones[0] = {0, 0, 0, false};  // a zone no line meets: the block is beside a float all the same
  ParsedText text(false, false, beside);
  text.setTypography(TypographyProfile::forLanguage("ja"));
  text.addWord("一二三四五", EpdFontFamily::REGULAR);
  EXPECT_EQ(layout(text, 47)[0].xpos, (X{0, 10, 20, 30}));
}
