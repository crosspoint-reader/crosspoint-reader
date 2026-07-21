#include <HalStorage.h>
#include <Xtc.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "BookPathMoveUtils.h"
#include "BookmarkUtil.h"

namespace {

std::vector<unsigned char> xtcMagic(const bool highQuality) {
  return {'X', 'T', 'C', static_cast<unsigned char>(highQuality ? 'H' : 0)};
}

uint32_t pendingCrc(const std::vector<unsigned char>& payload) {
  uint32_t crc = UINT32_MAX;
  for (const uint8_t byte : payload) {
    crc ^= byte;
    for (uint8_t bit = 0; bit < 8; ++bit) crc = (crc >> 1U) ^ (0xEDB88320U & (0U - (crc & 1U)));
  }
  return ~crc;
}

std::vector<unsigned char> replacementPending(const std::string& bookPath, const bool hadBook) {
  const uint8_t flags = hadBook ? 1U : 0U;
  std::vector<unsigned char> payload = {flags};
  payload.insert(payload.end(), bookPath.begin(), bookPath.end());
  const uint32_t crc = pendingCrc(payload);
  std::vector<unsigned char> marker = {'C',
                                       'V',
                                       'R',
                                       'P',
                                       2,
                                       flags,
                                       static_cast<uint8_t>(bookPath.size()),
                                       static_cast<uint8_t>(bookPath.size() >> 8U),
                                       static_cast<uint8_t>(crc),
                                       static_cast<uint8_t>(crc >> 8U),
                                       static_cast<uint8_t>(crc >> 16U),
                                       static_cast<uint8_t>(crc >> 24U)};
  marker.insert(marker.end(), bookPath.begin(), bookPath.end());
  return marker;
}

class BookFilePublishTest : public testing::TestWithParam<const char*> {
 protected:
  void SetUp() override {
    Storage.reset();
    Storage.addDirectory("/books");
    Storage.addDirectory("/.crosspoint");
    Storage.addDirectory(BookmarkUtil::getBookmarksDir());
  }
};

TEST_P(BookFilePublishTest, InvalidStagedXtcCannotReplaceBookOrMutateUserState) {
  const std::string bookPath = std::string("/books/story") + GetParam();
  const std::string stagingPath = hiddenBookFileSibling(bookPath, ".crossvi-upload.tmp");
  const std::string cachePath = Xtc(bookPath, "/.crosspoint").getCachePath();
  const std::string bookmarkPath = BookmarkUtil::getBookmarkPath(bookPath);
  const auto existingBook = xtcMagic(std::string(GetParam()) == ".xtch");
  const std::vector<unsigned char> invalidUpload = {'N', 'O', 'P', 'E'};
  const std::vector<unsigned char> progress = {1, 2, 3};
  const std::vector<unsigned char> settings = {4, 5, 6};
  const std::vector<unsigned char> unknownState = {7, 8, 9};
  const std::vector<unsigned char> bookmark = {'{', '}'};

  Storage.setFile(bookPath, existingBook);
  Storage.setFile(stagingPath, invalidUpload);
  Storage.addDirectory(cachePath);
  Storage.setFile(cachePath + "/progress.bin", progress);
  Storage.setFile(cachePath + "/crossvi_reader_settings.bin", settings);
  Storage.setFile(cachePath + "/future_state.bin", unknownState);
  Storage.setFile(bookmarkPath, bookmark);

  EXPECT_EQ(publishStagedBookFile(stagingPath, bookPath), BookFilePublishResult::InvalidStagedFile);

  EXPECT_EQ(Storage.file(bookPath), existingBook);
  EXPECT_EQ(Storage.file(stagingPath), invalidUpload);
  EXPECT_EQ(Storage.file(cachePath + "/progress.bin"), progress);
  EXPECT_EQ(Storage.file(cachePath + "/crossvi_reader_settings.bin"), settings);
  EXPECT_EQ(Storage.file(cachePath + "/future_state.bin"), unknownState);
  EXPECT_EQ(Storage.file(bookmarkPath), bookmark);
  EXPECT_TRUE(Storage.renameHistory().empty());
  EXPECT_FALSE(Storage.exists(hiddenBookFileSibling(bookPath, ".crossvi-replace.bak")));
  EXPECT_FALSE(Storage.exists(hiddenBookFileSibling(bookPath, ".crossvi-replace.pending")));
}

TEST_P(BookFilePublishTest, InterruptedReplacementRestoresOldBookAndClearsPendingMarker) {
  const std::string bookPath = std::string("/books/story") + GetParam();
  const std::string oldBookPath = hiddenBookFileSibling(bookPath, ".crossvi-replace.bak");
  const std::string pendingPath = hiddenBookFileSibling(bookPath, ".crossvi-replace.pending");
  const auto existingBook = xtcMagic(std::string(GetParam()) == ".xtch");
  const std::vector<unsigned char> pending = replacementPending(bookPath, true);

  Storage.setFile(oldBookPath, existingBook);
  Storage.setFile(pendingPath, pending);

  EXPECT_TRUE(recoverInterruptedBookFileReplacement(bookPath));
  EXPECT_EQ(Storage.file(bookPath), existingBook);
  EXPECT_FALSE(Storage.exists(oldBookPath));
  EXPECT_FALSE(Storage.exists(pendingPath));
  ASSERT_EQ(Storage.renameHistory().size(), 1U);
  EXPECT_EQ(Storage.renameHistory().front(), std::make_pair(oldBookPath, bookPath));
}

TEST_P(BookFilePublishTest, MarkerlessBackupCollisionIsPreservedAndFailsClosed) {
  const std::string bookPath = std::string("/books/story") + GetParam();
  const std::string oldBookPath = hiddenBookFileSibling(bookPath, ".crossvi-replace.bak");
  const auto currentBook = xtcMagic(std::string(GetParam()) == ".xtch");
  const std::vector<unsigned char> collision = {'U', 'S', 'E', 'R'};
  Storage.setFile(bookPath, currentBook);
  Storage.setFile(oldBookPath, collision);

  EXPECT_FALSE(recoverInterruptedBookFileReplacement(bookPath));
  EXPECT_EQ(Storage.file(bookPath), currentBook);
  EXPECT_EQ(Storage.file(oldBookPath), collision);
}

TEST_P(BookFilePublishTest, MismatchedPendingMarkerCannotAuthorizeBackupDeletion) {
  const std::string bookPath = std::string("/books/story") + GetParam();
  const std::string oldBookPath = hiddenBookFileSibling(bookPath, ".crossvi-replace.bak");
  const std::string pendingPath = hiddenBookFileSibling(bookPath, ".crossvi-replace.pending");
  const auto currentBook = xtcMagic(std::string(GetParam()) == ".xtch");
  const std::vector<unsigned char> collision = {'U', 'S', 'E', 'R'};
  Storage.setFile(bookPath, currentBook);
  Storage.setFile(oldBookPath, collision);
  Storage.setFile(pendingPath, replacementPending("/books/different.xtc", true));

  EXPECT_FALSE(recoverInterruptedBookFileReplacement(bookPath));
  EXPECT_EQ(Storage.file(bookPath), currentBook);
  EXPECT_EQ(Storage.file(oldBookPath), collision);
  EXPECT_TRUE(Storage.exists(pendingPath));
}

TEST_P(BookFilePublishTest, CommittedReplacementDeletesBackupBeforePendingMarker) {
  const std::string bookPath = std::string("/books/story") + GetParam();
  const std::string oldBookPath = hiddenBookFileSibling(bookPath, ".crossvi-replace.bak");
  const std::string pendingPath = hiddenBookFileSibling(bookPath, ".crossvi-replace.pending");
  const auto currentBook = xtcMagic(std::string(GetParam()) == ".xtch");
  Storage.setFile(bookPath, currentBook);
  Storage.setFile(oldBookPath, std::vector<unsigned char>{'O', 'L', 'D'});
  Storage.setFile(pendingPath, replacementPending(bookPath, true));
  Storage.failDeletePathOnce(pendingPath);

  EXPECT_FALSE(recoverInterruptedBookFileReplacement(bookPath));
  EXPECT_EQ(Storage.file(bookPath), currentBook);
  EXPECT_FALSE(Storage.exists(oldBookPath));
  EXPECT_TRUE(Storage.exists(pendingPath));

  EXPECT_TRUE(recoverInterruptedBookFileReplacement(bookPath));
  EXPECT_FALSE(Storage.exists(pendingPath));
}

TEST(BookFilePublishEpubRecoveryTest, MarkerlessBackupCollisionIsPreservedAndFailsClosed) {
  Storage.reset();
  Storage.addDirectory("/books");
  Storage.addDirectory("/.crosspoint");
  const std::string bookPath = "/books/story.epub";
  const std::string oldBookPath = hiddenBookFileSibling(bookPath, ".crossvi-replace.bak");
  const std::vector<unsigned char> currentBook = {'E', 'P', 'U', 'B'};
  const std::vector<unsigned char> collision = {'U', 'S', 'E', 'R'};
  Storage.setFile(bookPath, currentBook);
  Storage.setFile(oldBookPath, collision);

  EXPECT_FALSE(recoverInterruptedBookFileReplacement(bookPath));
  EXPECT_EQ(Storage.file(bookPath), currentBook);
  EXPECT_EQ(Storage.file(oldBookPath), collision);
}

TEST(BookFilePublishEpubRecoveryTest, OwnedBackupIsDeletedBeforePendingMarker) {
  Storage.reset();
  Storage.addDirectory("/books");
  Storage.addDirectory("/.crosspoint");
  Storage.addDirectory(BookmarkUtil::getBookmarksDir());
  const std::string bookPath = "/books/story.epub";
  const std::string oldBookPath = hiddenBookFileSibling(bookPath, ".crossvi-replace.bak");
  const std::string pendingPath = hiddenBookFileSibling(bookPath, ".crossvi-replace.pending");
  Storage.setFile(bookPath, std::vector<unsigned char>{'N', 'E', 'W'});
  Storage.setFile(oldBookPath, std::vector<unsigned char>{'O', 'L', 'D'});
  Storage.setFile(pendingPath, replacementPending(bookPath, true));
  Storage.failDeletePathOnce(pendingPath);

  EXPECT_FALSE(recoverInterruptedBookFileReplacement(bookPath));
  EXPECT_FALSE(Storage.exists(oldBookPath));
  EXPECT_TRUE(Storage.exists(pendingPath));
  EXPECT_TRUE(recoverInterruptedBookFileReplacement(bookPath));
  EXPECT_FALSE(Storage.exists(pendingPath));
}

TEST(BookFilePublishCopySafetyTest, ExistingDestinationSurvivesBackupRenameFailure) {
  Storage.reset();
  Storage.addDirectory("/books");
  const std::string destination = "/books/copied.txt";
  const std::string staging = hiddenBookFileSibling(destination, ".davtmp");
  const std::vector<unsigned char> oldBytes = {'O', 'L', 'D'};
  const std::vector<unsigned char> newBytes = {'N', 'E', 'W'};
  Storage.setFile(destination, oldBytes);
  Storage.setFile(staging, newBytes);
  Storage.failRenameOnCall(1);

  EXPECT_EQ(publishStagedBookFile(staging, destination), BookFilePublishResult::StorageError);
  EXPECT_EQ(Storage.file(destination), oldBytes);
  EXPECT_EQ(Storage.file(staging), newBytes);
  EXPECT_FALSE(Storage.exists(hiddenBookFileSibling(destination, ".crossvi-replace.bak")));
}

TEST(BookFilePublishCopySafetyTest, ExistingDestinationIsRestoredWhenStagingRenameFails) {
  Storage.reset();
  Storage.addDirectory("/books");
  const std::string destination = "/books/copied.txt";
  const std::string staging = hiddenBookFileSibling(destination, ".davtmp");
  const std::vector<unsigned char> oldBytes = {'O', 'L', 'D'};
  const std::vector<unsigned char> newBytes = {'N', 'E', 'W'};
  Storage.setFile(destination, oldBytes);
  Storage.setFile(staging, newBytes);
  Storage.failRenameOnCall(2);

  EXPECT_EQ(publishStagedBookFile(staging, destination), BookFilePublishResult::StorageError);
  EXPECT_EQ(Storage.file(destination), oldBytes);
  EXPECT_EQ(Storage.file(staging), newBytes);
  EXPECT_FALSE(Storage.exists(hiddenBookFileSibling(destination, ".crossvi-replace.bak")));
}

TEST(BookFilePublishCopySafetyTest, MarkerWriteOrSyncFailureLeavesExistingDestinationUntouched) {
  for (const bool failSync : {false, true}) {
    Storage.reset();
    Storage.addDirectory("/books");
    const std::string destination = "/books/copied.txt";
    const std::string staging = hiddenBookFileSibling(destination, ".davtmp");
    const std::vector<unsigned char> oldBytes = {'O', 'L', 'D'};
    const std::vector<unsigned char> newBytes = {'N', 'E', 'W'};
    Storage.setFile(destination, oldBytes);
    Storage.setFile(staging, newBytes);
    failSync ? Storage.failSyncOnce() : Storage.shortWriteOnce();

    EXPECT_EQ(publishStagedBookFile(staging, destination), BookFilePublishResult::StateUnavailable);
    EXPECT_EQ(Storage.file(destination), oldBytes);
    EXPECT_EQ(Storage.file(staging), newBytes);
    EXPECT_FALSE(Storage.exists(hiddenBookFileSibling(destination, ".crossvi-replace.bak")));
  }
}

INSTANTIATE_TEST_SUITE_P(XtcAndXtch, BookFilePublishTest, testing::Values(".xtc", ".xtch"));

}  // namespace
