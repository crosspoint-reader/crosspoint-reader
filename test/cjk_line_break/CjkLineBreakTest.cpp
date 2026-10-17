// CJK character classes (Utf8.h) and the CJK line-break rules (CjkLineBreak.h): where text without
// spaces may break, and the kinsoku pairs that must stay together.
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "Epub/CjkLineBreak.h"
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
