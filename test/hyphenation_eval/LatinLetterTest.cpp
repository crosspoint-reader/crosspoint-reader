#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "lib/Epub/Epub/hyphenation/HyphenationCommon.h"
#include "lib/Epub/Epub/hyphenation/LanguageHyphenator.h"
#include "lib/Epub/Epub/hyphenation/LanguageRegistry.h"
#include "lib/Epub/Epub/hyphenation/SerializedHyphenationTrie.h"

// The evaluation fixtures contain no comma-below Ș/ș/Ț/ț (U+0218-U+021B), so these cover
// their recognition and lowercasing directly, plus the end-to-end effect: a word containing
// one of them must reach the patterns instead of being skipped as "not a word".
namespace {

TEST(LatinLetterTest, CommaBelowLettersAreLatinLetters) {
  for (const uint32_t cp : {0x0218u, 0x0219u, 0x021Au, 0x021Bu}) {
    EXPECT_TRUE(isLatinLetter(cp)) << std::hex << cp;
    EXPECT_TRUE(isAlphabetic(cp)) << std::hex << cp;
  }
  // The cedilla forms (Latin Extended-A) already were.
  for (const uint32_t cp : {0x015Eu, 0x015Fu, 0x0162u, 0x0163u}) {
    EXPECT_TRUE(isLatinLetter(cp)) << std::hex << cp;
  }
}

TEST(LatinLetterTest, CommaBelowLettersLowercase) {
  EXPECT_EQ(toLowerLatin(0x0218u), 0x0219u);  // Ș -> ș
  EXPECT_EQ(toLowerLatin(0x021Au), 0x021Bu);  // Ț -> ț
  EXPECT_EQ(toLowerLatin(0x0219u), 0x0219u);
  EXPECT_EQ(toLowerLatin(0x021Bu), 0x021Bu);
}

std::vector<size_t> englishBreaks(const std::string& word) {
  const LanguageHyphenator* hyphenator = getLanguageHyphenatorForPrimaryTag("en");
  EXPECT_NE(hyphenator, nullptr);
  if (!hyphenator) return {};
  return hyphenator->breakIndexes(collectCodepoints(word));
}

TEST(LatinLetterTest, WordsWithCommaBelowLettersAreHyphenated) {
  // No English pattern mentions these letters, so a word ending in one must hyphenate exactly
  // like the same word ending in the cedilla form, which was always accepted.
  const auto cedilla = englishBreaks("hyphenation\xC5\x9F");  // ...ş (U+015F)
  ASSERT_FALSE(cedilla.empty());
  EXPECT_EQ(englishBreaks("hyphenation\xC8\x99"), cedilla);  // ...ș (U+0219)
  EXPECT_EQ(englishBreaks("hyphenation\xC8\x9B"), cedilla);  // ...ț (U+021B)
  EXPECT_EQ(englishBreaks("HYPHENATION\xC8\x98"), cedilla);  // ...Ș (U+0218), uppercase word
  EXPECT_EQ(englishBreaks("HYPHENATION\xC8\x9A"), cedilla);  // ...Ț (U+021A)
}

// Test-only patterns {1ș, 1ț}: a break before a lowercase comma-below s or t, nothing else. The bytes are the
// hypher trie of those two patterns, embedded the way scripts/generate_hyphenation_trie.py embeds the shipped
// tries (4-byte root header stripped, root offset 0x01). With these patterns a break only appears if the word
// reaches the trie with ș/ț lowercase, so the uppercase cases fail unless toLowerLatin() maps Ș/Ț, and every
// case fails unless isLatinLetter() accepts them.
alignas(4) constexpr uint8_t kCommaBelowTrie[] = {
    0x01, 0x21, 0xC8, 0x03, 0x22, 0x99, 0x9B, 0x05, 0x08, 0xE0, 0x00, 0x41, 0xE0, 0x00, 0x41,
};
constexpr SerializedHyphenationPatterns kCommaBelowPatterns = {0x01u, kCommaBelowTrie, sizeof(kCommaBelowTrie)};

std::vector<size_t> commaBelowBreaks(const std::string& word) {
  const LanguageHyphenator hyphenator(kCommaBelowPatterns, isLatinLetter, toLowerLatin);
  return hyphenator.breakIndexes(collectCodepoints(word));
}

TEST(LatinLetterTest, BreaksThatDependOnLowercaseCommaBelowLetters) {
  const std::vector<size_t> beforeThird = {2};
  EXPECT_EQ(commaBelowBreaks("aa\xC8\x99"
                             "aa"),
            beforeThird);  // aașaa
  EXPECT_EQ(commaBelowBreaks("aa\xC8\x9B"
                             "aa"),
            beforeThird);  // aațaa
  EXPECT_EQ(commaBelowBreaks("AA\xC8\x98"
                             "AA"),
            beforeThird);  // AAȘAA: needs Ș -> ș
  EXPECT_EQ(commaBelowBreaks("AA\xC8\x9A"
                             "AA"),
            beforeThird);  // AAȚAA: needs Ț -> ț
  // The cedilla forms are different letters: no pattern, no break.
  EXPECT_TRUE(commaBelowBreaks("aa\xC5\x9F"
                               "aa")
                  .empty());  // aaşaa
  EXPECT_TRUE(commaBelowBreaks("aa\xC5\xA3"
                               "aa")
                  .empty());  // aaţaa
}

TEST(LatinLetterTest, RegistryWiresLatinScriptLanguagesToLatinHelpers) {
  for (const char* tag : {"en", "fr", "de", "es", "it", "pl", "sv", "fi", "pt"}) {
    const LanguageHyphenator* hyphenator = getLanguageHyphenatorForPrimaryTag(tag);
    ASSERT_NE(hyphenator, nullptr) << tag;
    EXPECT_EQ(hyphenator->config().isLetter, &isLatinLetter) << tag;
    EXPECT_EQ(hyphenator->config().toLower, &toLowerLatin) << tag;
  }
}

}  // namespace
