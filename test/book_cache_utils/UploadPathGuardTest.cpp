#include <gtest/gtest.h>

#include <limits>
#include <string>

#include "UploadPathGuard.h"

TEST(UploadPathGuard, AcceptsOrdinaryAndUtf8LeafNames) {
  EXPECT_TRUE(UploadPathGuard::isSafeLeafName("book.epub"));
  EXPECT_TRUE(UploadPathGuard::isSafeLeafName("Truyện tiếng Việt 01.epub"));
  EXPECT_TRUE(UploadPathGuard::isSafeLeafName((std::string(225, 'a') + ".epub").c_str()));
}

TEST(UploadPathGuard, RejectsTraversalHiddenAndInvalidLeafNames) {
  for (const char* name : {"", ".", "..", "../settings.json", "dir/book.epub", "dir\\book.epub", ".crosspoint",
                           "book.epub.", "book.epub ", "bad:name.epub", "bad\nname.epub"}) {
    EXPECT_FALSE(UploadPathGuard::isSafeLeafName(name)) << name;
  }

  const std::string invalidUtf8 = std::string("bad-") + static_cast<char>(0xFF) + ".epub";
  EXPECT_FALSE(UploadPathGuard::isSafeLeafName(invalidUtf8.c_str()));
}

TEST(UploadPathGuard, ValidatesUtf8WithoutReadingPastTruncatedInput) {
  const std::string suffix = ".epub";
  for (const std::string& bytes :
       {std::string("\xC2", 1), std::string("\xE2\x82", 2), std::string("\xF0\x9F\x93", 3)}) {
    EXPECT_FALSE(UploadPathGuard::isSafeLeafName(bytes.c_str()));
  }
  for (const std::string& bytes :
       {std::string("\xC0\xAF", 2), std::string("\xE0\x80\x80", 3), std::string("\xF0\x80\x80\x80", 4),
        std::string("\xED\xA0\x80", 3), std::string("\xF4\x90\x80\x80", 4)}) {
    EXPECT_FALSE(UploadPathGuard::isSafeLeafName((bytes + suffix).c_str()));
  }
  EXPECT_TRUE(UploadPathGuard::isSafeLeafName((std::string("\xEF\xBF\xBD", 3) + suffix).c_str()));
}

TEST(UploadPathGuard, RejectsProtectedAndTransactionArtifactNamesCaseInsensitively) {
  for (const char* name :
       {"System Volume Information", "system volume information", "XTCache", "xtcache", "book.epub.crossvi-upload.tmp",
        "BOOK.EPUB.CROSSVI-REPLACE.BAK", "book.epub.davtmp", "book.epub.crossvi-invalid-publish.2"}) {
    EXPECT_FALSE(UploadPathGuard::isSafeLeafName(name)) << name;
  }
}

TEST(UploadPathGuard, ReservesSpaceForHiddenTransactionSiblings) {
  EXPECT_TRUE(UploadPathGuard::isSafeLeafName(std::string(UploadPathGuard::MAX_LEAF_BYTES, 'a').c_str()));
  EXPECT_FALSE(UploadPathGuard::isSafeLeafName(std::string(UploadPathGuard::MAX_LEAF_BYTES + 1, 'a').c_str()));
}

TEST(UploadPathGuard, ValidatesEveryFolderSegment) {
  EXPECT_TRUE(UploadPathGuard::isSafeAbsolutePath("/"));
  EXPECT_TRUE(UploadPathGuard::isSafeAbsolutePath("/Books/Tiếng Việt/"));
  for (const char* path :
       {"", "Books", "/Books//Nested", "/Books/../.crosspoint", "/Books/./Nested", "/Books\\..\\.crosspoint",
        "/system volume information/books", "/Books/.hidden", "/Books/book.epub.crossvi-replace.pending"}) {
    EXPECT_FALSE(UploadPathGuard::isSafeAbsolutePath(path)) << path;
  }
  EXPECT_FALSE(UploadPathGuard::isSafeAbsolutePath("/", false));
}

TEST(UploadPathGuard, ParsesRepresentableDecimalSizesWithoutWrapping) {
  size_t parsed = 99;
  EXPECT_TRUE(UploadPathGuard::parseSize("0", parsed));
  EXPECT_EQ(parsed, 0U);
  const std::string maximum = std::to_string(std::numeric_limits<size_t>::max());
  EXPECT_TRUE(UploadPathGuard::parseSize(maximum.c_str(), parsed));
  EXPECT_EQ(parsed, std::numeric_limits<size_t>::max());

  EXPECT_FALSE(UploadPathGuard::parseSize("", parsed));
  EXPECT_FALSE(UploadPathGuard::parseSize("+1", parsed));
  EXPECT_FALSE(UploadPathGuard::parseSize("-1", parsed));
  EXPECT_FALSE(UploadPathGuard::parseSize("12x", parsed));
  EXPECT_FALSE(UploadPathGuard::parseSize(("1" + maximum).c_str(), parsed));
}
