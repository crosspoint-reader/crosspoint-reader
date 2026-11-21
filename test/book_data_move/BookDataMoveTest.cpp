#include <Epub.h>
#include <HalStorage.h>
#include <gtest/gtest.h>

#include "util/BookCacheUtils.h"
#include "util/BookDataMove.h"
#include "util/BookmarkUtil.h"

namespace {
std::string bookmarks(const std::string& bookPath) { return BookmarkUtil::getBookmarkPath(bookPath); }
std::string cacheDir(const std::string& bookPath) { return Epub(bookPath, "/.crosspoint").getCachePath(); }
}  // namespace

TEST(BookDataMove, CaseOnlyBookmarkCollisionKeepsBookmarksOnMove) {
  fake::files = {{bookmarks("/Books_A.epub"), "marks"}};
  moveBookData("/Books_A.epub", "/books/A.epub");
  ASSERT_TRUE(Storage.exists(bookmarks("/books/A.epub").c_str()));
  EXPECT_EQ(fake::files.at(bookmarks("/books/A.epub")), "marks");
}

TEST(BookDataMove, CaseOnlyBookmarkCollisionKeepsBookmarksOnCopy) {
  fake::files = {{bookmarks("/Books_A.epub"), "marks"}};
  clearBookData("/books/A.epub", "/Books_A.epub");
  EXPECT_TRUE(Storage.exists(bookmarks("/Books_A.epub").c_str()));
}

TEST(BookDataMove, AccentedCaseOnlyCollisionKeepsBookmarks) {
  fake::files = {{bookmarks("/À_B.epub"), "marks"}};
  clearBookData("/à/B.epub", "/À_B.epub");
  EXPECT_TRUE(Storage.exists(bookmarks("/À_B.epub").c_str()));
  moveBookData("/À_B.epub", "/à/B.epub");
  EXPECT_TRUE(Storage.exists(bookmarks("/à/B.epub").c_str()));
}

TEST(BookDataMove, FolderNamedLikeBookLeavesOtherBooksAlone) {
  fake::files = {{"/Done.epub", fake::kDir}, {bookmarks("/Done.txt"), "marks"}};
  moveBookData("/Reading", "/Done.epub");
  EXPECT_TRUE(Storage.exists(bookmarks("/Done.txt").c_str()));
  clearBookData("/Done.epub", "/Reading");
  EXPECT_TRUE(Storage.exists(bookmarks("/Done.txt").c_str()));
}

TEST(BookDataMove, FolderOrNonBookRenameLeavesOtherBooksAlone) {
  fake::files = {{bookmarks("/Done.epub"), "marks"}, {cacheDir("/Done.epub"), ""}};
  moveBookData("/Reading", "/Done");
  moveBookData("/cover.png", "/Done.png");
  EXPECT_TRUE(Storage.exists(bookmarks("/Done.epub").c_str()));
  EXPECT_TRUE(Storage.exists(cacheDir("/Done.epub").c_str()));
}

TEST(BookDataMove, XtcDestinationLeavesEpubBookmarksAlone) {
  fake::files = {{bookmarks("/B.epub"), "marks"}};
  moveBookData("/A.xtc", "/B.xtc");
  EXPECT_TRUE(Storage.exists(bookmarks("/B.epub").c_str()));
  clearBookData("/B.xtc", "/A.epub");
  EXPECT_TRUE(Storage.exists(bookmarks("/B.epub").c_str()));
}

TEST(BookDataMove, MoveDropsRenderedPagesButKeepsPosition) {
  const std::string oldCache = cacheDir("/A.epub");
  fake::files = {{oldCache, ""},
                 {oldCache + "/progress.bin", "position"},
                 {oldCache + "/book.bin", "metadata"},
                 {oldCache + "/img_0_0.jpg", "image"},
                 {oldCache + "/sections", ""},
                 {oldCache + "/sections/0.bin", oldCache + "/img_0_0.jpg"}};
  moveBookData("/A.epub", "/Read/A.epub");
  const std::string newCache = cacheDir("/Read/A.epub");
  EXPECT_FALSE(Storage.exists(oldCache.c_str()));
  EXPECT_FALSE(Storage.exists((newCache + "/sections").c_str()));
  EXPECT_FALSE(Storage.exists((newCache + "/sections/0.bin").c_str()));
  EXPECT_EQ(fake::files.at(newCache + "/progress.bin"), "position");
  EXPECT_TRUE(Storage.exists((newCache + "/book.bin").c_str()));
  EXPECT_TRUE(Storage.exists((newCache + "/img_0_0.jpg").c_str()));
}

TEST(BookDataMove, SleepImageChangeClearsOnlyCachedSleepBitmaps) {
  fake::files = {{"/.crosspoint", fake::kDir},
                 {"/.crosspoint/sleep_ab12.bmp", "bmp"},
                 {"/.crosspoint/sleep_frame.bin", "frame"},
                 {"/.crosspoint/epub_1", fake::kDir},
                 {"/.crosspoint/epub_1/sleep_x.bmp", "nested"}};
  sleepImageChanged("/sleep.bmp");
  EXPECT_FALSE(Storage.exists("/.crosspoint/sleep_ab12.bmp"));
  EXPECT_TRUE(Storage.exists("/.crosspoint/sleep_frame.bin"));
  EXPECT_TRUE(Storage.exists("/.crosspoint/epub_1/sleep_x.bmp"));
  fake::files["/.crosspoint/sleep_cd34.bmp"] = "bmp";
  sleepImageChanged("/Books/a.epub");
  EXPECT_TRUE(Storage.exists("/.crosspoint/sleep_cd34.bmp"));
}
