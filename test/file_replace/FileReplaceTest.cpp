// FileReplace::replace(): a file put in the place of another never loses the one that was there,
// whichever step fails.

#include <FileReplace.h>
#include <gtest/gtest.h>

#include "HalStorage.h"

namespace {

constexpr char kBook[] = "/Books/Dune.epub";
constexpr char kAside[] = "/Books/Dune.epub.old";
constexpr char kFresh[] = "/Books/Dune.epub.part";

class FileReplaceTest : public ::testing::Test {
 protected:
  void SetUp() override { Storage.reset(); }
  void TearDown() override { Storage.reset(); }
  static void failRename(const char* from) { Storage.failRenameFrom.insert(HalStorage::key(from)); }
};

TEST_F(FileReplaceTest, WithNothingThereTheFileMovesIn) {
  Storage.put(kFresh, "new");

  EXPECT_TRUE(FileReplace::replace(kFresh, kBook));

  EXPECT_EQ(Storage.content(kBook), "new");
  EXPECT_FALSE(Storage.exists(kFresh));
}

TEST_F(FileReplaceTest, TheOldFileIsReplacedAndGone) {
  Storage.put(kBook, "old");
  Storage.put(kFresh, "new");

  EXPECT_TRUE(FileReplace::replace(kFresh, kBook));

  EXPECT_EQ(Storage.content(kBook), "new");
  EXPECT_FALSE(Storage.exists(kFresh));
  EXPECT_FALSE(Storage.exists(kAside));
}

TEST_F(FileReplaceTest, AFailedMoveInPutsTheOldFileBack) {
  Storage.put(kBook, "old");
  Storage.put(kFresh, "new");
  failRename(kFresh);

  EXPECT_FALSE(FileReplace::replace(kFresh, kBook));

  EXPECT_EQ(Storage.content(kBook), "old");
  EXPECT_EQ(Storage.content(kFresh), "new");  // the caller's to remove
  EXPECT_FALSE(Storage.exists(kAside));
}

TEST_F(FileReplaceTest, WhenPuttingItBackFailsTooTheOldFileStaysAside) {
  Storage.put(kBook, "old");
  Storage.put(kFresh, "new");
  failRename(kFresh);
  failRename(kAside);

  EXPECT_FALSE(FileReplace::replace(kFresh, kBook));

  EXPECT_EQ(Storage.content(kAside), "old");
  EXPECT_EQ(Storage.content(kFresh), "new");
}

TEST_F(FileReplaceTest, AnOldFileThatCannotBeSetAsideIsLeftAlone) {
  Storage.put(kBook, "old");
  Storage.put(kFresh, "new");
  failRename(kBook);

  EXPECT_FALSE(FileReplace::replace(kFresh, kBook));

  EXPECT_EQ(Storage.content(kBook), "old");
  EXPECT_EQ(Storage.content(kFresh), "new");
  EXPECT_FALSE(Storage.exists(kAside));
}

TEST_F(FileReplaceTest, AFileAlreadyNamedLikeTheAsideIsNeverOverwritten) {
  Storage.put(kBook, "old");
  Storage.put(kFresh, "new");
  Storage.put(kAside, "the user's");

  EXPECT_FALSE(FileReplace::replace(kFresh, kBook));

  EXPECT_EQ(Storage.content(kBook), "old");
  EXPECT_EQ(Storage.content(kFresh), "new");
  EXPECT_EQ(Storage.content(kAside), "the user's");
}

TEST_F(FileReplaceTest, AFolderIsNotReplaced) {
  Storage.putFolder(kBook);
  Storage.put(kFresh, "new");

  EXPECT_FALSE(FileReplace::replace(kFresh, kBook));

  EXPECT_TRUE(Storage.open(kBook).isDirectory());
  EXPECT_EQ(Storage.content(kFresh), "new");
  EXPECT_FALSE(Storage.exists(kAside));
}

TEST_F(FileReplaceTest, AnAsideThatCannotBeRemovedStillCountsAsReplaced) {
  Storage.put(kBook, "old");
  Storage.put(kFresh, "new");
  Storage.failRemove.insert(HalStorage::key(kAside));

  EXPECT_TRUE(FileReplace::replace(kFresh, kBook));

  EXPECT_EQ(Storage.content(kBook), "new");
  EXPECT_EQ(Storage.content(kAside), "old");
}

// A WebDAV MOVE from /Books/Dune.epub to /books/dune.EPUB names one file twice on FAT. Removing
// the destination to make room removed the source. The handlers ask the HAL first
// (HalStorage::areSameFile); replace() itself still keeps the file if one does not.
TEST_F(FileReplaceTest, TheSameFileNamedTwoWaysIsKept) {
  Storage.put(kBook, "the only copy");

  EXPECT_FALSE(FileReplace::replace(kBook, "/books/dune.EPUB"));

  EXPECT_EQ(Storage.content(kBook), "the only copy");
  EXPECT_FALSE(Storage.exists(kAside));
}

}  // namespace
