#include <HalStorage.h>
#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

#include "BookCacheUtils.h"
#include "BookmarkUtil.h"

namespace {

constexpr char CACHE_PATH[] = "/.crosspoint/epub_test";
constexpr char STAGING_PATH[] = "/.crosspoint/.crossvi_clear_epub_test";
constexpr char MOVED_CACHE_PATH[] = "/.crosspoint/epub_moved";
constexpr char MOVE_STAGING_PATH[] = "/.crosspoint/.crossvi_move_epub_moved";
constexpr char MOVE_CLEANUP_PATH[] = "/.crosspoint/.crossvi_cleanup_epub_moved";
constexpr char SOURCE_BOOK_PATH[] = "/books/source.epub";
constexpr char DESTINATION_BOOK_PATH[] = "/read/source.epub";

std::vector<unsigned char> bytes(const unsigned char value) { return {value, static_cast<unsigned char>(value + 1)}; }

std::vector<unsigned char> emptyBookmarks() {
  constexpr char EMPTY[] = "{\"bookmarks\":[]}";
  return std::vector<unsigned char>(EMPTY, EMPTY + sizeof(EMPTY) - 1);
}

class BookCacheUtilsTest : public testing::Test {
 protected:
  void SetUp() override {
    Storage.reset();
    Storage.addDirectory(CACHE_PATH);
    Storage.addDirectory("/read");
    Storage.addDirectory(BookmarkUtil::getBookmarksDir());
    Storage.setFile(SOURCE_BOOK_PATH, bytes(240));
  }

  void put(const std::string& name, const unsigned char value) {
    Storage.setFile(std::string(CACHE_PATH) + "/" + name, bytes(value));
  }

  void expectPreserved(const std::map<std::string, std::vector<unsigned char>>& expected) {
    for (const auto& [name, content] : expected) {
      const std::string path = std::string(CACHE_PATH) + "/" + name;
      ASSERT_TRUE(Storage.exists(path.c_str())) << name;
      EXPECT_EQ(Storage.file(path), content) << name;
    }
  }
};

TEST_F(BookCacheUtilsTest, ClearsDerivedCacheAndPreservesAllSupportedUserState) {
  const std::vector<std::string> names = {"stats.bin",
                                          "stats_v5.bin",
                                          "stats_v5.bin.bak",
                                          "stats_v5.bin.tmp",
                                          "stats_v6.bin",
                                          "stats_v6.bin.bak",
                                          "stats_v6.bin.tmp",
                                          "crossvi_reader_settings.bin",
                                          "crossvi_reader_settings.bin.bak",
                                          "crossvi_reader_settings.bin.tmp"};
  std::map<std::string, std::vector<unsigned char>> expected;
  unsigned char value = 1;
  for (const std::string& name : names) {
    put(name, value);
    expected.emplace(name, bytes(value++));
  }
  put("metadata.bin", 90);
  put("progress.bin", 91);
  Storage.addDirectory(std::string(CACHE_PATH) + "/sections");
  Storage.setFile(std::string(CACHE_PATH) + "/sections/1.bin", bytes(92));

  ASSERT_TRUE(clearBookCacheDirectoryPreservingUserState(CACHE_PATH));

  expectPreserved(expected);
  EXPECT_EQ(Storage.filesUnder(CACHE_PATH).size(), expected.size());
  EXPECT_FALSE(Storage.exists((std::string(CACHE_PATH) + "/metadata.bin").c_str()));
  EXPECT_FALSE(Storage.exists((std::string(CACHE_PATH) + "/progress.bin").c_str()));
  EXPECT_FALSE(Storage.exists((std::string(CACHE_PATH) + "/sections").c_str()));
  EXPECT_FALSE(Storage.exists(STAGING_PATH));
}

TEST_F(BookCacheUtilsTest, WhitelistRequiresExactNamesAndNumericStatsVersions) {
  const std::vector<std::string> preserved = {"stats.bin",
                                              "stats_v0.bin",
                                              "stats_v0000000000001.bin.tmp",
                                              "stats_v42.bin.bak",
                                              "crossvi_reader_settings.bin",
                                              "crossvi_reader_settings.bin.bak",
                                              "crossvi_reader_settings.bin.tmp"};
  const std::vector<std::string> removed = {
      "stats.bin.bak", "stats.bin.tmp",    "stats_v.bin",          "stats_vx.bin",
      "stats_v1x.bin", "stats_v5.bin.old", "stats_v5.bin.bak.old", "stats_v-1.bin",
      "Stats_v5.bin",  "mystats_v5.bin",   "reader_settings.bin",  "crossvi_reader_settings.bin.old"};

  unsigned char value = 1;
  for (const std::string& name : preserved) put(name, value++);
  for (const std::string& name : removed) put(name, value++);

  ASSERT_TRUE(clearBookCacheDirectoryPreservingUserState(CACHE_PATH));

  for (const std::string& name : preserved) {
    EXPECT_TRUE(Storage.exists((std::string(CACHE_PATH) + "/" + name).c_str())) << name;
  }
  for (const std::string& name : removed) {
    EXPECT_FALSE(Storage.exists((std::string(CACHE_PATH) + "/" + name).c_str())) << name;
  }
}

TEST_F(BookCacheUtilsTest, StageFailureRollsBackWithoutClearingAnything) {
  put("crossvi_reader_settings.bin", 1);
  put("stats_v5.bin", 2);
  put("metadata.bin", 3);
  const auto settings = Storage.file(std::string(CACHE_PATH) + "/crossvi_reader_settings.bin");
  const auto stats = Storage.file(std::string(CACHE_PATH) + "/stats_v5.bin");
  Storage.failRenameOnCall(2);

  EXPECT_FALSE(clearBookCacheDirectoryPreservingUserState(CACHE_PATH));

  EXPECT_EQ(Storage.file(std::string(CACHE_PATH) + "/crossvi_reader_settings.bin"), settings);
  EXPECT_EQ(Storage.file(std::string(CACHE_PATH) + "/stats_v5.bin"), stats);
  EXPECT_TRUE(Storage.exists((std::string(CACHE_PATH) + "/metadata.bin").c_str()));
  EXPECT_FALSE(Storage.exists(STAGING_PATH));
}

TEST_F(BookCacheUtilsTest, ClearFailureRestoresStateAndLeavesDerivedEntryForRetry) {
  put("stats_v5.bin", 1);
  put("metadata.bin", 2);
  const auto stats = Storage.file(std::string(CACHE_PATH) + "/stats_v5.bin");
  Storage.failDeletePathOnce(std::string(CACHE_PATH) + "/metadata.bin");

  EXPECT_FALSE(clearBookCacheDirectoryPreservingUserState(CACHE_PATH));

  EXPECT_EQ(Storage.file(std::string(CACHE_PATH) + "/stats_v5.bin"), stats);
  EXPECT_TRUE(Storage.exists((std::string(CACHE_PATH) + "/metadata.bin").c_str()));
  EXPECT_FALSE(Storage.exists(STAGING_PATH));
}

TEST_F(BookCacheUtilsTest, InterruptedRestoreIsRecoveredOnNextCall) {
  put("crossvi_reader_settings.bin", 1);
  put("stats_v5.bin", 2);
  put("metadata.bin", 3);
  const auto settings = Storage.file(std::string(CACHE_PATH) + "/crossvi_reader_settings.bin");
  const auto stats = Storage.file(std::string(CACHE_PATH) + "/stats_v5.bin");

  // Two moves into staging, then fail the first restore.
  Storage.failRenameOnCall(3);
  EXPECT_FALSE(clearBookCacheDirectoryPreservingUserState(CACHE_PATH));
  EXPECT_TRUE(Storage.exists(STAGING_PATH));

  ASSERT_TRUE(clearBookCacheDirectoryPreservingUserState(CACHE_PATH));
  EXPECT_EQ(Storage.file(std::string(CACHE_PATH) + "/crossvi_reader_settings.bin"), settings);
  EXPECT_EQ(Storage.file(std::string(CACHE_PATH) + "/stats_v5.bin"), stats);
  EXPECT_FALSE(Storage.exists((std::string(CACHE_PATH) + "/metadata.bin").c_str()));
  EXPECT_FALSE(Storage.exists(STAGING_PATH));
}

TEST_F(BookCacheUtilsTest, OpenTimeRecoveryRestoresStateWithoutClearingDerivedCache) {
  const auto settings = bytes(1);
  const auto stats = bytes(2);
  Storage.addDirectory(STAGING_PATH);
  Storage.setFile(std::string(STAGING_PATH) + "/crossvi_reader_settings.bin", settings);
  Storage.setFile(std::string(STAGING_PATH) + "/stats_v5.bin", stats);
  put("metadata.bin", 3);

  ASSERT_TRUE(recoverBookCacheUserState(CACHE_PATH, SOURCE_BOOK_PATH));

  EXPECT_EQ(Storage.file(std::string(CACHE_PATH) + "/crossvi_reader_settings.bin"), settings);
  EXPECT_EQ(Storage.file(std::string(CACHE_PATH) + "/stats_v5.bin"), stats);
  EXPECT_TRUE(Storage.exists((std::string(CACHE_PATH) + "/metadata.bin").c_str()));
  EXPECT_FALSE(Storage.exists(STAGING_PATH));
}

TEST_F(BookCacheUtilsTest, OpenTimeRecoveryFailsClosedOnConflictingState) {
  put("stats_v5.bin", 1);
  Storage.addDirectory(STAGING_PATH);
  Storage.setFile(std::string(STAGING_PATH) + "/stats_v5.bin", bytes(2));

  EXPECT_FALSE(recoverBookCacheUserState(CACHE_PATH, SOURCE_BOOK_PATH));
  EXPECT_EQ(Storage.file(std::string(CACHE_PATH) + "/stats_v5.bin"), bytes(1));
  EXPECT_EQ(Storage.file(std::string(STAGING_PATH) + "/stats_v5.bin"), bytes(2));
}

TEST_F(BookCacheUtilsTest, UnexpectedStagingContentIsNeverDeleted) {
  put("stats_v5.bin", 1);
  Storage.addDirectory(STAGING_PATH);
  Storage.setFile(std::string(STAGING_PATH) + "/not-user-state.bin", bytes(9));

  EXPECT_FALSE(clearBookCacheDirectoryPreservingUserState(CACHE_PATH));

  EXPECT_TRUE(Storage.exists((std::string(CACHE_PATH) + "/stats_v5.bin").c_str()));
  EXPECT_TRUE(Storage.exists((std::string(STAGING_PATH) + "/not-user-state.bin").c_str()));
}

TEST_F(BookCacheUtilsTest, ReplacementDiscardsVerifiedClearStagingBeforeItCanRevive) {
  put("stats_v5.bin", 1);
  Storage.addDirectory(STAGING_PATH);
  Storage.setFile(std::string(STAGING_PATH) + "/crossvi_reader_settings.bin", bytes(2));

  ASSERT_TRUE(resetBookCacheUserStateAfterReplacement(CACHE_PATH, SOURCE_BOOK_PATH));
  EXPECT_FALSE(Storage.exists(CACHE_PATH));
  EXPECT_FALSE(Storage.exists(STAGING_PATH));
  ASSERT_TRUE(recoverBookCacheUserState(CACHE_PATH, SOURCE_BOOK_PATH));
  EXPECT_FALSE(Storage.exists((std::string(CACHE_PATH) + "/stats_v5.bin").c_str()));
  EXPECT_FALSE(Storage.exists((std::string(CACHE_PATH) + "/crossvi_reader_settings.bin").c_str()));
}

TEST_F(BookCacheUtilsTest, ReplacementNeverDeletesAPreexistingDiscardDirectoryByNameAlone) {
  const std::string existingDiscard = "/.crosspoint/.crossvi_discard_epub_test";
  Storage.addDirectory(existingDiscard);
  Storage.setFile(existingDiscard + "/unknown.bin", bytes(77));

  ASSERT_TRUE(resetBookCacheUserStateAfterReplacement(CACHE_PATH, SOURCE_BOOK_PATH));

  EXPECT_EQ(Storage.file(existingDiscard + "/unknown.bin"), bytes(77));
  EXPECT_FALSE(Storage.exists(CACHE_PATH));
}

TEST_F(BookCacheUtilsTest, ReplacementPreservesUnexpectedClearStagingAndFailsClosed) {
  Storage.addDirectory(STAGING_PATH);
  Storage.setFile(std::string(STAGING_PATH) + "/unknown.bin", bytes(9));

  EXPECT_FALSE(resetBookCacheUserStateAfterReplacement(CACHE_PATH, SOURCE_BOOK_PATH));
  EXPECT_TRUE(Storage.exists(CACHE_PATH));
  EXPECT_EQ(Storage.file(std::string(STAGING_PATH) + "/unknown.bin"), bytes(9));
}

TEST_F(BookCacheUtilsTest, ReplacementPreservesCleanupWithAMalformedOwnershipMarker) {
  put("stats_v5.bin", 8);
  ASSERT_TRUE(prepareBookCacheUserStateMove(CACHE_PATH, MOVED_CACHE_PATH, SOURCE_BOOK_PATH, DESTINATION_BOOK_PATH));
  Storage.addDirectory(MOVED_CACHE_PATH);
  Storage.setFile(std::string(MOVED_CACHE_PATH) + "/stats_v5.bin", bytes(8));
  ASSERT_TRUE(Storage.rename(MOVE_STAGING_PATH, MOVE_CLEANUP_PATH));
  const std::string marker = std::string(MOVE_CLEANUP_PATH) + "/.crossvi_move_ready";
  const std::vector<unsigned char> malformed{0x01, 0x02, 0x03};
  Storage.setFile(marker, malformed);

  EXPECT_FALSE(resetBookCacheUserStateAfterReplacement(MOVED_CACHE_PATH, DESTINATION_BOOK_PATH));

  EXPECT_EQ(Storage.file(marker), malformed);
  EXPECT_TRUE(Storage.exists(MOVE_CLEANUP_PATH));
  EXPECT_FALSE(Storage.exists(MOVED_CACHE_PATH));
}

TEST_F(BookCacheUtilsTest, MovesOnlyVerifiedUserStateAndLeavesTheSourceAuthoritative) {
  put("stats_v5.bin", 1);
  put("crossvi_reader_settings.bin", 2);
  put("progress.bin", 3);
  put("book.bin", 4);
  const std::string sourceBookmark = BookmarkUtil::getBookmarkPath(SOURCE_BOOK_PATH);
  const std::string destinationBookmark = BookmarkUtil::getBookmarkPath(DESTINATION_BOOK_PATH);
  Storage.setFile(sourceBookmark, bytes(6));
  Storage.addDirectory(std::string(CACHE_PATH) + "/sections");
  Storage.setFile(std::string(CACHE_PATH) + "/sections/0.bin", bytes(5));

  ASSERT_TRUE(prepareBookCacheUserStateMove(CACHE_PATH, MOVED_CACHE_PATH, SOURCE_BOOK_PATH, DESTINATION_BOOK_PATH));
  EXPECT_TRUE(Storage.exists((std::string(CACHE_PATH) + "/stats_v5.bin").c_str()));
  EXPECT_TRUE(Storage.exists((std::string(CACHE_PATH) + "/book.bin").c_str()));
  EXPECT_TRUE(Storage.exists((std::string(MOVE_STAGING_PATH) + "/stats_v5.bin").c_str()));
  EXPECT_TRUE(Storage.exists((std::string(MOVE_STAGING_PATH) + "/crossvi_reader_settings.bin").c_str()));
  EXPECT_TRUE(Storage.exists((std::string(MOVE_STAGING_PATH) + "/progress.bin").c_str()));
  EXPECT_FALSE(Storage.exists((std::string(MOVE_STAGING_PATH) + "/book.bin").c_str()));
  EXPECT_EQ(Storage.file(std::string(MOVE_STAGING_PATH) + "/.crossvi_bookmark.json"), bytes(6));

  ASSERT_TRUE(Storage.rename(SOURCE_BOOK_PATH, DESTINATION_BOOK_PATH));
  ASSERT_TRUE(finalizeBookCacheUserStateMove(CACHE_PATH, MOVED_CACHE_PATH, SOURCE_BOOK_PATH, DESTINATION_BOOK_PATH));
  EXPECT_EQ(Storage.file(std::string(MOVED_CACHE_PATH) + "/stats_v5.bin"), bytes(1));
  EXPECT_EQ(Storage.file(std::string(MOVED_CACHE_PATH) + "/crossvi_reader_settings.bin"), bytes(2));
  EXPECT_EQ(Storage.file(std::string(MOVED_CACHE_PATH) + "/progress.bin"), bytes(3));
  EXPECT_FALSE(Storage.exists((std::string(MOVED_CACHE_PATH) + "/book.bin").c_str()));
  EXPECT_EQ(Storage.file(destinationBookmark), bytes(6));
  EXPECT_EQ(Storage.file(sourceBookmark), bytes(6));
  EXPECT_FALSE(Storage.exists(MOVE_STAGING_PATH));
  EXPECT_TRUE(Storage.exists((std::string(CACHE_PATH) + "/stats_v5.bin").c_str()));
  ASSERT_TRUE(completeBookCacheUserStateMove(CACHE_PATH, MOVED_CACHE_PATH, SOURCE_BOOK_PATH, DESTINATION_BOOK_PATH));
  EXPECT_FALSE(Storage.exists(sourceBookmark.c_str()));
  EXPECT_EQ(Storage.file(destinationBookmark), bytes(6));
}

TEST_F(BookCacheUtilsTest, OpenTimeRecoveryPublishesACompletedStateMove) {
  put("stats_v5.bin", 8);
  put("progress.bin", 9);
  const std::string sourceBookmark = BookmarkUtil::getBookmarkPath(SOURCE_BOOK_PATH);
  const std::string destinationBookmark = BookmarkUtil::getBookmarkPath(DESTINATION_BOOK_PATH);
  Storage.setFile(sourceBookmark, bytes(10));
  ASSERT_TRUE(prepareBookCacheUserStateMove(CACHE_PATH, MOVED_CACHE_PATH, SOURCE_BOOK_PATH, DESTINATION_BOOK_PATH));

  ASSERT_TRUE(Storage.rename(SOURCE_BOOK_PATH, DESTINATION_BOOK_PATH));

  ASSERT_TRUE(recoverBookCacheUserState(MOVED_CACHE_PATH, DESTINATION_BOOK_PATH));
  EXPECT_EQ(Storage.file(std::string(MOVED_CACHE_PATH) + "/stats_v5.bin"), bytes(8));
  EXPECT_EQ(Storage.file(std::string(MOVED_CACHE_PATH) + "/progress.bin"), bytes(9));
  EXPECT_FALSE(Storage.exists(MOVE_STAGING_PATH));
  EXPECT_FALSE(Storage.exists(CACHE_PATH));
  EXPECT_FALSE(Storage.exists(sourceBookmark.c_str()));
  EXPECT_EQ(Storage.file(destinationBookmark), bytes(10));
}

TEST_F(BookCacheUtilsTest, ReplacementDiscardsOwnedMoveStagingWithoutTouchingLiveSource) {
  put("stats_v5.bin", 8);
  ASSERT_TRUE(prepareBookCacheUserStateMove(CACHE_PATH, MOVED_CACHE_PATH, SOURCE_BOOK_PATH, DESTINATION_BOOK_PATH));
  Storage.setFile(DESTINATION_BOOK_PATH, bytes(200));  // unrelated replacement at the prepared destination

  ASSERT_TRUE(resetBookCacheUserStateAfterReplacement(MOVED_CACHE_PATH, DESTINATION_BOOK_PATH));
  EXPECT_FALSE(Storage.exists(MOVE_STAGING_PATH));
  EXPECT_TRUE(Storage.exists(SOURCE_BOOK_PATH));
  EXPECT_EQ(Storage.file(std::string(CACHE_PATH) + "/stats_v5.bin"), bytes(8));
  ASSERT_TRUE(recoverBookCacheUserState(MOVED_CACHE_PATH, DESTINATION_BOOK_PATH));
  EXPECT_FALSE(Storage.exists((std::string(MOVED_CACHE_PATH) + "/stats_v5.bin").c_str()));
}

TEST_F(BookCacheUtilsTest, ReplacementAfterRenameDiscardsOldSourceDuplicates) {
  put("stats_v5.bin", 8);
  Storage.setFile(BookmarkUtil::getBookmarkPath(SOURCE_BOOK_PATH), bytes(10));
  ASSERT_TRUE(prepareBookCacheUserStateMove(CACHE_PATH, MOVED_CACHE_PATH, SOURCE_BOOK_PATH, DESTINATION_BOOK_PATH));
  ASSERT_TRUE(Storage.rename(SOURCE_BOOK_PATH, DESTINATION_BOOK_PATH));

  ASSERT_TRUE(resetBookCacheUserStateAfterReplacement(MOVED_CACHE_PATH, DESTINATION_BOOK_PATH));
  EXPECT_FALSE(Storage.exists(MOVE_STAGING_PATH));
  EXPECT_FALSE(Storage.exists(CACHE_PATH));
  EXPECT_EQ(Storage.file(BookmarkUtil::getBookmarkPath(SOURCE_BOOK_PATH)), emptyBookmarks());
}

TEST_F(BookCacheUtilsTest, ReplacingMoveSourceCancelsOwnedPreRenameDestinationStaging) {
  put("stats_v5.bin", 8);
  ASSERT_TRUE(prepareBookCacheUserStateMove(CACHE_PATH, MOVED_CACHE_PATH, SOURCE_BOOK_PATH, DESTINATION_BOOK_PATH));
  ASSERT_TRUE(Storage.remove(SOURCE_BOOK_PATH));

  ASSERT_TRUE(resetBookCacheUserStateAfterReplacement(CACHE_PATH, SOURCE_BOOK_PATH));
  EXPECT_FALSE(Storage.exists(MOVE_STAGING_PATH));
  EXPECT_EQ(Storage.file(BookmarkUtil::getBookmarkPath(DESTINATION_BOOK_PATH)), emptyBookmarks());
}

TEST_F(BookCacheUtilsTest, ReplacingMoveSourcePreservesAValidMarkerUnderTheWrongStagingKey) {
  put("stats_v5.bin", 8);
  ASSERT_TRUE(prepareBookCacheUserStateMove(CACHE_PATH, MOVED_CACHE_PATH, SOURCE_BOOK_PATH, DESTINATION_BOOK_PATH));
  constexpr char WRONG_STAGING_PATH[] = "/.crosspoint/.crossvi_move_unrelated";
  ASSERT_TRUE(Storage.rename(MOVE_STAGING_PATH, WRONG_STAGING_PATH));
  ASSERT_TRUE(Storage.remove(SOURCE_BOOK_PATH));

  ASSERT_TRUE(resetBookCacheUserStateAfterReplacement(CACHE_PATH, SOURCE_BOOK_PATH));

  EXPECT_TRUE(Storage.exists(WRONG_STAGING_PATH));
  EXPECT_FALSE(Storage.exists(BookmarkUtil::getBookmarkPath(DESTINATION_BOOK_PATH).c_str()));
}

TEST_F(BookCacheUtilsTest, DestinationOpenFinishesCleanupAfterPublishReset) {
  put("stats_v5.bin", 8);
  const std::string sourceBookmark = BookmarkUtil::getBookmarkPath(SOURCE_BOOK_PATH);
  const std::string destinationBookmark = BookmarkUtil::getBookmarkPath(DESTINATION_BOOK_PATH);
  Storage.setFile(sourceBookmark, bytes(10));
  ASSERT_TRUE(prepareBookCacheUserStateMove(CACHE_PATH, MOVED_CACHE_PATH, SOURCE_BOOK_PATH, DESTINATION_BOOK_PATH));
  ASSERT_TRUE(Storage.rename(SOURCE_BOOK_PATH, DESTINATION_BOOK_PATH));
  ASSERT_TRUE(finalizeBookCacheUserStateMove(CACHE_PATH, MOVED_CACHE_PATH, SOURCE_BOOK_PATH, DESTINATION_BOOK_PATH));
  ASSERT_TRUE(Storage.exists((std::string(MOVED_CACHE_PATH) + "/.crossvi_move_ready").c_str()));
  ASSERT_TRUE(Storage.exists(CACHE_PATH));
  ASSERT_TRUE(Storage.exists(sourceBookmark.c_str()));

  ASSERT_TRUE(recoverBookCacheUserState(MOVED_CACHE_PATH, DESTINATION_BOOK_PATH));
  EXPECT_FALSE(Storage.exists(CACHE_PATH));
  EXPECT_FALSE(Storage.exists(sourceBookmark.c_str()));
  EXPECT_EQ(Storage.file(std::string(MOVED_CACHE_PATH) + "/stats_v5.bin"), bytes(8));
  EXPECT_EQ(Storage.file(destinationBookmark), bytes(10));
  EXPECT_FALSE(Storage.exists((std::string(MOVED_CACHE_PATH) + "/.crossvi_move_ready").c_str()));
}

TEST_F(BookCacheUtilsTest, PreparedStateMoveNeverOverwritesAnExistingDestination) {
  put("stats_v5.bin", 1);
  Storage.addDirectory(MOVED_CACHE_PATH);
  Storage.setFile(std::string(MOVED_CACHE_PATH) + "/stats_v5.bin", bytes(7));

  EXPECT_FALSE(prepareBookCacheUserStateMove(CACHE_PATH, MOVED_CACHE_PATH, SOURCE_BOOK_PATH, DESTINATION_BOOK_PATH));
  EXPECT_EQ(Storage.file(std::string(MOVED_CACHE_PATH) + "/stats_v5.bin"), bytes(7));
  EXPECT_FALSE(Storage.exists(MOVE_STAGING_PATH));
}

TEST_F(BookCacheUtilsTest, RecoveryNeverPublishesBeforeTheBookRenameBoundary) {
  put("stats_v5.bin", 8);
  ASSERT_TRUE(prepareBookCacheUserStateMove(CACHE_PATH, MOVED_CACHE_PATH, SOURCE_BOOK_PATH, DESTINATION_BOOK_PATH));

  EXPECT_FALSE(recoverBookCacheUserState(MOVED_CACHE_PATH, DESTINATION_BOOK_PATH));
  EXPECT_FALSE(Storage.exists(MOVED_CACHE_PATH));
  EXPECT_TRUE(Storage.exists(MOVE_STAGING_PATH));
  EXPECT_TRUE(Storage.exists(SOURCE_BOOK_PATH));
}

TEST_F(BookCacheUtilsTest, RecoveryMergesIntoDerivedOnlyCacheCreatedAfterBookRename) {
  put("stats_v5.bin", 8);
  Storage.setFile(BookmarkUtil::getBookmarkPath(SOURCE_BOOK_PATH), bytes(10));
  ASSERT_TRUE(prepareBookCacheUserStateMove(CACHE_PATH, MOVED_CACHE_PATH, SOURCE_BOOK_PATH, DESTINATION_BOOK_PATH));
  ASSERT_TRUE(Storage.rename(SOURCE_BOOK_PATH, DESTINATION_BOOK_PATH));
  Storage.addDirectory(MOVED_CACHE_PATH);
  Storage.setFile(std::string(MOVED_CACHE_PATH) + "/book.bin", bytes(11));
  Storage.addDirectory(std::string(MOVED_CACHE_PATH) + "/sections");
  Storage.setFile(std::string(MOVED_CACHE_PATH) + "/sections/0.bin", bytes(12));

  ASSERT_TRUE(recoverBookCacheUserState(MOVED_CACHE_PATH, DESTINATION_BOOK_PATH));
  EXPECT_EQ(Storage.file(std::string(MOVED_CACHE_PATH) + "/stats_v5.bin"), bytes(8));
  EXPECT_EQ(Storage.file(std::string(MOVED_CACHE_PATH) + "/book.bin"), bytes(11));
  EXPECT_EQ(Storage.file(std::string(MOVED_CACHE_PATH) + "/sections/0.bin"), bytes(12));
  EXPECT_EQ(Storage.file(BookmarkUtil::getBookmarkPath(DESTINATION_BOOK_PATH)), bytes(10));
  EXPECT_FALSE(Storage.exists(MOVE_STAGING_PATH));
}

TEST_F(BookCacheUtilsTest, CleanupFailureAfterAtomicMergeNeverBlocksTheRecoveredBook) {
  put("stats_v5.bin", 8);
  ASSERT_TRUE(prepareBookCacheUserStateMove(CACHE_PATH, MOVED_CACHE_PATH, SOURCE_BOOK_PATH, DESTINATION_BOOK_PATH));
  ASSERT_TRUE(Storage.rename(SOURCE_BOOK_PATH, DESTINATION_BOOK_PATH));
  Storage.addDirectory(MOVED_CACHE_PATH);
  Storage.setFile(std::string(MOVED_CACHE_PATH) + "/book.bin", bytes(11));
  Storage.failDeletePathTimes(MOVE_CLEANUP_PATH, 2);

  ASSERT_TRUE(recoverBookCacheUserState(MOVED_CACHE_PATH, DESTINATION_BOOK_PATH));
  EXPECT_EQ(Storage.file(std::string(MOVED_CACHE_PATH) + "/stats_v5.bin"), bytes(8));
  EXPECT_TRUE(Storage.exists(MOVE_CLEANUP_PATH));
  EXPECT_FALSE(Storage.exists(MOVE_STAGING_PATH));

  // Model a reset after recursive cleanup removed the marker first.
  ASSERT_TRUE(Storage.remove((std::string(MOVE_CLEANUP_PATH) + "/.crossvi_move_ready").c_str()));
  ASSERT_TRUE(recoverBookCacheUserState(MOVED_CACHE_PATH, DESTINATION_BOOK_PATH));
  EXPECT_FALSE(Storage.exists(MOVE_CLEANUP_PATH));
  EXPECT_EQ(Storage.file(std::string(MOVED_CACHE_PATH) + "/stats_v5.bin"), bytes(8));
}

TEST_F(BookCacheUtilsTest, InterruptedMarkerlessPrepareIsSafelyRebuilt) {
  put("stats_v5.bin", 3);
  put("crossvi_reader_settings.bin", 4);
  Storage.addDirectory(MOVE_STAGING_PATH);
  Storage.setFile(std::string(MOVE_STAGING_PATH) + "/stats_v5.bin", bytes(3));

  ASSERT_TRUE(prepareBookCacheUserStateMove(CACHE_PATH, MOVED_CACHE_PATH, SOURCE_BOOK_PATH, DESTINATION_BOOK_PATH));
  EXPECT_EQ(Storage.file(std::string(MOVE_STAGING_PATH) + "/stats_v5.bin"), bytes(3));
  EXPECT_EQ(Storage.file(std::string(MOVE_STAGING_PATH) + "/crossvi_reader_settings.bin"), bytes(4));
  EXPECT_TRUE(Storage.exists((std::string(MOVE_STAGING_PATH) + "/.crossvi_move_ready").c_str()));
}

TEST_F(BookCacheUtilsTest, PreparedCopyIsRebuiltWhenSourceStateChangesBeforeBookRename) {
  put("stats_v5.bin", 3);
  ASSERT_TRUE(prepareBookCacheUserStateMove(CACHE_PATH, MOVED_CACHE_PATH, SOURCE_BOOK_PATH, DESTINATION_BOOK_PATH));
  put("stats_v5.bin", 7);

  ASSERT_TRUE(prepareBookCacheUserStateMove(CACHE_PATH, MOVED_CACHE_PATH, SOURCE_BOOK_PATH, DESTINATION_BOOK_PATH));
  EXPECT_EQ(Storage.file(std::string(MOVE_STAGING_PATH) + "/stats_v5.bin"), bytes(7));
  EXPECT_TRUE(Storage.exists(SOURCE_BOOK_PATH));
  EXPECT_FALSE(Storage.exists(DESTINATION_BOOK_PATH));
}

TEST_F(BookCacheUtilsTest, UnexpectedInterruptedMoveContentIsPreserved) {
  put("stats_v5.bin", 3);
  Storage.addDirectory(MOVE_STAGING_PATH);
  Storage.setFile(std::string(MOVE_STAGING_PATH) + "/unknown.bin", bytes(9));

  EXPECT_FALSE(prepareBookCacheUserStateMove(CACHE_PATH, MOVED_CACHE_PATH, SOURCE_BOOK_PATH, DESTINATION_BOOK_PATH));
  EXPECT_EQ(Storage.file(std::string(MOVE_STAGING_PATH) + "/unknown.bin"), bytes(9));
}

TEST_F(BookCacheUtilsTest, BookmarkKeysDoNotFlattenDistinctBookPathsTogether) {
  EXPECT_NE(BookmarkUtil::getBookmarkPath("/Books/Foo_Bar.epub"), BookmarkUtil::getBookmarkPath("/Books/Foo/Bar.epub"));
  EXPECT_EQ(BookmarkUtil::getLegacyBookmarkPath("/Books/Foo_Bar.epub"),
            BookmarkUtil::getLegacyBookmarkPath("/Books/Foo/Bar.epub"));
}

TEST_F(BookCacheUtilsTest, ReplacementShadowsButNeverDeletesAnAmbiguousLegacyBookmark) {
  const std::string legacy = BookmarkUtil::getLegacyBookmarkPath(SOURCE_BOOK_PATH);
  const std::string canonical = BookmarkUtil::getBookmarkPath(SOURCE_BOOK_PATH);
  Storage.setFile(legacy, bytes(14));

  ASSERT_TRUE(BookmarkUtil::ensureLegacyBookmarkShadowed(SOURCE_BOOK_PATH));
  EXPECT_EQ(Storage.file(canonical), emptyBookmarks());
  EXPECT_EQ(Storage.file(legacy), bytes(14));
}

TEST_F(BookCacheUtilsTest, LegacyShadowOverwritesStaleCanonicalBookmarksWithVerifiedEmptyState) {
  const std::string legacy = BookmarkUtil::getLegacyBookmarkPath(SOURCE_BOOK_PATH);
  const std::string canonical = BookmarkUtil::getBookmarkPath(SOURCE_BOOK_PATH);
  Storage.setFile(legacy, bytes(14));
  Storage.setFile(canonical, bytes(22));

  ASSERT_TRUE(BookmarkUtil::ensureLegacyBookmarkShadowed(SOURCE_BOOK_PATH));
  EXPECT_EQ(Storage.file(canonical), emptyBookmarks());
  EXPECT_EQ(Storage.file(legacy), bytes(14));
}

TEST_F(BookCacheUtilsTest, EmptyDestinationShadowCanBeReplacedByMovedSourceBookmarks) {
  const std::string source = BookmarkUtil::getBookmarkPath(SOURCE_BOOK_PATH);
  const std::string destination = BookmarkUtil::getBookmarkPath(DESTINATION_BOOK_PATH);
  Storage.setFile(source, bytes(21));
  ASSERT_TRUE(BookmarkUtil::writeEmptyCanonicalBookmark(DESTINATION_BOOK_PATH));

  ASSERT_TRUE(prepareBookCacheUserStateMove(CACHE_PATH, MOVED_CACHE_PATH, SOURCE_BOOK_PATH, DESTINATION_BOOK_PATH));
  ASSERT_TRUE(Storage.rename(SOURCE_BOOK_PATH, DESTINATION_BOOK_PATH));
  ASSERT_TRUE(finalizeBookCacheUserStateMove(CACHE_PATH, MOVED_CACHE_PATH, SOURCE_BOOK_PATH, DESTINATION_BOOK_PATH));
  ASSERT_TRUE(completeBookCacheUserStateMove(CACHE_PATH, MOVED_CACHE_PATH, SOURCE_BOOK_PATH, DESTINATION_BOOK_PATH));

  EXPECT_EQ(Storage.file(destination), bytes(21));
}

TEST_F(BookCacheUtilsTest, DestinationLegacyIsShadowedWhenMovedSourceHasNoBookmarks) {
  const std::string destinationLegacy = BookmarkUtil::getLegacyBookmarkPath(DESTINATION_BOOK_PATH);
  const std::string destination = BookmarkUtil::getBookmarkPath(DESTINATION_BOOK_PATH);
  Storage.setFile(destinationLegacy, bytes(31));

  ASSERT_TRUE(prepareBookCacheUserStateMove(CACHE_PATH, MOVED_CACHE_PATH, SOURCE_BOOK_PATH, DESTINATION_BOOK_PATH));
  EXPECT_EQ(Storage.file(std::string(MOVE_STAGING_PATH) + "/.crossvi_bookmark.json"), emptyBookmarks());
  ASSERT_TRUE(Storage.rename(SOURCE_BOOK_PATH, DESTINATION_BOOK_PATH));
  ASSERT_TRUE(finalizeBookCacheUserStateMove(CACHE_PATH, MOVED_CACHE_PATH, SOURCE_BOOK_PATH, DESTINATION_BOOK_PATH));
  ASSERT_TRUE(completeBookCacheUserStateMove(CACHE_PATH, MOVED_CACHE_PATH, SOURCE_BOOK_PATH, DESTINATION_BOOK_PATH));

  EXPECT_EQ(Storage.file(destination), emptyBookmarks());
  EXPECT_EQ(Storage.file(destinationLegacy), bytes(31));
}

TEST_F(BookCacheUtilsTest, LegacyBookmarkIsCopiedButNeverDeletedAsOwnedState) {
  const std::string legacySource = BookmarkUtil::getLegacyBookmarkPath(SOURCE_BOOK_PATH);
  const std::string destination = BookmarkUtil::getBookmarkPath(DESTINATION_BOOK_PATH);
  Storage.setFile(legacySource, bytes(14));
  ASSERT_TRUE(prepareBookCacheUserStateMove(CACHE_PATH, MOVED_CACHE_PATH, SOURCE_BOOK_PATH, DESTINATION_BOOK_PATH));
  ASSERT_TRUE(Storage.rename(SOURCE_BOOK_PATH, DESTINATION_BOOK_PATH));
  ASSERT_TRUE(finalizeBookCacheUserStateMove(CACHE_PATH, MOVED_CACHE_PATH, SOURCE_BOOK_PATH, DESTINATION_BOOK_PATH));
  ASSERT_TRUE(completeBookCacheUserStateMove(CACHE_PATH, MOVED_CACHE_PATH, SOURCE_BOOK_PATH, DESTINATION_BOOK_PATH));

  EXPECT_EQ(Storage.file(destination), bytes(14));
  EXPECT_EQ(Storage.file(legacySource), bytes(14));
  EXPECT_EQ(Storage.file(BookmarkUtil::getBookmarkPath(SOURCE_BOOK_PATH)), emptyBookmarks());
}

}  // namespace
