// The typography profile a book's language picks (spec section 3), and the punctuation widths of each language's
// table (spec section 5.2, from JLREQ and CLREQ).
#include <gtest/gtest.h>

#include "Epub/TypographyProfile.h"

namespace {

using Table = TypographyProfile::Punctuation;

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

}  // namespace
