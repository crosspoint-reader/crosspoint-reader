// CJK character classes (Utf8.h) and the CJK line-break rules (CjkLineBreak.h): where text without
// spaces may break, and the kinsoku pairs that must stay together.
#include <gtest/gtest.h>

#include <cstdlib>
#include <new>
#include <string>
#include <vector>

#include "Epub/CjkLineBreak.h"
#include "Epub/hyphenation/Hyphenator.h"
#include "Utf8.h"

TEST(CjkClasses, HanKanaHangulPunctuationAndFullwidthAreBreakable) {
  for (const uint32_t cp : {0x6F22u, 0x3042u, 0x30A2u, 0x3002u, 0xD55Cu, 0xFF21u, 0x20000u}) {
    EXPECT_TRUE(utf8IsCjkBreakable(cp)) << std::hex << cp;
  }
  for (const uint32_t cp : {uint32_t{'a'}, 0x00E9u, 0x2014u, 0x0410u}) {
    EXPECT_FALSE(utf8IsCjkBreakable(cp)) << std::hex << cp;
  }
}

TEST(CjkClasses, HangulIsBreakableButNotUnspaced) {
  EXPECT_TRUE(utf8IsHangul(0xD55C));   // 한
  EXPECT_TRUE(utf8IsHangul(0x3131));   // ㄱ
  EXPECT_FALSE(utf8IsHangul(0x6F22));  // 漢
  EXPECT_FALSE(utf8IsUnspacedCjk(0xD55C));
  for (const uint32_t cp : {0x6F22u, 0x3042u, 0x30A2u, 0x3002u, 0xFF0Cu}) {
    EXPECT_TRUE(utf8IsUnspacedCjk(cp)) << std::hex << cp;
  }
  EXPECT_FALSE(utf8IsUnspacedCjk('a'));
}

TEST(CjkKinsoku, ClosingPunctuationAndIterationMarksNeverStartALine) {
  // 。、」）！ from upstream; 々・〜‼｡﹐ added from crengine.
  for (const uint32_t cp : {0x3002u, 0x3001u, 0x300Du, 0xFF09u, 0xFF01u, 0x3005u, 0x30FBu, 0x301Cu, 0x203Cu, 0xFF61u,
                            0xFE50u, 0x2026u, 0x2014u}) {
    EXPECT_TRUE(CjkLineBreak::isNoLineStart(cp)) << std::hex << cp;
  }
}

TEST(CjkKinsoku, SmallKanaAndTheProlongedSoundMarkMayStartALine) {
  for (const uint32_t cp : {0x3041u, 0x3063u, 0x30C3u, 0x30E3u, 0x30FCu, 0x6F22u}) {  // ぁ っ ッ ャ ー 漢
    EXPECT_FALSE(CjkLineBreak::isNoLineStart(cp)) << std::hex << cp;
  }
}

TEST(CjkKinsoku, OpeningBracketsAndQuotesNeverEndALine) {
  for (const uint32_t cp : {0x300Cu, 0xFF08u, 0x201Cu, 0x301Du, 0xFF62u, 0x201Eu}) {  // 「 （ “ 〝 ｢ „
    EXPECT_TRUE(CjkLineBreak::isNoLineEnd(cp)) << std::hex << cp;
  }
  EXPECT_FALSE(CjkLineBreak::isNoLineEnd(0x300D));  // 」
}

TEST(CjkKinsoku, StraightQuotesNeverBreakOnEitherSide) {
  // " ' ＂ ＇ may open or close a quotation, so neither side of one is a break (UAX #14 LB19).
  for (const uint32_t cp : {uint32_t{'"'}, uint32_t{'\''}, 0xFF02u, 0xFF07u}) {
    EXPECT_TRUE(CjkLineBreak::isNoLineStart(cp)) << std::hex << cp;
    EXPECT_TRUE(CjkLineBreak::isNoLineEnd(cp)) << std::hex << cp;
  }
}

TEST(CjkKinsoku, TheMiddleDotAndTheIdeographicSpaceNeverStartALine) {
  EXPECT_TRUE(CjkLineBreak::isNoLineStart(0x00B7));  // · in transliterated names: 列夫·托尔斯泰
  EXPECT_TRUE(CjkLineBreak::isNoLineStart(0x3000));  // 　 (UAX #14 BA): ！　 stays together
}

TEST(CjkBreakOffsets, AStraightQuoteStaysWithTheCharactersOnBothSides) {
  // 而|借"通|靈"之|說: no break on either side of either quote.
  EXPECT_EQ(CjkLineBreak::breakByteOffsets("而借\"通靈\"之說"), (std::vector<size_t>{3, 10, 17}));
}

TEST(CjkBreakOffsets, TheIdeographicSpaceStaysWithTheCharacterBeforeIt) {
  EXPECT_EQ(CjkLineBreak::breakByteOffsets("四！　彼"), (std::vector<size_t>{9}));
}

TEST(CjkBreakOffsets, EveryHanCharacterIsABreak) {
  EXPECT_EQ(CjkLineBreak::breakByteOffsets("漢字です"), (std::vector<size_t>{3, 6, 9}));
}

TEST(CjkBreakOffsets, KinsokuPairsStayTogether) {
  // 字。 and 「漢 never split; 漢|字 inside the brackets still may.
  EXPECT_EQ(CjkLineBreak::breakByteOffsets("漢字。"), (std::vector<size_t>{3}));
  EXPECT_EQ(CjkLineBreak::breakByteOffsets("「漢字」"), (std::vector<size_t>{6}));
}

TEST(CjkBreakOffsets, ARunOfEllipsesOrDashesNeverSplits) {
  // 漢|…… no (… may not start a line), …|… no, ……|字 yes (a break after a dash or ellipsis).
  EXPECT_EQ(CjkLineBreak::breakByteOffsets("漢……字"), (std::vector<size_t>{9}));
  EXPECT_EQ(CjkLineBreak::breakByteOffsets("漢——字"), (std::vector<size_t>{9}));
}

TEST(CjkBreakOffsets, SmallKanaAndTheProlongedSoundMarkAreBreaks) {
  EXPECT_EQ(CjkLineBreak::breakByteOffsets("きゃく"), (std::vector<size_t>{3, 6}));
  EXPECT_EQ(CjkLineBreak::breakByteOffsets("ラーメン"), (std::vector<size_t>{3, 6, 9}));
}

TEST(CjkBreakOffsets, LatinRunsStayWholeButMeetCjkAtABreak) {
  EXPECT_TRUE(CjkLineBreak::breakByteOffsets("Latin").empty());
  EXPECT_TRUE(CjkLineBreak::breakByteOffsets("漢").empty());
  EXPECT_EQ(CjkLineBreak::breakByteOffsets("abc漢字"), (std::vector<size_t>{3, 6}));
}

TEST(CjkBreakOffsets, HangulWordsStayWhole) {
  EXPECT_TRUE(CjkLineBreak::breakByteOffsets("한국어").empty());
  EXPECT_EQ(CjkLineBreak::breakByteOffsets("漢字한글"), (std::vector<size_t>{3}));  // nothing touching Hangul
}

TEST(CjkGap, OnlyBoundariesTouchingCjkAndNoHangulAreGaps) {
  EXPECT_TRUE(CjkLineBreak::isGapBetween(0x6F22, 0x5B57));   // 漢|字
  EXPECT_TRUE(CjkLineBreak::isGapBetween('c', 0x6F22));      // c|漢
  EXPECT_FALSE(CjkLineBreak::isGapBetween('a', 'b'));        // Latin
  EXPECT_FALSE(CjkLineBreak::isGapBetween(0xD55C, 0xAD6D));  // 한|국
  EXPECT_FALSE(CjkLineBreak::isGapBetween(0x5B57, 0x3002));  // 字|。
  EXPECT_FALSE(CjkLineBreak::isGapBetween(0x6F22, 0x0301));  // a combining mark stays with its base
}

TEST(HangulLineEndBreaks, BetweenSyllablesWithoutAHyphen) {
  const auto breaks = CjkLineBreak::hangulLineEndBreaks("다라마바");
  ASSERT_EQ(breaks.size(), 3u);
  for (size_t i = 0; i < breaks.size(); ++i) {
    EXPECT_EQ(breaks[i].byteOffset, 3 * (i + 1));
    EXPECT_FALSE(breaks[i].requiresInsertedHyphen);
  }
}

TEST(HangulLineEndBreaks, NeverBeforeClosingPunctuationButAtDigitsAndBrackets) {
  const auto offsetsOf = [](const std::string& word) {
    std::vector<size_t> offsets;
    for (const auto& b : CjkLineBreak::hangulLineEndBreaks(word)) offsets.push_back(b.byteOffset);
    return offsets;
  };
  EXPECT_EQ(offsetsOf("사."), (std::vector<size_t>{}));
  EXPECT_EQ(offsetsOf("12월"), (std::vector<size_t>{2}));
  EXPECT_EQ(offsetsOf("소신(ab)이"), (std::vector<size_t>{3, 6, 10}));  // 소|신|(ab)|이
}

// Hyphenator::breakOffsets offers fallback breaks inside a word that has no pattern breaks. Next to
// a CJK character such a break draws no hyphen: CJK scripts are not hyphenated.
TEST(CjkHyphenator, FallbackBreaksNextToCjkDrawNoHyphen) {
  Hyphenator::setPreferredLanguage("ja");
  const auto breaks = Hyphenator::breakOffsets("漢字仮名交", /*includeFallback=*/true);
  ASSERT_FALSE(breaks.empty());
  for (const auto& b : breaks) EXPECT_FALSE(b.requiresInsertedHyphen) << "offset " << b.byteOffset;
}

TEST(CjkHyphenator, LatinFallbackBreaksStillDrawAHyphen) {
  Hyphenator::setPreferredLanguage("ja");
  const auto breaks = Hyphenator::breakOffsets("abcdefgh", /*includeFallback=*/true);
  ASSERT_FALSE(breaks.empty());
  for (const auto& b : breaks) EXPECT_TRUE(b.requiresInsertedHyphen) << "offset " << b.byteOffset;
}

namespace {
size_t gAllocations = 0;
}

void* operator new(std::size_t size) {
  ++gAllocations;
  if (void* p = std::malloc(size ? size : 1)) return p;
  std::abort();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

TEST(CjkBreakOffsets, ALatinWordAllocatesNothing) {
  const std::string hello = "hello";
  const std::string longWord(200, 'a');
  const size_t before = gAllocations;
  const auto a = CjkLineBreak::breakByteOffsets(hello);
  const auto b = CjkLineBreak::breakByteOffsets(longWord);
  const size_t after = gAllocations;
  EXPECT_TRUE(a.empty());
  EXPECT_TRUE(b.empty());
  EXPECT_EQ(after - before, 0u);
}
