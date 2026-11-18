#include <gtest/gtest.h>

#include <string>

#include "OpdsFilename.h"
#include "StringUtils.h"

namespace {

TEST(OpdsFilename, AuthorTitleIsDefaultOrder) {
  EXPECT_EQ(opdsBookFilename("J. Doe", "My Book", OpdsFilenameFormat::AuthorTitle), "J. Doe - My Book.epub");
}

TEST(OpdsFilename, TitleAuthorSwapsOrder) {
  EXPECT_EQ(opdsBookFilename("J. Doe", "My Book", OpdsFilenameFormat::TitleAuthor), "My Book - J. Doe.epub");
}

TEST(OpdsFilename, TitleOnlyIgnoresAuthor) {
  EXPECT_EQ(opdsBookFilename("J. Doe", "My Book", OpdsFilenameFormat::TitleOnly), "My Book.epub");
}

TEST(OpdsFilename, EmptyAuthorCollapsesToTitleForEveryFormat) {
  EXPECT_EQ(opdsBookFilename("", "My Book", OpdsFilenameFormat::AuthorTitle), "My Book.epub");
  EXPECT_EQ(opdsBookFilename("", "My Book", OpdsFilenameFormat::TitleAuthor), "My Book.epub");
  EXPECT_EQ(opdsBookFilename("", "My Book", OpdsFilenameFormat::TitleOnly), "My Book.epub");
}

TEST(OpdsFilename, IllegalCharactersAreSanitized) {
  // '/' ':' '*' '?' etc. are replaced with '_' by sanitizeFilename.
  EXPECT_EQ(opdsBookFilename("A/B", "C:D*E?", OpdsFilenameFormat::AuthorTitle), "A_B - C_D_E_.epub");
}

TEST(OpdsFilename, EmptyAuthorAndTitleFallsBackToBook) {
  // sanitizeFilename returns "book" when nothing usable remains.
  EXPECT_EQ(opdsBookFilename("", "", OpdsFilenameFormat::AuthorTitle), "book.epub");
  EXPECT_EQ(opdsBookFilename("", "", OpdsFilenameFormat::TitleOnly), "book.epub");
}

TEST(OpdsFilename, LongNameIsTruncatedToByteBudgetBeforeExtension) {
  // sanitizeFilename caps the base at 100 bytes; ".epub" is appended after.
  const std::string longTitle(200, 'a');
  const std::string result = opdsBookFilename("", longTitle, OpdsFilenameFormat::TitleOnly);
  EXPECT_EQ(result, std::string(100, 'a') + ".epub");
  EXPECT_EQ(result.size(), 105u);
}

TEST(OpdsFilename, UnknownFormatValueFallsBackToAuthorTitle) {
  // Defensive: a persisted value outside the enum still yields a valid name.
  const auto bogus = static_cast<OpdsFilenameFormat>(99);
  EXPECT_EQ(opdsBookFilename("J. Doe", "My Book", bogus), "J. Doe - My Book.epub");
}

TEST(PluginFilename, PreservesWebDavExtension) {
  const std::string name = "Shadow Divers (Robert Kurson) (z-library.sk, 1lib.sk, z-lib.sk).epub";
  EXPECT_EQ(StringUtils::sanitizeFilenamePreservingExtension(name), name);
}

TEST(PluginFilename, TruncatesStemBeforeExtension) {
  const std::string name = std::string(200, 'a') + ".epub";
  const std::string result = StringUtils::sanitizeFilenamePreservingExtension(name);
  EXPECT_EQ(result, std::string(95, 'a') + ".epub");
  EXPECT_EQ(result.size(), 100u);
}

TEST(PluginFilename, TruncatesUtf8StemOnCodepointBoundary) {
  std::string title;
  for (int i = 0; i < 20; i++) title += "\xC3\xA9";
  title += ".epub";
  const std::string result = StringUtils::sanitizeFilenamePreservingExtension(title, 20);
  EXPECT_EQ(result, title.substr(0, 14) + ".epub");
  EXPECT_EQ(result.size(), 19u);
  EXPECT_EQ(result.substr(result.size() - 5), ".epub");
}

}  // namespace
