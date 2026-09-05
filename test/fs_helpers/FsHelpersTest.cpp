#include <FsHelpers.h>
#include <gtest/gtest.h>

// bookCachePathFor() and clearBookCache() dispatch on file extension to decide whether a
// book path is a supported TXT-family book (see src/util/BookDataMove.cpp and
// src/util/BookCacheUtils.cpp). Markdown books are read through the same TxtReaderActivity
// as .txt books, so this predicate must recognise both.
TEST(FsHelpersHasPlainTextBookExtension, RecognisesMarkdown) {
  EXPECT_TRUE(FsHelpers::hasPlainTextBookExtension(std::string_view{"book.md"}));
  EXPECT_TRUE(FsHelpers::hasPlainTextBookExtension(std::string_view{"BOOK.MD"}));
}

TEST(FsHelpersHasPlainTextBookExtension, RecognisesTxt) {
  EXPECT_TRUE(FsHelpers::hasPlainTextBookExtension(std::string_view{"notes.txt"}));
}

TEST(FsHelpersHasPlainTextBookExtension, RejectsOtherExtensions) {
  EXPECT_FALSE(FsHelpers::hasPlainTextBookExtension(std::string_view{"book.epub"}));
  EXPECT_FALSE(FsHelpers::hasPlainTextBookExtension(std::string_view{"book.bak"}));
}
