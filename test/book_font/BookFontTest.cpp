#include <gtest/gtest.h>

#include <string>

#include "ReaderFont.h"

namespace {

using namespace BookFontField;

ReaderFont font(const char* family, const uint8_t size, const uint8_t line = 2, const uint8_t word = 100,
                const uint8_t character = 2) {
  ReaderFont f;
  f.setFamily(family);
  f.fontPointSize = size;
  f.lineSpacing = line;
  f.wordSpacing = word;
  f.characterSpacing = character;
  return f;
}

BookFont book(const ReaderFont& f, const uint8_t fields) { return BookFont{f, fields}; }

const BookFont noFile;

TEST(ReaderFont, ComparesFamilySizeAndSpacing) {
  EXPECT_EQ(font("Alpha", 14), font("Alpha", 14));
  EXPECT_NE(font("Alpha", 14), font("Alpha", 16));
  EXPECT_NE(font("Alpha", 14), font("Beta", 14));
  EXPECT_NE(font("", 14), font("Alpha", 14));
  EXPECT_NE(font("Alpha", 14, 2), font("Alpha", 14, 3));
  EXPECT_NE(font("Alpha", 14, 2, 100), font("Alpha", 14, 2, 125));
  EXPECT_NE(font("Alpha", 14, 2, 100, 2), font("Alpha", 14, 2, 100, 1));
}

TEST(ReaderFont, SetFamilyTruncatesAndAcceptsNull) {
  const std::string longName(64, 'x');
  ReaderFont f = font(longName.c_str(), 14);
  EXPECT_EQ(std::string(f.sdFontFamilyName), longName.substr(0, sizeof(f.sdFontFamilyName) - 1));
  f.setFamily(nullptr);
  EXPECT_STREQ(f.sdFontFamilyName, "");
}

TEST(ReaderFont, FamilyAndSizeAreOneField) {
  EXPECT_EQ(differingFields(font("Alpha", 14), font("Beta", 14)), Font);
  EXPECT_EQ(differingFields(font("Alpha", 14), font("Alpha", 16)), Font);
  EXPECT_EQ(differingFields(font("Alpha", 14, 1, 75, 3), font("Alpha", 14)),
            LineSpacing | WordSpacing | CharacterSpacing);
}

TEST(BookFont, ComparesOnlyItsFields) {
  EXPECT_EQ(book(font("Alpha", 14, 1), LineSpacing), book(font("Beta", 16, 1), LineSpacing));
  EXPECT_NE(book(font("Alpha", 14, 1), LineSpacing), book(font("Alpha", 14, 3), LineSpacing));
  EXPECT_NE(book(font("Alpha", 14), Font), book(font("Alpha", 14), Font | LineSpacing));
}

TEST(BookFontWrite, DefaultFontNeedsNoFile) {
  const ReaderFont def = font("Alpha", 18);
  BookFont next;
  EXPECT_EQ(bookFontWrite(def, font("", 16, 4), def, noFile, next), BookFontWrite::None);
}

TEST(BookFontWrite, PickingTheDefaultAgainRemovesTheFile) {
  const ReaderFont def = font("Alpha", 18);
  const ReaderFont inUse = font("", 16, 4);
  BookFont next;
  EXPECT_EQ(bookFontWrite(def, inUse, def, book(inUse, Font | LineSpacing), next), BookFontWrite::Remove);
  EXPECT_EQ(next.fields, 0);
}

TEST(BookFontWrite, ANewOrChangedBookFontIsSaved) {
  const ReaderFont def = font("Alpha", 18);
  const BookFont saved = book(font("", 16), Font);
  BookFont next;
  EXPECT_EQ(bookFontWrite(font("", 16), def, def, noFile, next), BookFontWrite::Save);
  EXPECT_EQ(next, saved);
  EXPECT_EQ(bookFontWrite(font("", 14), font("", 16), def, saved, next), BookFontWrite::Save);
  EXPECT_EQ(next, book(font("", 14), Font));
  EXPECT_EQ(bookFontWrite(font("Beta", 16), font("", 16), def, saved, next), BookFontWrite::Save);
  EXPECT_EQ(next, book(font("Beta", 16), Font));
}

TEST(BookFontWrite, AFontLeftUnchangedIsNotWritten) {
  const ReaderFont def = font("Alpha", 18);
  const ReaderFont inUse = font("", 16, 4, 125);
  BookFont next;
  EXPECT_EQ(bookFontWrite(inUse, inUse, def, book(inUse, Font | LineSpacing | WordSpacing), next), BookFontWrite::None);
}

TEST(BookFontWrite, AFallbackTheReaderMadeKeepsTheFile) {
  // The saved family is missing from the card, so the book opened in the
  // default font; closing it must not drop the file.
  const ReaderFont def = font("Alpha", 18);
  const BookFont saved = book(font("Removed", 20), Font);
  BookFont next;
  EXPECT_EQ(bookFontWrite(def, def, def, saved, next), BookFontWrite::None);
  // A failed load cleared the family for this visit only.
  EXPECT_EQ(bookFontWrite(font("", 18), font("", 18), def, saved, next), BookFontWrite::None);
  // A spacing change during such a visit keeps the saved font too.
  EXPECT_EQ(bookFontWrite(font("Alpha", 18, 4), def, def, saved, next), BookFontWrite::Save);
  EXPECT_EQ(next, book(font("Removed", 20, 4), Font | LineSpacing));
}

TEST(BookFontWrite, ChangingBackWithinAVisitSavesTheLatestPick) {
  const ReaderFont def = font("Alpha", 18);
  const ReaderFont first = font("Beta", 14);
  const ReaderFont second = font("Beta", 16);
  BookFont next;
  // Picked 14 (saved), then 16: the file follows the latest pick.
  EXPECT_EQ(bookFontWrite(second, first, def, book(first, Font), next), BookFontWrite::Save);
  EXPECT_EQ(next, book(second, Font));
}

TEST(BookFontWrite, OnlySpacingThatDiffersFromTheDefaultIsStored) {
  const ReaderFont def = font("Alpha", 18);
  BookFont next;
  EXPECT_EQ(bookFontWrite(font("Alpha", 18, 4), def, def, noFile, next), BookFontWrite::Save);
  EXPECT_EQ(next, book(font("Alpha", 18, 4), LineSpacing));
  EXPECT_EQ(bookFontWrite(font("Alpha", 18, 2, 150, 1), def, def, noFile, next), BookFontWrite::Save);
  EXPECT_EQ(next, book(font("Alpha", 18, 2, 150, 1), WordSpacing | CharacterSpacing));
}

TEST(BookFontWrite, PickingTheDefaultSpacingAgainDropsOnlyThatEntry) {
  const ReaderFont def = font("Alpha", 18);
  const ReaderFont inUse = font("Beta", 16, 4, 150);
  BookFont next;
  EXPECT_EQ(bookFontWrite(font("Beta", 16, 2, 150), inUse, def, book(inUse, Font | LineSpacing | WordSpacing), next),
            BookFontWrite::Save);
  EXPECT_EQ(next, book(inUse, Font | WordSpacing));
}

TEST(BookFontWrite, AFileWithOnlyTheFontGainsSpacing) {
  // Files written before spacing was kept per book hold family and size only.
  const ReaderFont def = font("Alpha", 18);
  const ReaderFont inUse = font("Beta", 16);
  BookFont next;
  EXPECT_EQ(bookFontWrite(font("Beta", 16, 2, 100, 3), inUse, def, book(inUse, Font), next), BookFontWrite::Save);
  EXPECT_EQ(next, book(font("Beta", 16, 2, 100, 3), Font | CharacterSpacing));
}

}  // namespace
