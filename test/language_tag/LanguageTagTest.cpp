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
  EXPECT_EQ(normalised("ger"), "de");
  EXPECT_EQ(normalised("deu"), "de");
  EXPECT_EQ(normalised("jpn"), "ja");
  EXPECT_EQ(normalised("aar"), "aa");
  EXPECT_EQ(normalised("zul"), "zu");
}

TEST(NormaliseLanguageTag, KeepsAnUnmappedCodeLowerCasedAndTrimmed) {
  EXPECT_EQ(normalised("yue"), "yue");
  EXPECT_EQ(normalised("  EN \n"), "en");
}

TEST(NormaliseLanguageTag, RejectsAMissingOrMalformedPrimarySubtag) {
  char out[LANGUAGE_TAG_BUFFER_SIZE] = "junk";
  EXPECT_FALSE(normaliseLanguageTag("", out, sizeof(out)));
  EXPECT_STREQ(out, "");
  for (const char* tag : {"  ", "-US", "en1", "abcdefghi"})
    EXPECT_FALSE(normaliseLanguageTag(tag, out, sizeof(out))) << tag;
  EXPECT_EQ(normalised("abcdefgh"), "abcdefgh");
}

TEST(NormaliseLanguageTag, RejectsPlaceholderCodesThatNameNoLanguage) {
  char out[LANGUAGE_TAG_BUFFER_SIZE];
  for (const char* tag : {"und", "UND", "und-Latn", "mul", "zxx", "mis"}) {
    EXPECT_FALSE(normaliseLanguageTag(tag, out, sizeof(out))) << tag;
  }
  EXPECT_EQ(normalised("mun"), "mun");
}

TEST(NormaliseLanguageTag, DropsARegionTheFirmwareDoesNotDistinguish) {
  EXPECT_EQ(normalised("en-US"), "en");
  EXPECT_EQ(normalised("en_GB"), "en");
}

TEST(NormaliseLanguageTag, KeepsADistinguishedRegionInTheTranslationSpelling) {
  EXPECT_EQ(normalised("pt-br"), "pt-BR");
  EXPECT_EQ(normalised("pt-PT"), "pt-PT");
  EXPECT_EQ(normalised("PT_BR"), "pt-BR");
  EXPECT_EQ(normalised("\tpor-BR "), "pt-BR");
}

TEST(NormaliseLanguageTag, KeepsADistinguishedVariant) { EXPECT_EQ(normalised("ca-valencia"), "ca-valencia"); }

TEST(NormaliseLanguageTag, DropsAScriptSubtagFromAnUnknownLanguage) {
  EXPECT_EQ(normalised("xx-Latn"), "xx");
  EXPECT_EQ(languageNameForTag("xx"), nullptr);
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
