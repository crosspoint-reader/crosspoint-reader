#include <LanguageCode.h>
#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace {

std::string primary(const char* tag) {
  char out[PRIMARY_LANGUAGE_SUBTAG_BUFFER_SIZE];
  EXPECT_TRUE(normalisePrimaryLanguageSubtag(tag, out, sizeof(out))) << tag;
  return out;
}

}  // namespace

TEST(NormalisePrimaryLanguageSubtag, MapsIso6392CodesToIso6391) {
  EXPECT_EQ(primary("eng"), "en");
  EXPECT_EQ(primary("ger"), "de");
  EXPECT_EQ(primary("deu"), "de");
  EXPECT_EQ(primary("aar"), "aa");
  EXPECT_EQ(primary("zul"), "zu");
}

TEST(NormalisePrimaryLanguageSubtag, KeepsAnUnmappedCodeLowerCased) {
  EXPECT_EQ(primary("yue"), "yue");
  EXPECT_EQ(primary("EN"), "en");
}

TEST(NormalisePrimaryLanguageSubtag, DropsEveryLaterSubtagIncludingTranslatedRegions) {
  EXPECT_EQ(primary("en-US"), "en");
  EXPECT_EQ(primary("pt_BR"), "pt");
  EXPECT_EQ(primary("por-BR"), "pt");
  EXPECT_EQ(primary("ca-valencia"), "ca");
}

TEST(NormalisePrimaryLanguageSubtag, ReportsTheTrimmedRestAfterTheSeparator) {
  char out[PRIMARY_LANGUAGE_SUBTAG_BUFFER_SIZE];
  std::string_view rest = "junk";
  ASSERT_TRUE(normalisePrimaryLanguageSubtag("  zh-Hant-TW \n", out, sizeof(out), &rest));
  EXPECT_STREQ(out, "zh");
  EXPECT_EQ(rest, "Hant-TW");

  ASSERT_TRUE(normalisePrimaryLanguageSubtag("fr", out, sizeof(out), &rest));
  EXPECT_EQ(rest, "");
}

TEST(NormalisePrimaryLanguageSubtag, RejectsAMissingOrMalformedPrimarySubtag) {
  char out[PRIMARY_LANGUAGE_SUBTAG_BUFFER_SIZE] = "junk";
  EXPECT_FALSE(normalisePrimaryLanguageSubtag("", out, sizeof(out)));
  EXPECT_STREQ(out, "");
  EXPECT_FALSE(normalisePrimaryLanguageSubtag("  ", out, sizeof(out)));
  EXPECT_FALSE(normalisePrimaryLanguageSubtag("-US", out, sizeof(out)));
  EXPECT_FALSE(normalisePrimaryLanguageSubtag("en1", out, sizeof(out)));
  EXPECT_FALSE(normalisePrimaryLanguageSubtag("abcdefghi", out, sizeof(out)));
  EXPECT_EQ(primary("abcdefgh"), "abcdefgh");
}

TEST(NormalisePrimaryLanguageSubtag, RejectsPlaceholderCodesThatNameNoLanguage) {
  char out[PRIMARY_LANGUAGE_SUBTAG_BUFFER_SIZE] = "junk";
  EXPECT_FALSE(normalisePrimaryLanguageSubtag("und", out, sizeof(out)));
  EXPECT_STREQ(out, "");
  EXPECT_FALSE(normalisePrimaryLanguageSubtag("UND", out, sizeof(out)));
  EXPECT_FALSE(normalisePrimaryLanguageSubtag("und-Latn", out, sizeof(out)));
  EXPECT_FALSE(normalisePrimaryLanguageSubtag("mul", out, sizeof(out)));
  EXPECT_FALSE(normalisePrimaryLanguageSubtag("zxx", out, sizeof(out)));
  EXPECT_FALSE(normalisePrimaryLanguageSubtag("mis", out, sizeof(out)));
  EXPECT_EQ(primary("mun"), "mun");
}

TEST(NormalisePrimaryLanguageSubtag, RejectsABufferSmallerThanTheDocumentedMinimum) {
  char out[PRIMARY_LANGUAGE_SUBTAG_BUFFER_SIZE - 1] = "junk";
  EXPECT_FALSE(normalisePrimaryLanguageSubtag("en", out, sizeof(out)));
  EXPECT_STREQ(out, "");
}
