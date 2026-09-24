#include <gtest/gtest.h>

#include <iomanip>

#include "CjkLineBreak.h"

using CjkLineBreak::hasCjkBreakOpportunityBetween;

namespace {
constexpr uint32_t KANJI = 0x6F22;     // 漢
constexpr uint32_t HIRAGANA = 0x3042;  // あ
constexpr uint32_t HANGUL = 0xAC00;    // 가
}  // namespace

TEST(CjkLineBreak, PlainIdeographsMayBreakBetweenEachOther) {
  EXPECT_TRUE(hasCjkBreakOpportunityBetween(KANJI, KANJI));
  EXPECT_TRUE(hasCjkBreakOpportunityBetween(KANJI, HIRAGANA));
  EXPECT_TRUE(hasCjkBreakOpportunityBetween(HANGUL, HANGUL));
}

TEST(CjkLineBreak, LatinPairsAreNotCjkOpportunities) {
  EXPECT_FALSE(hasCjkBreakOpportunityBetween('a', 'b'));
  EXPECT_FALSE(hasCjkBreakOpportunityBetween('5', 0x00B0));  // 5° stays a Latin word
}

// JLREQ 3.1.7: characters that cannot start a line stay attached to what precedes them.
TEST(CjkLineBreak, LineStartProhibitedCharactersStayWithThePrecedingCharacter) {
  constexpr uint32_t cps[] = {
      0x3001, 0x3002, 0x300D, 0xFF09,  // 、。」）        (cl-06/07, cl-02)
      0x30FC, 0xFF70,                  // ー ｰ            (cl-10 prolonged sound mark)
      0x30FB, 0xFF65,                  // ・ ･            (cl-05 middle dots)
      0x3005, 0x309D, 0x30FD, 0x303B,  // 々 ゝ ヽ 〻      (cl-09 iteration marks)
      0x2010, 0x2013, 0x301C, 0xFF5E,  // ‐ – 〜 ～       (cl-03 hyphens)
      0x2026, 0x2025, 0x2015,          // … ‥ ―          (cl-08 inseparable)
      0xFF05, 0x2103, 0x2032, 0x00B0,  // ％ ℃ ′ °       (cl-13 postfixed abbreviations)
      0x301F, 0xFF61, 0xFF64,          // 〟 ｡ ､
  };
  for (const uint32_t cp : cps) {
    EXPECT_FALSE(hasCjkBreakOpportunityBetween(KANJI, cp)) << "U+" << std::hex << cp << " may not start a line";
    EXPECT_FALSE(hasCjkBreakOpportunityBetween(HIRAGANA, cp)) << "U+" << std::hex << cp << " may not start a line";
  }
}

// JLREQ 3.1.7: characters that cannot end a line stay attached to what follows them.
TEST(CjkLineBreak, LineEndProhibitedCharactersStayWithTheFollowingCharacter) {
  constexpr uint32_t cps[] = {
      0x300C, 0x300E, 0xFF08, 0x3008,  // 「『（〈          (cl-01)
      0x301D, 0xFF5F,                  // 〝 ｟
      0x00A5, 0xFFE5, 0x2116, 0xFF04,  // ¥ ￥ № ＄        (cl-12 prefixed abbreviations)
  };
  for (const uint32_t cp : cps) {
    EXPECT_FALSE(hasCjkBreakOpportunityBetween(cp, KANJI)) << "U+" << std::hex << cp << " may not end a line";
  }
}

// A prohibited character still releases the character after it: 「漢 glues, 漢」glues, but 」漢 breaks.
TEST(CjkLineBreak, ProhibitionsAreOneSided) {
  EXPECT_TRUE(hasCjkBreakOpportunityBetween(0x300D, KANJI));   // 」漢
  EXPECT_TRUE(hasCjkBreakOpportunityBetween(0x30FC, KANJI));   // ー漢
  EXPECT_TRUE(hasCjkBreakOpportunityBetween(KANJI, 0x300C));   // 漢「
  EXPECT_TRUE(hasCjkBreakOpportunityBetween(0x3002, 0x300C));  // 。「
}

// Nested brackets chain: every pair in 『「漢」』 is glued, so the run never splits.
TEST(CjkLineBreak, NestedBracketsChainThroughGluedPairs) {
  EXPECT_FALSE(hasCjkBreakOpportunityBetween(0x300E, 0x300C));  // 『「
  EXPECT_FALSE(hasCjkBreakOpportunityBetween(0x300C, KANJI));   // 「漢
  EXPECT_FALSE(hasCjkBreakOpportunityBetween(KANJI, 0x300D));   // 漢」
  EXPECT_FALSE(hasCjkBreakOpportunityBetween(0x300D, 0x300F));  // 」』
  EXPECT_FALSE(hasCjkBreakOpportunityBetween(0x300F, 0x3002));  // 』。
}

// cl-08: a run of the same leader is one mark. It may still break after the run ends.
TEST(CjkLineBreak, InseparableRunsStayTogetherButReleaseWhatFollows) {
  EXPECT_FALSE(hasCjkBreakOpportunityBetween(0x2026, 0x2026));  // ……
  EXPECT_FALSE(hasCjkBreakOpportunityBetween(0x2015, 0x2015));  // ――
  EXPECT_FALSE(hasCjkBreakOpportunityBetween(KANJI, 0x2026));   // 漢…
  EXPECT_TRUE(hasCjkBreakOpportunityBetween(0x2026, KANJI));    // …漢
  // 〳〴〵 sit inside the CJK Symbols block, so the pair rule is what keeps them together;
  // … and ― are General Punctuation and already stay glued by the block test above.
  EXPECT_FALSE(hasCjkBreakOpportunityBetween(0x3033, 0x3033));  // 〳〳
  EXPECT_TRUE(hasCjkBreakOpportunityBetween(0x3033, KANJI));    // 〳漢
}

// cl-11 small kana are in the 3.1.7 rule itself (the Note records a relaxed practice that lets
// them start a line; the principle does not).
TEST(CjkLineBreak, SmallKanaMayNotStartALine) {
  constexpr uint32_t cps[] = {0x3063, 0x3083, 0x30A1, 0x30C3, 0x30F5, 0x31F0};  // っ ゃ ァ ッ ヵ ㇰ
  for (const uint32_t cp : cps) {
    EXPECT_FALSE(hasCjkBreakOpportunityBetween(KANJI, cp)) << "U+" << std::hex << cp;
    EXPECT_FALSE(hasCjkBreakOpportunityBetween(HIRAGANA, cp)) << "U+" << std::hex << cp;
  }
  EXPECT_TRUE(hasCjkBreakOpportunityBetween(0x3063, KANJI));  // っ漢: still releases what follows
}

// cl-04 dividing punctuation beyond ! and ?, cl-12/13 abbreviations from Appendix A.
TEST(CjkLineBreak, AppendixAMembersBeyondTheAsciiForms) {
  EXPECT_FALSE(hasCjkBreakOpportunityBetween(KANJI, 0x203C));  // 漢‼
  EXPECT_FALSE(hasCjkBreakOpportunityBetween(KANJI, 0x2049));  // 漢⁉
  EXPECT_FALSE(hasCjkBreakOpportunityBetween(KANJI, 0x00A2));  // 漢¢
  EXPECT_FALSE(hasCjkBreakOpportunityBetween(0xFF04, KANJI));  // ＄漢
  EXPECT_TRUE(hasCjkBreakOpportunityBetween(KANJI, 0x3012));   // 漢〒: not a JLREQ prefix class
}
