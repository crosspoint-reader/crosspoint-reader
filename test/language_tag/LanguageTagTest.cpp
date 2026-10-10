#include <LanguageTag.h>
#include <gtest/gtest.h>

#include <cstring>
#include <string>

namespace {

std::string normalised(const char* tag) {
  char out[LANGUAGE_TAG_BUFFER_SIZE];
  EXPECT_TRUE(normaliseLanguageTag(tag, out, sizeof(out))) << tag;
  return out;
}

}  // namespace

TEST(NormaliseLanguageTag, MapsIso6392CodesToIso6391) {
  EXPECT_EQ(normalised("eng"), "en");
  EXPECT_EQ(normalised("fre"), "fr");
  EXPECT_EQ(normalised("fra"), "fr");
  EXPECT_EQ(normalised("deu"), "de");
}

TEST(NormaliseLanguageTag, DropsARegionTheFirmwareDoesNotDistinguish) {
  EXPECT_EQ(normalised("en-US"), "en");
  EXPECT_EQ(normalised("en_GB"), "en");
}

TEST(NormaliseLanguageTag, LowerCasesThePrimarySubtag) {
  EXPECT_EQ(normalised("EN"), "en");
  EXPECT_EQ(normalised("Fr"), "fr");
}

TEST(NormaliseLanguageTag, KeepsADistinguishedRegionInTheTranslationSpelling) {
  EXPECT_EQ(normalised("pt-br"), "pt-BR");
  EXPECT_EQ(normalised("pt-PT"), "pt-PT");
  EXPECT_EQ(normalised("PT_BR"), "pt-BR");
}

TEST(NormaliseLanguageTag, KeepsADistinguishedVariant) { EXPECT_EQ(normalised("ca-valencia"), "ca-valencia"); }

TEST(NormaliseLanguageTag, DropsAScriptSubtagFromAnUnknownLanguage) {
  EXPECT_EQ(normalised("xx-Latn"), "xx");
  EXPECT_EQ(languageNameForTag("xx"), nullptr);
}

TEST(NormaliseLanguageTag, TrimsSurroundingWhitespace) {
  EXPECT_EQ(normalised("  en \n"), "en");
  EXPECT_EQ(normalised("\tpt-BR "), "pt-BR");
}

TEST(NormaliseLanguageTag, RejectsAnEmptyTag) {
  char out[LANGUAGE_TAG_BUFFER_SIZE] = "junk";
  EXPECT_FALSE(normaliseLanguageTag("", out, sizeof(out)));
  EXPECT_STREQ(out, "");
  EXPECT_FALSE(normaliseLanguageTag("   ", out, sizeof(out)));
  EXPECT_STREQ(out, "");
}

TEST(NormaliseLanguageTag, RejectsPlaceholderCodesThatNameNoLanguage) {
  char out[LANGUAGE_TAG_BUFFER_SIZE] = "junk";
  EXPECT_FALSE(normaliseLanguageTag("und", out, sizeof(out)));
  EXPECT_STREQ(out, "");
  EXPECT_FALSE(normaliseLanguageTag("mul", out, sizeof(out)));
  EXPECT_FALSE(normaliseLanguageTag("zxx", out, sizeof(out)));
  EXPECT_FALSE(normaliseLanguageTag("mis-US", out, sizeof(out)));
}

TEST(NormaliseLanguageTag, RejectsAPrimarySubtagThatIsNotLetters) {
  char out[LANGUAGE_TAG_BUFFER_SIZE];
  EXPECT_FALSE(normaliseLanguageTag("123", out, sizeof(out)));
  EXPECT_FALSE(normaliseLanguageTag("-US", out, sizeof(out)));
  EXPECT_FALSE(normaliseLanguageTag("en1", out, sizeof(out)));
}

TEST(NormaliseLanguageTag, RejectsAPrimarySubtagLongerThanEightBytes) {
  char out[LANGUAGE_TAG_BUFFER_SIZE];
  EXPECT_FALSE(normaliseLanguageTag("abcdefghi", out, sizeof(out)));
  EXPECT_EQ(normalised("abcdefgh"), "abcdefgh");
}

TEST(NormaliseLanguageTag, RejectsABufferSmallerThanTheDocumentedMinimum) {
  char out[8] = "junk";
  EXPECT_FALSE(normaliseLanguageTag("en", out, sizeof(out)));
  EXPECT_STREQ(out, "");
}

TEST(LanguageNameForTag, ReturnsTheNativeNameOfAKnownTag) {
  EXPECT_STREQ(languageNameForTag("fr"), "Français");
  EXPECT_STREQ(languageNameForTag("en"), "English");
  EXPECT_STREQ(languageNameForTag("pt-BR"), "Português (Brasil)");
  EXPECT_STREQ(languageNameForTag("ca-valencia"), "Valencià");
}

TEST(LanguageNameForTag, ReturnsNullForAnUnknownOrUnnormalisedTag) {
  EXPECT_EQ(languageNameForTag("xx"), nullptr);
  EXPECT_EQ(languageNameForTag("FR"), nullptr);
  EXPECT_EQ(languageNameForTag("pt"), nullptr);
  EXPECT_EQ(languageNameForTag(""), nullptr);
  EXPECT_EQ(languageNameForTag(nullptr), nullptr);
}

TEST(NormaliseLanguageTag, MapsLanguagesBeyondTheHyphenationDictionary) {
  EXPECT_EQ(normalised("nld"), "nl");
  EXPECT_EQ(normalised("dut"), "nl");
  EXPECT_EQ(normalised("pol"), "pl");
  EXPECT_EQ(normalised("por-BR"), "pt-BR");
  EXPECT_EQ(normalised("zho"), "zh");
  EXPECT_EQ(normalised("chi"), "zh");
  EXPECT_EQ(normalised("jpn"), "ja");
  EXPECT_EQ(normalised("kor"), "ko");
  EXPECT_EQ(normalised("sqi"), "sq");
  EXPECT_EQ(normalised("alb"), "sq");
  EXPECT_EQ(normalised("aar"), "aa");
  EXPECT_EQ(normalised("zul"), "zu");
  EXPECT_EQ(normalised("yue"), "yue");
}
