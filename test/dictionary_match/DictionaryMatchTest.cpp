#include <gtest/gtest.h>

#include <string>

#include "DictionaryMatch.h"

namespace {

using DictionaryMatch::isAsciiCasePrefix;
using DictionaryMatch::turkishIFold;
using DictionaryMatch::utf8Length;

// U+0130 İ and U+0131 ı as UTF-8, spelled out so the expectations stay
// readable in editors that render the codepoints themselves.
constexpr const char* CAPITAL_DOTTED_I = "\xC4\xB0";  // İ
constexpr const char* SMALL_DOTLESS_I = "\xC4\xB1";   // ı

TEST(TurkishIFold, FoldsAsciiCapitalIToDotlessSmallI) {
  // Uppercase Turkish I is dotless: ILIK (warm) reads ılık, not ilik (marrow).
  EXPECT_EQ(turkishIFold("ILIK"), std::string(SMALL_DOTLESS_I) + "L" + SMALL_DOTLESS_I + "K");
}

TEST(TurkishIFold, FoldsCapitalDottedIToAsciiSmallI) {
  EXPECT_EQ(turkishIFold((std::string(CAPITAL_DOTTED_I) + "stanbul").c_str()), "istanbul");
}

TEST(TurkishIFold, FoldsBothFormsInOneWord) {
  // KIRMIZI İP → kırmızı ip, both mappings in a single pass.
  const std::string input = std::string("KIRMIZI ") + CAPITAL_DOTTED_I + "P";
  const std::string expected =
      std::string("K") + SMALL_DOTLESS_I + "RM" + SMALL_DOTLESS_I + "Z" + SMALL_DOTLESS_I + " iP";
  EXPECT_EQ(turkishIFold(input.c_str()), expected);
}

TEST(TurkishIFold, ReturnsEmptyWhenNothingToFold) {
  EXPECT_EQ(turkishIFold("kitap"), "");
  EXPECT_EQ(turkishIFold("Reading"), "");  // lowercase i untouched
  EXPECT_EQ(turkishIFold(""), "");
  EXPECT_EQ(turkishIFold(nullptr), "");
}

TEST(TurkishIFold, LeavesOtherMultibyteSequencesAlone) {
  // ş (U+015F) contains no 0xC4 lead byte; ğ (U+011F) is 0xC4 0x9F — the
  // second byte check must keep it from being mistaken for İ.
  EXPECT_EQ(turkishIFold("I\xC5\x9F\xC4\x9F"), std::string(SMALL_DOTLESS_I) + "\xC5\x9F\xC4\x9F");
}

TEST(TurkishIFold, TruncatedLeadByteAtEndPassesThrough) {
  // A dangling 0xC4 with no continuation byte must not read past the end.
  EXPECT_EQ(turkishIFold("I\xC4"), std::string(SMALL_DOTLESS_I) + "\xC4");
}

TEST(Utf8Length, CountsCodepointsNotBytes) {
  EXPECT_EQ(utf8Length(""), 0u);
  EXPECT_EQ(utf8Length("kitap"), 5u);
  // ık: two letters, three bytes — the prefix-stem threshold must see 2.
  EXPECT_EQ(utf8Length((std::string(SMALL_DOTLESS_I) + "k").c_str()), 2u);
  // ışık: four letters, six bytes.
  const std::string isik = std::string(SMALL_DOTLESS_I) + "\xC5\x9F" + SMALL_DOTLESS_I + "k";
  EXPECT_EQ(utf8Length(isik.c_str()), 4u);
}

TEST(IsAsciiCasePrefix, AcceptsProperPrefix) {
  EXPECT_TRUE(isAsciiCasePrefix("kitap", "kitaplar"));
  EXPECT_TRUE(isAsciiCasePrefix("verhandlung", "verhandlungen"));
}

TEST(IsAsciiCasePrefix, FoldsAsciiCaseLikeTheIndexSort) {
  EXPECT_TRUE(isAsciiCasePrefix("Kitap", "kitaplar"));
  EXPECT_TRUE(isAsciiCasePrefix("KITAP", "kitaplar"));
}

TEST(IsAsciiCasePrefix, RejectsExactMatchAsNotProper) {
  EXPECT_FALSE(isAsciiCasePrefix("kitap", "kitap"));
  EXPECT_FALSE(isAsciiCasePrefix("Kitap", "kitap"));
}

TEST(IsAsciiCasePrefix, RejectsLongerOrDivergentHead) {
  EXPECT_FALSE(isAsciiCasePrefix("kitaplar", "kitap"));
  EXPECT_FALSE(isAsciiCasePrefix("kedi", "kitap"));
}

TEST(IsAsciiCasePrefix, ComparesMultibyteBytesExactly) {
  // ı is 0xC4 0xB1 on both sides — byte-wise equality, no case folding above
  // ASCII, exactly like StringUtils::asciiCaseCmp.
  const std::string stem = std::string("kap") + SMALL_DOTLESS_I;             // kapı
  const std::string derived = std::string("kap") + SMALL_DOTLESS_I + "lar";  // kapılar
  EXPECT_TRUE(isAsciiCasePrefix(stem.c_str(), derived.c_str()));
  // İ (0xC4 0xB0) vs ı (0xC4 0xB1) differ in the continuation byte.
  const std::string dotted = std::string("kap") + CAPITAL_DOTTED_I;
  EXPECT_FALSE(isAsciiCasePrefix(dotted.c_str(), derived.c_str()));
}

TEST(IsAsciiCasePrefix, EmptyHeadPrefixesAnyNonEmptyTarget) {
  // Documented contract: length policy lives in the caller.
  EXPECT_TRUE(isAsciiCasePrefix("", "kitap"));
  EXPECT_FALSE(isAsciiCasePrefix("", ""));
}

}  // namespace
