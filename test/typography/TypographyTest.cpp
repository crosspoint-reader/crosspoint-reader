// The typography profile a book's language picks (spec section 3), and the punctuation widths of each language's
// table (spec section 5.2, from JLREQ and CLREQ).
#include <gtest/gtest.h>

#include "Epub/CjkPunctuation.h"
#include "Epub/TypographyProfile.h"

namespace {

using Table = TypographyProfile::Punctuation;
using CjkPunctuation::Class;

TEST(TypographyProfile, ChineseAndJapanesePickTheirTablesTheGridAndTheGap) {
  const TypographyProfile ja = TypographyProfile::forLanguage("ja");
  EXPECT_EQ(ja.punctuation, Table::Japanese);
  EXPECT_EQ(ja.breaking, TypographyProfile::Breaking::BetweenCharacters);
  EXPECT_EQ(ja.justify, TypographyProfile::Justify::BetweenCharacters);
  EXPECT_TRUE(ja.wholeEmGrid);
  EXPECT_TRUE(ja.cjkLatinGap);
  EXPECT_TRUE(ja.verticalAllowed);

  EXPECT_EQ(TypographyProfile::forLanguage("zh").punctuation, Table::SimplifiedChinese);
  EXPECT_EQ(TypographyProfile::forLanguage("zh-CN").punctuation, Table::SimplifiedChinese);
  EXPECT_TRUE(TypographyProfile::forLanguage("zh-CN").wholeEmGrid);
}

TEST(TypographyProfile, TraditionalChineseByScriptRegionOrClassicalChinese) {
  for (const char* tag : {"zh-Hant", "zh-TW", "zh_HK", "ZH-mo", "zh-Hant-CN", "lzh"}) {
    EXPECT_EQ(TypographyProfile::forLanguage(tag).punctuation, Table::TraditionalChinese) << tag;
  }
  // An explicit script subtag wins over the region.
  EXPECT_EQ(TypographyProfile::forLanguage("zh-Hans-TW").punctuation, Table::SimplifiedChinese);
}

TEST(TypographyProfile, KoreanKeepsWholeWordsWithoutGridOrGap) {
  const TypographyProfile ko = TypographyProfile::forLanguage("ko-KR");
  EXPECT_EQ(ko.breaking, TypographyProfile::Breaking::KoreanWords);
  EXPECT_EQ(ko.justify, TypographyProfile::Justify::WordSpaces);
  EXPECT_EQ(ko.punctuation, Table::SimplifiedChinese);
  EXPECT_FALSE(ko.wholeEmGrid);
  EXPECT_FALSE(ko.cjkLatinGap);
  EXPECT_FALSE(ko.verticalAllowed);
}

TEST(TypographyProfile, OtherLanguagesKeepLatinRulesAndTheirHyphenation) {
  const TypographyProfile en = TypographyProfile::forLanguage("en-US");
  EXPECT_EQ(en.breaking, TypographyProfile::Breaking::AtSpaces);
  EXPECT_EQ(en.punctuation, Table::SimplifiedChinese) << "CJK punctuation in any book";
  EXPECT_FALSE(en.wholeEmGrid);
  EXPECT_FALSE(en.cjkLatinGap);
  EXPECT_NE(en.hyphenator, nullptr);
  EXPECT_EQ(TypographyProfile::forLanguage("").hyphenator, nullptr);
  EXPECT_FALSE(TypographyProfile::forLanguage("zhx").wholeEmGrid) << "only the primary subtag zh is Chinese";
}

TEST(TypographyProfile, EveryLanguageRunsLeftToRightThisRound) {
  for (const char* tag : {"en", "ja", "zh-Hant", "ko", "ar", "he"}) {
    EXPECT_EQ(TypographyProfile::forLanguage(tag).direction, TypographyProfile::Direction::LeftToRight) << tag;
  }
}

TEST(CjkPunctuation, ClassesFollowJlreq) {
  EXPECT_EQ(CjkPunctuation::classOf(0x300C), Class::Opening);           // 「
  EXPECT_EQ(CjkPunctuation::classOf(0x300D), Class::Closing);           // 」
  EXPECT_EQ(CjkPunctuation::classOf(0xFF01), Class::Dividing);          // ！
  EXPECT_EQ(CjkPunctuation::classOf(0x30FB), Class::MiddleDot);         // ・
  EXPECT_EQ(CjkPunctuation::classOf(0x3002), Class::FullStop);          // 。
  EXPECT_EQ(CjkPunctuation::classOf(0x3001), Class::Comma);             // 、
  EXPECT_EQ(CjkPunctuation::classOf(0x3000), Class::IdeographicSpace);  // U+3000
  EXPECT_EQ(CjkPunctuation::classOf(0xFF02), Class::AmbiguousQuote);    // ＂
  EXPECT_EQ(CjkPunctuation::classOf(0x201C), Class::Opening);           // “
  EXPECT_EQ(CjkPunctuation::classOf(0x6F22), Class::None);              // 漢
  EXPECT_EQ(CjkPunctuation::classOf('A'), Class::None);
  EXPECT_TRUE(CjkPunctuation::isCurlyQuote(0x2019));
  EXPECT_FALSE(CjkPunctuation::isCurlyQuote(0x300D));
}

// Widths in eighths of the mark's advance: {forced, minimum}.
void expectWidth(const Table table, const Class mark, const Class neighbour, const bool edge, const int forced,
                 const int minimum) {
  const CjkPunctuation::Width w = CjkPunctuation::widthOf(table, mark, neighbour, edge);
  EXPECT_EQ(w.forced, forced);
  EXPECT_EQ(w.minimum, minimum);
}

TEST(CjkPunctuation, SimplifiedChineseHalvesMarksAtLineEdgesAndInRuns) {
  const Table sc = Table::SimplifiedChinese;
  expectWidth(sc, Class::Closing, Class::None, true, 4, 4);       // 」 ending a line
  expectWidth(sc, Class::Opening, Class::None, true, 4, 4);       // 「 starting a line
  expectWidth(sc, Class::Closing, Class::FullStop, false, 4, 4);  // 」 in 」。: all but the last of a run
  expectWidth(sc, Class::FullStop, Class::None, false, 8, 4);     // 。 before text: full, may halve
  expectWidth(sc, Class::Opening, Class::Opening, false, 4, 4);   // the second 「 of 「「
  expectWidth(sc, Class::Opening, Class::None, false, 8, 4);      // 「 after text
  expectWidth(sc, Class::Closing, Class::Opening, false, 6, 6);   // 」 in 」「: a quarter em each side
  expectWidth(sc, Class::Opening, Class::Closing, false, 6, 6);   // 「 in 」「
  expectWidth(sc, Class::IdeographicSpace, Class::None, true, 8, 8);
}

TEST(CjkPunctuation, TraditionalChineseKeepsCentredMarksFullWidth) {
  const Table tc = Table::TraditionalChinese;
  expectWidth(tc, Class::FullStop, Class::None, true, 8, 6);      // 。 at a line end stays full
  expectWidth(tc, Class::Closing, Class::None, true, 8, 4);       // a bracket may halve on demand
  expectWidth(tc, Class::Closing, Class::FullStop, false, 8, 4);  // no forced run rule
  expectWidth(tc, Class::Comma, Class::None, false, 8, 6);
}

TEST(CjkPunctuation, JapaneseKeepsTheFullStopAndLetsLineEndsChoose) {
  const Table ja = Table::Japanese;
  expectWidth(ja, Class::FullStop, Class::None, false, 8, 6);     // 。 keeps its blank mid-line
  expectWidth(ja, Class::FullStop, Class::Closing, false, 4, 4);  // 。」: solid before the bracket
  expectWidth(ja, Class::FullStop, Class::None, true, 8, 4);      // half or full at a line end
  expectWidth(ja, Class::Comma, Class::None, true, 8, 4);
  expectWidth(ja, Class::Closing, Class::None, true, 8, 4);
  expectWidth(ja, Class::Opening, Class::None, true, 4, 4);       // a line start sets solid
  expectWidth(ja, Class::Opening, Class::FullStop, false, 4, 4);  // 。「: solid after 。
  expectWidth(ja, Class::Closing, Class::Opening, false, 6, 6);   // 」「
  expectWidth(ja, Class::Dividing, Class::None, false, 8, 6);     // ！？ keep their width
  expectWidth(ja, Class::MiddleDot, Class::None, false, 8, 4);    // ・：； may halve
}

}  // namespace
