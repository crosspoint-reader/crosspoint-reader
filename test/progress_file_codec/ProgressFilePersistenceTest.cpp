#include <HalStorage.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

#include "ProgressFile.h"
#include "ProgressFileCodec.h"

namespace {
constexpr char CACHE_PATH[] = "/book";
constexpr char PRIMARY[] = "/book/progress.bin";
constexpr char BACKUP[] = "/book/progress.bin.bak";
constexpr char TEMP[] = "/book/progress.bin.tmp";

template <size_t Size>
std::vector<uint8_t> bytes(const std::array<uint8_t, Size>& value) {
  return {value.begin(), value.end()};
}

std::array<uint8_t, 4> pageBytes(const uint32_t page) {
  return {static_cast<uint8_t>(page), static_cast<uint8_t>(page >> 8), static_cast<uint8_t>(page >> 16),
          static_cast<uint8_t>(page >> 24)};
}
}  // namespace

TEST(ProgressFilePersistence, LoadsPrimaryThenFallsBackToBackupAndTemp) {
  Storage.reset();
  const std::array<uint8_t, 6> primary{1, 0, 2, 0, 3, 0};
  const std::array<uint8_t, 6> backup{4, 0, 5, 0, 6, 0};
  const std::array<uint8_t, 6> temp{7, 0, 8, 0, 9, 0};
  Storage.setFile(PRIMARY, bytes(primary));
  Storage.setFile(BACKUP, bytes(backup));
  Storage.setFile(TEMP, bytes(temp));

  uint8_t loaded[6]{};
  auto result = ProgressFile::loadEpub(CACHE_PATH, loaded, sizeof(loaded));
  EXPECT_EQ(result.source, ProgressFile::LoadSource::Primary);
  EXPECT_EQ(result.size, primary.size());
  EXPECT_EQ(std::vector<uint8_t>(loaded, loaded + result.size), bytes(primary));

  ASSERT_TRUE(Storage.remove(PRIMARY));
  result = ProgressFile::loadEpub(CACHE_PATH, loaded, sizeof(loaded));
  EXPECT_EQ(result.source, ProgressFile::LoadSource::Backup);
  EXPECT_EQ(std::vector<uint8_t>(loaded, loaded + result.size), bytes(backup));

  ASSERT_TRUE(Storage.remove(BACKUP));
  result = ProgressFile::loadEpub(CACHE_PATH, loaded, sizeof(loaded));
  EXPECT_EQ(result.source, ProgressFile::LoadSource::Temp);
  EXPECT_EQ(std::vector<uint8_t>(loaded, loaded + result.size), bytes(temp));
}

TEST(ProgressFilePersistence, AcceptsLegacyEpubButKeepsPageLayoutsStrict) {
  Storage.reset();
  const std::array<uint8_t, 4> legacy{1, 0, 2, 0};
  Storage.setFile(PRIMARY, bytes(legacy));
  uint8_t epub[6]{};
  const auto epubResult = ProgressFile::loadEpub(CACHE_PATH, epub, sizeof(epub));
  EXPECT_EQ(epubResult.source, ProgressFile::LoadSource::Primary);
  EXPECT_EQ(epubResult.size, legacy.size());

  Storage.setFile(PRIMARY, {1, 2, 3, 4, 5, 6});
  uint8_t page[4]{};
  EXPECT_EQ(ProgressFile::loadPage(CACHE_PATH, page, sizeof(page)).source, ProgressFile::LoadSource::Invalid);
}

TEST(ProgressFilePersistence, SkipsCorruptPrimaryForValidBackup) {
  Storage.reset();
  const std::array<uint8_t, 4> backup{9, 8, 7, 6};
  Storage.setFile(PRIMARY, {1, 2, 3});
  Storage.setFile(BACKUP, bytes(backup));

  uint8_t loaded[4]{};
  const auto result = ProgressFile::loadPage(CACHE_PATH, loaded, sizeof(loaded));
  EXPECT_EQ(result.source, ProgressFile::LoadSource::Backup);
  EXPECT_EQ(std::vector<uint8_t>(loaded, loaded + result.size), bytes(backup));
}

TEST(ProgressFilePersistence, SkipsSameSizeOutOfRangePageForValidBackup) {
  Storage.reset();
  const auto invalidPrimary = pageBytes(20);
  const auto validBackup = pageBytes(4);
  Storage.setFile(PRIMARY, bytes(invalidPrimary));
  Storage.setFile(BACKUP, bytes(validBackup));
  const ProgressFile::PageBounds bounds{10};
  const ProgressFile::CandidateValidator validator{ProgressFile::validatePageBounds, &bounds};

  uint8_t loaded[4]{};
  const auto result = ProgressFile::loadPage(CACHE_PATH, loaded, sizeof(loaded), validator);
  EXPECT_EQ(result.source, ProgressFile::LoadSource::Backup);
  EXPECT_EQ(std::vector<uint8_t>(loaded, loaded + result.size), bytes(validBackup));
}

TEST(ProgressFilePersistence, SizeOnlyFallbackRetainsOutOfRangePageAfterRepagination) {
  Storage.reset();
  const auto oldPaginationPage = pageBytes(20);
  Storage.setFile(PRIMARY, bytes(oldPaginationPage));
  const ProgressFile::PageBounds newPagination{10};
  const ProgressFile::CandidateValidator validator{ProgressFile::validatePageBounds, &newPagination};

  uint8_t loaded[4]{};
  EXPECT_EQ(ProgressFile::loadPage(CACHE_PATH, loaded, sizeof(loaded), validator).source,
            ProgressFile::LoadSource::Invalid);
  const auto sizeValid = ProgressFile::loadPage(CACHE_PATH, loaded, sizeof(loaded));
  ASSERT_TRUE(sizeValid);
  EXPECT_EQ(ProgressFileCodec::decodePage(loaded), 20u);
  EXPECT_EQ(std::min(ProgressFileCodec::decodePage(loaded), newPagination.pageCount - 1), 9u);
}

TEST(ProgressFilePersistence, SkipsSameSizeInvalidEpubForValidBackup) {
  Storage.reset();
  const std::array<uint8_t, 6> invalidPrimary{1, 0, 7, 0, 7, 0};
  const std::array<uint8_t, 6> validBackup{1, 0, 6, 0, 7, 0};
  Storage.setFile(PRIMARY, bytes(invalidPrimary));
  Storage.setFile(BACKUP, bytes(validBackup));
  const ProgressFile::EpubBounds bounds{3};
  const ProgressFile::CandidateValidator validator{ProgressFile::validateEpubBounds, &bounds};

  uint8_t loaded[6]{};
  const auto result = ProgressFile::loadEpub(CACHE_PATH, loaded, sizeof(loaded), validator);
  EXPECT_EQ(result.source, ProgressFile::LoadSource::Backup);
  EXPECT_EQ(std::vector<uint8_t>(loaded, loaded + result.size), bytes(validBackup));
}

TEST(ProgressFilePersistence, PublishesVerifiedPrimaryAndRotatesBackup) {
  Storage.reset();
  const std::array<uint8_t, 4> older{1, 0, 0, 0};
  const std::array<uint8_t, 4> previous{2, 0, 0, 0};
  const std::array<uint8_t, 4> next{3, 0, 0, 0};
  Storage.setFile(PRIMARY, bytes(previous));
  Storage.setFile(BACKUP, bytes(older));

  ASSERT_TRUE(ProgressFile::writeAtomic(CACHE_PATH, next.data(), next.size()));
  EXPECT_EQ(Storage.file(PRIMARY), bytes(next));
  EXPECT_EQ(Storage.file(BACKUP), bytes(previous));
  EXPECT_FALSE(Storage.exists(TEMP));
}

TEST(ProgressFilePersistence, DoesNotRotateSemanticInvalidPrimaryOverGoodBackup) {
  Storage.reset();
  const auto invalidPrimary = pageBytes(20);
  const auto validBackup = pageBytes(4);
  const auto next = pageBytes(5);
  Storage.setFile(PRIMARY, bytes(invalidPrimary));
  Storage.setFile(BACKUP, bytes(validBackup));
  const ProgressFile::PageBounds bounds{10};
  const ProgressFile::CandidateValidator validator{ProgressFile::validatePageBounds, &bounds};

  ASSERT_TRUE(ProgressFile::writeAtomic(CACHE_PATH, next.data(), next.size(), validator));
  EXPECT_EQ(Storage.file(PRIMARY), bytes(next));
  EXPECT_EQ(Storage.file(BACKUP), bytes(validBackup));
  EXPECT_FALSE(Storage.exists(TEMP));
}

TEST(ProgressFilePersistence, RejectsOutOfRangeNewProgressWithoutChangingFiles) {
  Storage.reset();
  const auto primary = pageBytes(3);
  const auto invalidNext = pageBytes(10);
  Storage.setFile(PRIMARY, bytes(primary));
  const ProgressFile::PageBounds bounds{10};
  const ProgressFile::CandidateValidator validator{ProgressFile::validatePageBounds, &bounds};

  EXPECT_FALSE(ProgressFile::writeAtomic(CACHE_PATH, invalidNext.data(), invalidNext.size(), validator));
  EXPECT_EQ(Storage.file(PRIMARY), bytes(primary));
  EXPECT_FALSE(Storage.exists(BACKUP));
  EXPECT_FALSE(Storage.exists(TEMP));
}

TEST(ProgressFilePersistence, ShortWriteLeavesCommittedCopiesUntouched) {
  Storage.reset();
  const std::array<uint8_t, 4> primary{2, 0, 0, 0};
  const std::array<uint8_t, 4> backup{1, 0, 0, 0};
  const std::array<uint8_t, 4> next{3, 0, 0, 0};
  Storage.setFile(PRIMARY, bytes(primary));
  Storage.setFile(BACKUP, bytes(backup));
  Storage.shortWriteOnce();

  EXPECT_FALSE(ProgressFile::writeAtomic(CACHE_PATH, next.data(), next.size()));
  EXPECT_EQ(Storage.file(PRIMARY), bytes(primary));
  EXPECT_EQ(Storage.file(BACKUP), bytes(backup));
  EXPECT_FALSE(Storage.exists(TEMP));
}

TEST(ProgressFilePersistence, FailedPublishKeepsBackupAndVerifiedTempRecoverable) {
  Storage.reset();
  const std::array<uint8_t, 4> backup{1, 0, 0, 0};
  const std::array<uint8_t, 4> next{2, 0, 0, 0};
  Storage.setFile(BACKUP, bytes(backup));
  Storage.failRenameOnce();

  EXPECT_FALSE(ProgressFile::writeAtomic(CACHE_PATH, next.data(), next.size()));
  EXPECT_EQ(Storage.file(BACKUP), bytes(backup));
  EXPECT_EQ(Storage.file(TEMP), bytes(next));

  uint8_t loaded[4]{};
  EXPECT_EQ(ProgressFile::loadPage(CACHE_PATH, loaded, sizeof(loaded)).source, ProgressFile::LoadSource::Backup);
  ASSERT_TRUE(Storage.remove(BACKUP));
  EXPECT_EQ(ProgressFile::loadPage(CACHE_PATH, loaded, sizeof(loaded)).source, ProgressFile::LoadSource::Temp);
  EXPECT_EQ(std::vector<uint8_t>(loaded, loaded + sizeof(loaded)), bytes(next));
}

TEST(ProgressFilePersistence, FailedPostRenameVerificationRecreatesRecoverableTemp) {
  Storage.reset();
  const std::array<uint8_t, 4> next{2, 0, 0, 0};
  Storage.corruptRenameOnce();

  EXPECT_FALSE(ProgressFile::writeAtomic(CACHE_PATH, next.data(), next.size()));
  EXPECT_FALSE(Storage.exists(PRIMARY));
  EXPECT_EQ(Storage.file(TEMP), bytes(next));

  uint8_t loaded[4]{};
  EXPECT_EQ(ProgressFile::loadPage(CACHE_PATH, loaded, sizeof(loaded)).source, ProgressFile::LoadSource::Temp);
  EXPECT_EQ(std::vector<uint8_t>(loaded, loaded + sizeof(loaded)), bytes(next));
}

TEST(ProgressFilePersistence, RecoversOnlyValidTempBeforeReusingItsPath) {
  Storage.reset();
  const std::array<uint8_t, 4> recovered{1, 0, 0, 0};
  const std::array<uint8_t, 4> next{2, 0, 0, 0};
  Storage.setFile(PRIMARY, {0, 0, 0});
  Storage.setFile(TEMP, bytes(recovered));

  ASSERT_TRUE(ProgressFile::writeAtomic(CACHE_PATH, next.data(), next.size()));
  EXPECT_EQ(Storage.file(PRIMARY), bytes(next));
  EXPECT_EQ(Storage.file(BACKUP), bytes(recovered));
}

TEST(ProgressFilePersistence, PreservesUnknownLargerLayouts) {
  Storage.reset();
  const std::vector<uint8_t> unknown{1, 2, 3, 4, 5};
  const std::array<uint8_t, 4> next{2, 0, 0, 0};
  Storage.setFile(PRIMARY, unknown);

  EXPECT_FALSE(ProgressFile::writeAtomic(CACHE_PATH, next.data(), next.size()));
  EXPECT_EQ(Storage.file(PRIMARY), unknown);
  EXPECT_FALSE(Storage.exists(TEMP));
}
