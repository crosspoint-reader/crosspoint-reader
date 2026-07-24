#include <HalStorage.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <iterator>
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

std::array<uint8_t, ProgressFileCodec::TXT_V2_SIZE> txtOffsetBytes(const uint32_t offset) {
  uint8_t raw[ProgressFileCodec::TXT_V2_SIZE];
  ProgressFileCodec::encodeTxtOffset(offset, raw);
  std::array<uint8_t, ProgressFileCodec::TXT_V2_SIZE> result{};
  std::copy(std::begin(raw), std::end(raw), result.begin());
  return result;
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

TEST(ProgressFilePersistence, TxtLoadValidatesFormatAndFallsBackToLegacyBackup) {
  Storage.reset();
  auto invalidPrimary = txtOffsetBytes(200);
  invalidPrimary[0] ^= 0x80U;
  const auto legacyBackup = pageBytes(3);
  Storage.setFile(PRIMARY, bytes(invalidPrimary));
  Storage.setFile(BACKUP, bytes(legacyBackup));
  const ProgressFile::TxtBounds bounds{1000, 10};
  const ProgressFile::CandidateValidator validator{ProgressFile::validateTxtBounds, &bounds};

  uint8_t loaded[ProgressFileCodec::TXT_V2_SIZE]{};
  const auto result = ProgressFile::loadTxt(CACHE_PATH, loaded, sizeof(loaded), validator);
  ASSERT_TRUE(result);
  EXPECT_EQ(result.source, ProgressFile::LoadSource::Backup);
  EXPECT_EQ(result.size, 4u);
  uint32_t page = 0;
  EXPECT_EQ(ProgressFileCodec::decodeTxt(loaded, result.size, page), ProgressFileCodec::TxtDecodeStatus::LegacyPage);
  EXPECT_EQ(page, 3u);
}

TEST(ProgressFilePersistence, TxtLegacyPageCanBeMigratedToVerifiedByteOffset) {
  Storage.reset();
  const auto legacy = pageBytes(3);
  Storage.setFile(PRIMARY, bytes(legacy));
  const ProgressFile::TxtBounds bounds{5000, 10};
  const ProgressFile::CandidateValidator validator{ProgressFile::validateTxtBounds, &bounds};

  uint8_t loaded[ProgressFileCodec::TXT_V2_SIZE]{};
  const auto legacyResult = ProgressFile::loadTxt(CACHE_PATH, loaded, sizeof(loaded), validator);
  ASSERT_TRUE(legacyResult);
  uint32_t legacyPage = 0;
  ASSERT_EQ(ProgressFileCodec::decodeTxt(loaded, legacyResult.size, legacyPage),
            ProgressFileCodec::TxtDecodeStatus::LegacyPage);
  ASSERT_EQ(legacyPage, 3u);

  uint8_t migrated[ProgressFileCodec::TXT_V2_SIZE];
  ProgressFileCodec::encodeTxtOffset(1234, migrated);
  ASSERT_TRUE(ProgressFile::writeTxtAtomic(CACHE_PATH, migrated, validator));
  EXPECT_EQ(Storage.file(PRIMARY), std::vector<uint8_t>(std::begin(migrated), std::end(migrated)));
  EXPECT_EQ(Storage.file(BACKUP), bytes(legacy));

  const auto migratedResult = ProgressFile::loadTxt(CACHE_PATH, loaded, sizeof(loaded), validator);
  ASSERT_TRUE(migratedResult);
  uint32_t byteOffset = 0;
  EXPECT_EQ(ProgressFileCodec::decodeTxt(loaded, migratedResult.size, byteOffset),
            ProgressFileCodec::TxtDecodeStatus::Ok);
  EXPECT_EQ(byteOffset, 1234u);
}

TEST(ProgressFilePersistence, TxtWritePreservesSameSizeNewerVersion) {
  Storage.reset();
  auto newer = txtOffsetBytes(100);
  newer[1] = ProgressFileCodec::TXT_VERSION + 1;
  Storage.setFile(PRIMARY, bytes(newer));
  const ProgressFile::TxtBounds bounds{1000, 10};
  const ProgressFile::CandidateValidator validator{ProgressFile::validateTxtBounds, &bounds};
  uint8_t next[ProgressFileCodec::TXT_V2_SIZE];
  ProgressFileCodec::encodeTxtOffset(200, next);

  EXPECT_FALSE(ProgressFile::writeTxtAtomic(CACHE_PATH, next, validator));
  EXPECT_EQ(Storage.file(PRIMARY), bytes(newer));
  EXPECT_FALSE(Storage.exists(BACKUP));
  EXPECT_FALSE(Storage.exists(TEMP));
}

TEST(ProgressFilePersistence, TxtLoadFailsClosedWhenAnySiblingUsesNewerVersion) {
  const std::array<const char*, 3> candidatePaths = {PRIMARY, BACKUP, TEMP};
  for (const size_t futureSize :
       {size_t{2}, size_t{3}, size_t{5}, ProgressFileCodec::TXT_V2_SIZE, ProgressFileCodec::TXT_V2_SIZE + 4}) {
    for (size_t index = 0; index < candidatePaths.size(); ++index) {
      const char* newerPath = candidatePaths[index];
      SCOPED_TRACE(newerPath);
      SCOPED_TRACE(futureSize);
      Storage.reset();
      const auto legacy = pageBytes(3);
      Storage.setFile(index == 0 ? BACKUP : PRIMARY, bytes(legacy));
      auto newerRecord = txtOffsetBytes(100);
      newerRecord[1] = ProgressFileCodec::TXT_VERSION + 1;
      std::vector<uint8_t> newer = bytes(newerRecord);
      newer.resize(futureSize, 0xA5);
      Storage.setFile(newerPath, newer);

      uint8_t loaded[ProgressFileCodec::TXT_V2_SIZE]{};
      const auto result = ProgressFile::loadTxt(CACHE_PATH, loaded, sizeof(loaded));
      EXPECT_FALSE(result);
      EXPECT_EQ(result.source, ProgressFile::LoadSource::Invalid);

      uint8_t next[ProgressFileCodec::TXT_V2_SIZE];
      ProgressFileCodec::encodeTxtOffset(200, next);
      EXPECT_FALSE(ProgressFile::writeTxtAtomic(CACHE_PATH, next));
      EXPECT_EQ(Storage.file(newerPath), newer);
    }
  }
}

TEST(ProgressFilePersistence, TxtLegacyPageThatResemblesFutureHeaderRemainsUsable) {
  Storage.reset();
  // 852 == 0x00000354, whose first bytes are TXT magic 'T' and future v3.
  const auto legacy = pageBytes(852);
  Storage.setFile(PRIMARY, bytes(legacy));
  const ProgressFile::TxtBounds bounds{5000, 1000};
  const ProgressFile::CandidateValidator validator{ProgressFile::validateTxtBounds, &bounds};

  uint8_t loaded[ProgressFileCodec::TXT_V2_SIZE]{};
  const auto result = ProgressFile::loadTxt(CACHE_PATH, loaded, sizeof(loaded), validator);
  ASSERT_TRUE(result);
  EXPECT_EQ(result.source, ProgressFile::LoadSource::Primary);
  uint32_t page = 0;
  EXPECT_EQ(ProgressFileCodec::decodeTxt(loaded, result.size, page), ProgressFileCodec::TxtDecodeStatus::LegacyPage);
  EXPECT_EQ(page, 852u);

  uint8_t migrated[ProgressFileCodec::TXT_V2_SIZE];
  ProgressFileCodec::encodeTxtOffset(1234, migrated);
  EXPECT_TRUE(ProgressFile::writeTxtAtomic(CACHE_PATH, migrated, validator));
  EXPECT_EQ(Storage.file(BACKUP), bytes(legacy));
}

TEST(ProgressFilePersistence, TxtBoundsRejectOffsetAtOrPastEnd) {
  Storage.reset();
  const ProgressFile::TxtBounds bounds{1000, 10};
  const ProgressFile::CandidateValidator validator{ProgressFile::validateTxtBounds, &bounds};
  uint8_t invalid[ProgressFileCodec::TXT_V2_SIZE];
  ProgressFileCodec::encodeTxtOffset(1000, invalid);

  EXPECT_FALSE(ProgressFile::writeTxtAtomic(CACHE_PATH, invalid, validator));
  EXPECT_FALSE(Storage.exists(PRIMARY));
}
