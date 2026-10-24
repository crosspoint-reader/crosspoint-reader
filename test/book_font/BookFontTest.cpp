#include <gtest/gtest.h>

#include <string>

#include "ReaderFont.h"

namespace {

ReaderFont font(const char* family, const uint8_t size) {
  ReaderFont f;
  f.setFamily(family);
  f.fontPointSize = size;
  return f;
}

TEST(ReaderFont, ComparesFamilyAndSize) {
  EXPECT_EQ(font("Alpha", 14), font("Alpha", 14));
  EXPECT_NE(font("Alpha", 14), font("Alpha", 16));
  EXPECT_NE(font("Alpha", 14), font("Beta", 14));
  EXPECT_NE(font("", 14), font("Alpha", 14));
}

TEST(ReaderFont, SetFamilyTruncatesAndAcceptsNull) {
  const std::string longName(64, 'x');
  ReaderFont f = font(longName.c_str(), 14);
  EXPECT_EQ(std::string(f.sdFontFamilyName), longName.substr(0, sizeof(f.sdFontFamilyName) - 1));
  f.setFamily(nullptr);
  EXPECT_STREQ(f.sdFontFamilyName, "");
}

TEST(BookFontWrite, DefaultFontNeedsNoFile) {
  const ReaderFont def = font("Alpha", 18);
  EXPECT_EQ(bookFontWrite(def, font("", 16), def, nullptr), BookFontWrite::None);
}

TEST(BookFontWrite, PickingTheDefaultAgainRemovesTheFile) {
  const ReaderFont def = font("Alpha", 18);
  const ReaderFont saved = font("", 16);
  EXPECT_EQ(bookFontWrite(def, saved, def, &saved), BookFontWrite::Remove);
}

TEST(BookFontWrite, ANewOrChangedBookFontIsSaved) {
  const ReaderFont def = font("Alpha", 18);
  const ReaderFont saved = font("", 16);
  EXPECT_EQ(bookFontWrite(font("", 16), def, def, nullptr), BookFontWrite::Save);
  EXPECT_EQ(bookFontWrite(font("", 14), saved, def, &saved), BookFontWrite::Save);
  EXPECT_EQ(bookFontWrite(font("Beta", 16), saved, def, &saved), BookFontWrite::Save);
}

TEST(BookFontWrite, AFontLeftUnchangedIsNotWritten) {
  const ReaderFont def = font("Alpha", 18);
  const ReaderFont saved = font("", 16);
  EXPECT_EQ(bookFontWrite(saved, saved, def, &saved), BookFontWrite::None);
}

TEST(BookFontWrite, AFallbackTheReaderMadeKeepsTheFile) {
  // The saved family is missing from the card, so the book opened in the
  // default font; closing it must not drop the file.
  const ReaderFont def = font("Alpha", 18);
  const ReaderFont saved = font("Removed", 20);
  EXPECT_EQ(bookFontWrite(def, def, def, &saved), BookFontWrite::None);
  // A failed load cleared the family for this visit only.
  EXPECT_EQ(bookFontWrite(font("", 18), font("", 18), def, &saved), BookFontWrite::None);
}

TEST(BookFontWrite, ChangingBackWithinAVisitSavesTheLatestPick) {
  const ReaderFont def = font("Alpha", 18);
  const ReaderFont first = font("Beta", 14);
  const ReaderFont second = font("Beta", 16);
  // Picked 14 (saved), then 16: the file follows the latest pick.
  EXPECT_EQ(bookFontWrite(second, first, def, &first), BookFontWrite::Save);
}

}  // namespace
