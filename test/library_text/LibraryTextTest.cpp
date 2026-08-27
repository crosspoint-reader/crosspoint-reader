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

TEST(LibraryFold, ArticleStrippingOnlyWhenAsked) {
  EXPECT_EQ(fold("The Iliad"), "the iliad");
  EXPECT_EQ(fold("The Iliad", true), "iliad");
  EXPECT_EQ(fold("Les Mis\xC3\xA9rables", true), "miserables");
  EXPECT_EQ(fold("L\xE2\x80\x99\xC3\x89n\xC3\xA9ide", true), "eneide");
  // A title that IS an article-like word must not vanish.
  EXPECT_EQ(fold("The", true), "the");
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

// The stored fold is capped at 96 bytes, so a query word beyond that cannot be
// found. Asserted rather than left implicit: it is the one place a search can
// honestly fail to find a book that is really there.
TEST(MatchesQuery, LongTitlesAreOnlySearchableWithinTheStoredFold) {
  const std::string longTitle(120, 'a');
  const std::string folded = library::fold(longTitle + " needle").substr(0, 96);
  EXPECT_FALSE(library::matchesQuery(folded, library::fold("needle")));
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
