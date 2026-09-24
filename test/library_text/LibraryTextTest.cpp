#include <gtest/gtest.h>

#include <string>

#include "LibraryText.h"

using library::authorKey;
using library::fold;

namespace {

// The same text in both Unicode normal forms. Every fold test runs both, because
// a card holding only one form makes a one-sided test pass by accident — which
// is exactly how a fold built on utf8ComposeNfc() survives until the first file
// arrives from the other kind of machine.
struct NormalisationPair {
  const char* nfc;  // precomposed: e-acute is one codepoint
  const char* nfd;  // decomposed: e followed by a combining acute
  const char* expected;
};

constexpr NormalisationPair PAIRS[] = {
    {"pand\xC3\xA9mie", "pande\xCC\x81mie", "pandemie"},
    {"\xC3\x89"
     "clipse totale",
     "E\xCC\x81"
     "clipse totale",
     "eclipse totale"},
    {"M\xC3\xA9moires", "Me\xCC\x81moires", "memoires"},
    {"Ang\xC3\xA9lina", "Ange\xCC\x81lina", "angelina"},
    {"R\xC3\xA9"
     "camier",
     "Re\xCC\x81"
     "camier",
     "recamier"},
    {"Derri\xC3\xA8re les collines", "Derrie\xCC\x80re les collines", "derriere les collines"},
};

}  // namespace

TEST(LibraryFold, BothNormalisationsAgree) {
  for (const auto& p : PAIRS) {
    EXPECT_EQ(fold(p.nfc), p.expected) << "NFC input: " << p.nfc;
    EXPECT_EQ(fold(p.nfd), p.expected) << "NFD input: " << p.nfd;
    EXPECT_EQ(fold(p.nfc), fold(p.nfd)) << "forms disagree for " << p.expected;
  }
}

TEST(LibraryFold, LettersWithoutCanonicalDecomposition) {
  // These have no NFD form at all, so a decompose-only fold silently deletes
  // them. "Søren" losing its last letter is the case that motivated the map.
  EXPECT_EQ(fold("S\xC3\xB8ren"), "soren");
  EXPECT_EQ(fold("\xC3\x98rsted"), "orsted");
  EXPECT_EQ(fold("\xC3\x86"
                 "sop"),
            "aesop");
  EXPECT_EQ(fold("Stra\xC3\x9F"
                 "e"),
            "strasse");
  EXPECT_EQ(fold("\xC5\x81odz"), "lodz");
  EXPECT_EQ(fold("s\xC5\x93ur"), "soeur");
}

TEST(LibraryFold, ApostrophesSurviveInNamesAndElisions) {
  // U+2019 is what exporters actually emit; folding it to a space would split
  // "O'Malley" into two tokens and change both its sort place and its search.
  // ("Malley", not "Brien": C++ hex escapes are greedy, so \x99B would parse
  // as the single escape 0x99B.)
  EXPECT_EQ(fold("O\xE2\x80\x99Malley"), "o'malley");
  EXPECT_EQ(fold("O'Malley"), "o'malley");
  EXPECT_EQ(fold("L\xE2\x80\x99\xC3\x89n\xC3\xA9ide"), "l'eneide");
}

TEST(LibraryFold, TypographicDashesAndQuotesFoldLikeTheirAsciiForms) {
  // An em dash used to stay inside the fold word while an ASCII hyphen broke
  // it, so the same title written both ways sorted and searched differently.
  EXPECT_EQ(library::fold("a\u2014b"), library::fold("a-b"));
  EXPECT_EQ(library::fold("a\u2013b"), library::fold("a-b"));
  EXPECT_EQ(library::fold("\u201Cquoted\u201D"), library::fold("\"quoted\""));
}

TEST(LibraryFold, PunctuationSeparatesAndSpaceRunsCollapse) {
  EXPECT_EQ(fold("Le juge Untel.T2.Le po\xC3\xA8me"), "le juge untel t2 le poeme");
  EXPECT_EQ(fold("  spaced   out  "), "spaced out");
  EXPECT_EQ(fold("a---b"), "a b");
  EXPECT_EQ(fold("2085 _ Artificial"), "2085 artificial");
  EXPECT_EQ(fold(""), "");
  EXPECT_EQ(fold("!!!"), "");
}

TEST(LibraryFold, PreservesHebrewLettersAndDropsNiqqud) {
  EXPECT_EQ(fold("\u05E9\u05B8\u05C1\u05DC\u05D5\u05B9\u05DD"), "\u05E9\u05DC\u05D5\u05DD");
  EXPECT_TRUE(library::matchesQuery(fold("\u05E9\u05DC\u05D5\u05DD \u05E2\u05D5\u05DC\u05DD"), fold("\u05E9\u05DC")));
  EXPECT_FALSE(authorKey("\u05E2\u05DE\u05D5\u05E1 \u05E2\u05D5\u05D6").empty());
}

TEST(LibraryFold, GroupInitialUsesUnicodeLettersAndBucketsNumbers) {
  EXPECT_EQ(library::foldedGroupInitial(fold("Alpha")), static_cast<uint32_t>('a'));
  EXPECT_EQ(library::foldedGroupInitial(fold("\u05E9\u05DC\u05D5\u05DD")), 0x05E9u);
  EXPECT_EQ(library::foldedGroupInitial(fold("\u041A\u043D\u0438\u0433\u0430")), 0x041Au);
  EXPECT_EQ(library::foldedGroupInitial(fold("\u4E66")), 0x4E66u);
  EXPECT_EQ(library::foldedGroupInitial(fold("2085")), 0u);
  EXPECT_EQ(library::foldedGroupInitial(fold("\u0662\u0660\u0668\u0665")), 0u);
  EXPECT_EQ(library::foldedGroupInitial(fold("!!!")), 0u);
}

namespace {

// Copies of the translation files' `_articles`; the library itself never names
// a language.
constexpr const char* EN_ARTICLES[] = {"the", "a", "an"};
constexpr const char* FR_ARTICLES[] = {"le", "la", "les", "l'", "un", "une", "du", "des"};
constexpr const char* DE_ARTICLES[] = {"der", "die", "das"};
constexpr const char* IT_ARTICLES[] = {"il", "lo", "la", "l'", "gli", "i", "un"};
constexpr const char* PT_ARTICLES[] = {"o", "os", "a"};
constexpr library::Articles NO_ARTICLES{};

// std::span has no ==; the same list is the same array.
bool sameList(const library::Articles a, const library::Articles b) {
  return a.data() == b.data() && a.size() == b.size();
}

std::string sortKey(const char* title, const library::Articles articles) {
  std::string key = fold(title);
  library::stripLeadingArticle(key, articles);
  return key;
}

constexpr const char* TAGS[] = {"en", "fr", "pt-BR", "pt-PT", "ru"};
constexpr const char* EN_CODES[] = {"eng"};
constexpr const char* FR_CODES[] = {"fre", "fra"};
constexpr const char* PT_CODES[] = {"por"};
constexpr const char* RU_CODES[] = {"rus"};
constexpr library::LanguageCodes CODES[] = {EN_CODES, FR_CODES, PT_CODES, PT_CODES, RU_CODES};
constexpr library::Articles LISTS[] = {EN_ARTICLES, FR_ARTICLES, PT_ARTICLES, PT_ARTICLES, NO_ARTICLES};
const library::ArticlesByLanguage CONFIG{TAGS, CODES, LISTS, 5, EN_ARTICLES};

}  // namespace

TEST(LibraryFold, FoldKeepsLeadingArticles) { EXPECT_EQ(fold("The Iliad"), "the iliad"); }

TEST(LibraryArticles, EnglishStripsOnlyEnglishArticles) {
  EXPECT_EQ(sortKey("The Catcher in the Rye", EN_ARTICLES), "catcher in the rye");
  EXPECT_EQ(sortKey("An Instance of the Fingerpost", EN_ARTICLES), "instance of the fingerpost");
  // Articles in other languages, words in English.
  EXPECT_EQ(sortKey("I Am Number Four", EN_ARTICLES), "i am number four");
  EXPECT_EQ(sortKey("I, Robot", EN_ARTICLES), "i robot");
  EXPECT_EQ(sortKey("O Pioneers!", EN_ARTICLES), "o pioneers");
  EXPECT_EQ(sortKey("Die Trying", EN_ARTICLES), "die trying");
  // An article must be a whole word.
  EXPECT_EQ(sortKey("Anna Karenina", EN_ARTICLES), "anna karenina");
  // A title that IS an article must not vanish.
  EXPECT_EQ(sortKey("The", EN_ARTICLES), "the");
}

TEST(LibraryArticles, EachLanguageStripsItsOwn) {
  EXPECT_EQ(sortKey("Les Mis\xC3\xA9rables", FR_ARTICLES), "miserables");
  EXPECT_EQ(sortKey("L\xE2\x80\x99\xC3\x89n\xC3\xA9ide", FR_ARTICLES), "eneide");
  EXPECT_EQ(sortKey("Il \xC3\xA9tait une fois", FR_ARTICLES), "il etait une fois");
  EXPECT_EQ(sortKey("\xC3\x80 la recherche du temps perdu", FR_ARTICLES), "a la recherche du temps perdu");
  EXPECT_EQ(sortKey("Die Verwandlung", DE_ARTICLES), "verwandlung");
  EXPECT_EQ(sortKey("I promessi sposi", IT_ARTICLES), "promessi sposi");
  EXPECT_EQ(sortKey("L'amica geniale", IT_ARTICLES), "amica geniale");
  EXPECT_EQ(sortKey("The Hobbit", NO_ARTICLES), "the hobbit");
}

TEST(LibraryArticles, TagFindsItsLanguage) {
  using library::articlesForLanguage;
  EXPECT_TRUE(sameList(articlesForLanguage("en", CONFIG), LISTS[0]));
  EXPECT_TRUE(sameList(articlesForLanguage("en-US", CONFIG), LISTS[0]));
  EXPECT_TRUE(sameList(articlesForLanguage("EN", CONFIG), LISTS[0]));
  EXPECT_TRUE(sameList(articlesForLanguage(" en \n", CONFIG), LISTS[0]));
  EXPECT_TRUE(sameList(articlesForLanguage("eng", CONFIG), LISTS[0]));
  EXPECT_TRUE(sameList(articlesForLanguage("fre", CONFIG), LISTS[1]));
  EXPECT_TRUE(sameList(articlesForLanguage("fra", CONFIG), LISTS[1]));
  EXPECT_TRUE(sameList(articlesForLanguage("fr-CA", CONFIG), LISTS[1]));
  EXPECT_TRUE(sameList(articlesForLanguage("pt_BR", CONFIG), LISTS[2]));
  EXPECT_TRUE(sameList(articlesForLanguage("pt", CONFIG), LISTS[2]));
  EXPECT_TRUE(sameList(articlesForLanguage("por", CONFIG), LISTS[2]));
}

TEST(LibraryArticles, KnownLanguageWithoutArticlesStripsNothingAndAnythingElseFallsBack) {
  using library::articlesForLanguage;
  // A known language without articles keeps every word.
  EXPECT_TRUE(sameList(articlesForLanguage("ru", CONFIG), LISTS[4]));
  EXPECT_TRUE(sameList(articlesForLanguage("rus", CONFIG), LISTS[4]));
  // Anything that is not a known language code falls back.
  for (const char* tag : {"", "und", "mul", "zxx", "ja", "English", "enfr", "e", "gem"}) {
    EXPECT_TRUE(sameList(articlesForLanguage(tag, CONFIG), CONFIG.fallbackArticles)) << tag;
  }
}

TEST(LibraryArticles, ConfigIdTracksListsAndFallback) {
  const uint32_t base = library::articleConfigId(CONFIG);
  EXPECT_NE(base, 0u);

  library::ArticlesByLanguage otherFallback = CONFIG;
  otherFallback.fallbackArticles = LISTS[1];
  EXPECT_NE(library::articleConfigId(otherFallback), base);

  static constexpr const char* FR_SHORTER[] = {"le", "la", "les", "l'", "un", "une", "du"};
  static constexpr library::Articles EDITED[] = {EN_ARTICLES, FR_SHORTER, PT_ARTICLES, PT_ARTICLES, NO_ARTICLES};
  library::ArticlesByLanguage editedList = CONFIG;
  editedList.articles = EDITED;
  EXPECT_NE(library::articleConfigId(editedList), base);

  static constexpr const char* FR_ONE_CODE[] = {"fre"};
  static constexpr library::LanguageCodes EDITED_CODES[] = {EN_CODES, FR_ONE_CODE, PT_CODES, PT_CODES, RU_CODES};
  library::ArticlesByLanguage editedCodes = CONFIG;
  editedCodes.iso639_2 = EDITED_CODES;
  EXPECT_NE(library::articleConfigId(editedCodes), base);

  // The same words split differently between two languages.
  static constexpr const char* THE_A[] = {"the", "a"};
  static constexpr const char* AN[] = {"an"};
  static constexpr const char* THE[] = {"the"};
  static constexpr const char* A_AN[] = {"a", "an"};
  static constexpr library::Articles SPLIT_A[] = {THE_A, AN};
  static constexpr library::Articles SPLIT_B[] = {THE, A_AN};
  EXPECT_NE(library::articleConfigId({TAGS, CODES, SPLIT_A, 2, NO_ARTICLES}),
            library::articleConfigId({TAGS, CODES, SPLIT_B, 2, NO_ARTICLES}));
}

TEST(LibraryAuthorKey, OrderAndPunctuationDoNotMatter) {
  const std::string expected = authorKey("Lu Xun");
  EXPECT_FALSE(expected.empty());
  for (const char* spelling : {"Lu, Xun", "Xun, Lu", "Lu Xun_", "Lu Xun [Xun, Lu]", "  lu   xun  "}) {
    EXPECT_EQ(authorKey(spelling), expected) << spelling;
  }
}

TEST(LibraryAuthorKey, InitialsAreIgnored) {
  EXPECT_EQ(authorKey("Herbert G Wells"), authorKey("Herbert Wells"));
  EXPECT_EQ(authorKey("Wells, Herbert G."), authorKey("Herbert Wells"));
}

TEST(LibraryAuthorKey, SecondaryAuthorsAndBracketsDropped) {
  EXPECT_EQ(authorKey("Emile Erckmann; Alexandre Chatrian"), authorKey("Emile Erckmann"));
  EXPECT_EQ(authorKey("George Sand [Sand, George]"), authorKey("George Sand"));
}

TEST(LibraryAuthorKey, FilesystemUnderscoreStandsInForAFullStop) {
  // The one input where the key's cleanup and cleanPersonName's differ before
  // folding: an underscore the filesystem took instead of a full stop. Both
  // reduce to the same initial, which fold() then drops as a one-letter token.
  EXPECT_EQ(authorKey("Herbert G_ Wells"), authorKey("Herbert Wells"));
  EXPECT_EQ(authorKey("Wells_ Herbert"), authorKey("Herbert Wells"));
}

TEST(LibraryAuthorKey, DistinctPeopleDoNotCollide) {
  EXPECT_NE(authorKey("Mary Wollstonecraft"), authorKey("Charlotte Bronte"));
  EXPECT_NE(authorKey("Victor Hugo"), authorKey("Jules Verne"));
}

TEST(LibraryAuthorKey, FitsTheRecordFieldWithoutCollapsingToAForename) {
  const std::string key = authorKey("Bartholomew Fitzgerald Wellington");
  ASSERT_FALSE(key.empty());
  EXPECT_LE(key.size(), library::AUTHOR_KEY_MAX_BYTES);
  EXPECT_NE(key.back(), ' ');

  // Sorting puts a short forename first, so cutting on a token boundary would
  // reduce this to "mary" and merge every Alex in the library. The byte cut must
  // keep enough of the surname to discriminate.
  const std::string mary = authorKey("Wollstonecraft, Mary");
  EXPECT_GT(mary.size(), 5u);
  EXPECT_NE(mary, "mary");
  EXPECT_NE(mary, authorKey("Mary Trevelyan"));

  // A truncated key stays a prefix of the untruncated one, so grouping is stable
  // however long the name is.
  EXPECT_EQ(authorKey("Wollstonecraft, Maryse").rfind("mary", 0), 0u);

  EXPECT_FALSE(authorKey("Nebuchadnezzarson").empty());
  EXPECT_TRUE(authorKey("").empty());
  EXPECT_TRUE(authorKey("Q. X. Z.").empty());  // initials only: no identity
}

// --- matchesQuery ------------------------------------------------------------
//
// Cases taken from the shape of the accented and
// apostrophised titles real cards hold — what a naive matcher gets wrong.

TEST(MatchesQuery, EmptyQueryMatchesEverything) {
  EXPECT_TRUE(library::matchesQuery(library::fold("Wuthering Heights"), ""));
}

TEST(MatchesQuery, WholeWordMatches) {
  EXPECT_TRUE(library::matchesQuery(library::fold("Wuthering Heights"), library::fold("heights")));
}

TEST(MatchesQuery, PrefixOfOneWordIsEnough) {
  EXPECT_TRUE(library::matchesQuery(library::fold("Wuthering Heights"), library::fold("hei")));
}

// The point of the whole design: six keypresses instead of ten, on a panel where
// each one costs a full repaint.
TEST(MatchesQuery, EveryWordMayBeAbbreviated) {
  EXPECT_TRUE(library::matchesQuery(library::fold("Wuthering Heights"), library::fold("wut hei")));
}

TEST(MatchesQuery, WordsNeedNotBeInOrder) {
  EXPECT_TRUE(library::matchesQuery(library::fold("Wuthering Heights"), library::fold("heights wuthering")));
}

TEST(MatchesQuery, EveryWordMustHit) {
  EXPECT_FALSE(library::matchesQuery(library::fold("Wuthering Heights"), library::fold("wuthering blue")));
}

// A prefix, not a substring: "eights" is inside "heights" but starts no word.
TEST(MatchesQuery, MidWordDoesNotMatch) {
  EXPECT_FALSE(library::matchesQuery(library::fold("Wuthering Heights"), library::fold("eights")));
}

TEST(MatchesQuery, AccentsAreIgnoredOnBothSides) {
  EXPECT_TRUE(library::matchesQuery(library::fold("L'Énéide"), library::fold("eneide")));
  EXPECT_TRUE(library::matchesQuery(library::fold("L'Eneide"), library::fold("énéide")));
  EXPECT_TRUE(library::matchesQuery(library::fold("Éluard"), library::fold("eluard")));
}

TEST(MatchesQuery, ApostropheSplitsWords) {
  EXPECT_TRUE(library::matchesQuery(library::fold("Le bureau d'à côté"), library::fold("cote")));
}

TEST(MatchesQuery, CaseIsIgnored) {
  EXPECT_TRUE(library::matchesQuery(library::fold("Wuthering Heights"), library::fold("HEIGHTS")));
}

// --- inverted author names ---------------------------------------------------

TEST(CleanPersonName, InvertedNameIsTurnedRound) {
  EXPECT_EQ(library::cleanPersonName("Austen, Jane"), "Jane Austen");
  EXPECT_EQ(library::cleanPersonName("Wollstonecraft, Mary"), "Mary Wollstonecraft");
}

TEST(CleanPersonName, PlainNameIsUntouched) { EXPECT_EQ(library::cleanPersonName("Emily Bronte"), "Emily Bronte"); }

// Two commas mean a suffix or a list, not an inversion — leave it alone rather
// than scramble it.
TEST(CleanPersonName, MultipleCommasAreLeftAlone) {
  // The trailing full stop is stripped by the existing noise rules.
  EXPECT_EQ(library::cleanPersonName("Smith, John, Jr."), "Smith, John, Jr");
}

TEST(CleanPersonName, DanglingCommaIsNotAnInversion) { EXPECT_EQ(library::cleanPersonName("Austen,"), "Austen"); }

// --- surnameKey --------------------------------------------------------------

TEST(SurnameKey, SurnameLeadsThenGivenNames) {
  EXPECT_EQ(library::surnameKey("Herman Melville"), "melville herman");
  EXPECT_EQ(library::surnameKey("Mary Wollstonecraft"), "wollstonecraft mary");
}

TEST(SurnameKey, SingleWordKeysOnItself) { EXPECT_EQ(library::surnameKey("Voltaire"), "voltaire"); }

TEST(SurnameKey, AccentsAreFolded) { EXPECT_EQ(library::surnameKey("Paul Éluard"), "eluard paul"); }

TEST(SurnameKey, ThreeWordNamesTakeTheLast) { EXPECT_EQ(library::surnameKey("Herbert G. Wells"), "wells herbert g"); }

TEST(SurnameKey, EmptyStaysEmpty) { EXPECT_EQ(library::surnameKey(""), ""); }

// The whole point of keying off the DISPLAY name: the spelling vote has already
// made every book by one author show one name, so a group cannot land in two
// places even though "Victor Hugo" and "Hugo Victor" both exist in the wild.
TEST(SurnameKey, HarmonisedDisplayNameKeepsAGroupTogether) {
  EXPECT_NE(library::surnameKey("Victor Hugo"), library::surnameKey("Hugo Victor"));
}

TEST(LibraryPath, RootDoesNotGainASecondSeparator) {
  EXPECT_EQ(library::joinLibraryPath("/", "book.epub"), "/book.epub");
  EXPECT_EQ(library::joinLibraryPath("", "book.epub"), "/book.epub");
}

TEST(LibraryPath, NestedFolderGetsOneSeparator) {
  EXPECT_EQ(library::joinLibraryPath("/Books", "book.epub"), "/Books/book.epub");
}
