#include <HalStorage.h>
#include <esp_mac.h>
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "BookReadingStats.h"
#include "GlobalReadingStats.h"
#include "ReadingSessionTracker.h"
#include "ReadingStatsCodec.h"
#include "ReadingStatsCompletionTransaction.h"
#include "ReadingStatsEnvelope.h"
#include "ReadingStatsStorage.h"
#include "ReadingStatsUtils.h"
#include "activities/network/NearbyStatsPolicy.h"
#include "activities/network/NearbyStatsStorage.h"

namespace {
void expectBookStatsEqual(const BookReadingStats& lhs, const BookReadingStats& rhs) {
  EXPECT_EQ(lhs.sessionCount, rhs.sessionCount);
  EXPECT_EQ(lhs.totalReadingSeconds, rhs.totalReadingSeconds);
  EXPECT_EQ(lhs.totalPagesTurned, rhs.totalPagesTurned);
  EXPECT_EQ(lhs.isCompleted, rhs.isCompleted);
  EXPECT_EQ(lhs.avgSecondsPerForwardPage, rhs.avgSecondsPerForwardPage);
  EXPECT_EQ(lhs.paceSampleCount, rhs.paceSampleCount);
  EXPECT_EQ(lhs.estimatedTimeLeftSeconds, rhs.estimatedTimeLeftSeconds);
  EXPECT_EQ(lhs.startDateManual, rhs.startDateManual);
  EXPECT_EQ(lhs.finishedDateManual, rhs.finishedDateManual);
  EXPECT_EQ(lhs.startDate.year, rhs.startDate.year);
  EXPECT_EQ(lhs.startDate.month, rhs.startDate.month);
  EXPECT_EQ(lhs.startDate.day, rhs.startDate.day);
  EXPECT_EQ(lhs.finishedDate.year, rhs.finishedDate.year);
  EXPECT_EQ(lhs.finishedDate.month, rhs.finishedDate.month);
  EXPECT_EQ(lhs.finishedDate.day, rhs.finishedDate.day);
  EXPECT_EQ(lhs.startMinuteOfDay, rhs.startMinuteOfDay);
  EXPECT_EQ(lhs.finishedMinuteOfDay, rhs.finishedMinuteOfDay);
  EXPECT_EQ(lhs.timeOfDaySeconds, rhs.timeOfDaySeconds);
  EXPECT_EQ(lhs.dayOfWeekSeconds, rhs.dayOfWeekSeconds);
}

void expectGlobalStatsEqual(const GlobalReadingStats& lhs, const GlobalReadingStats& rhs) {
  EXPECT_EQ(lhs.totalSessions, rhs.totalSessions);
  EXPECT_EQ(lhs.totalReadingSeconds, rhs.totalReadingSeconds);
  EXPECT_EQ(lhs.totalPagesTurned, rhs.totalPagesTurned);
  EXPECT_EQ(lhs.completedBooks, rhs.completedBooks);
  EXPECT_EQ(lhs.timeOfDaySeconds, rhs.timeOfDaySeconds);
  EXPECT_EQ(lhs.dayOfWeekSeconds, rhs.dayOfWeekSeconds);
  EXPECT_EQ(lhs.readingHistoryAnchorDay, rhs.readingHistoryAnchorDay);
  EXPECT_EQ(lhs.readingHistoryBits, rhs.readingHistoryBits);
  EXPECT_EQ(lhs.longestReadingStreak, rhs.longestReadingStreak);
  EXPECT_EQ(lhs.latestReadingDay, rhs.latestReadingDay);
  EXPECT_EQ(lhs.latestDayReadingSeconds, rhs.latestDayReadingSeconds);
  EXPECT_EQ(lhs.latestDaySessions, rhs.latestDaySessions);
  EXPECT_EQ(lhs.hasLatestDayReadingSeconds, rhs.hasLatestDayReadingSeconds);
}

template <typename Bytes>
std::vector<uint8_t> asVector(const Bytes& bytes) {
  return {bytes.begin(), bytes.end()};
}

template <typename Payload>
std::vector<uint8_t> enveloped(const ReadingStatsEnvelope::Kind kind, const Payload& payload) {
  ReadingStatsEnvelope::Bytes encoded{};
  const size_t size = ReadingStatsEnvelope::encode(kind, payload.data(), payload.size(), encoded);
  return {encoded.begin(), encoded.begin() + static_cast<std::ptrdiff_t>(size)};
}

std::vector<uint8_t> bookEnvelope(const BookReadingStats& stats) {
  return enveloped(ReadingStatsEnvelope::Kind::Book, ReadingStatsCodec::encode(stats));
}

std::vector<uint8_t> globalEnvelope(const GlobalReadingStats& stats,
                                    const ReadingStatsEnvelope::Kind kind = ReadingStatsEnvelope::Kind::Global) {
  return enveloped(kind, ReadingStatsCodec::encode(stats));
}

GlobalReadingStats globalStatsWithSeconds(const uint32_t seconds) {
  GlobalReadingStats stats;
  stats.totalReadingSeconds = seconds;
  return stats;
}

BookReadingStats bookStatsWithSeconds(const uint32_t seconds) {
  BookReadingStats stats;
  stats.totalReadingSeconds = seconds;
  return stats;
}

constexpr char COMPLETION_CACHE_PATH[] = "/.crosspoint/epub_12345";
constexpr char BOOK_STATS_PATH[] = "/.crosspoint/epub_12345/stats_v6.bin";
constexpr char TXT_COMPLETION_CACHE_PATH[] = "/.crosspoint/txt_67890";
constexpr char XTC_COMPLETION_CACHE_PATH[] = "/.crosspoint/xtc_24680";
constexpr char GLOBAL_STATS_PATH[] = "/.crosspoint/global_stats_v4.bin";
constexpr char DAILY_STATS_PATH[] = "/.crosspoint/daily_stats_v1.bin";
constexpr char USER_STATS_BACKUP_PATH[] = "/.crosspoint/stats_backups/device_stats_v1.bin";
constexpr char COMPLETION_MARKER_PATH[] = "/.crosspoint/stats_completion.txn";
constexpr char COMPLETION_MARKER_TEMP_PATH[] = "/.crosspoint/stats_completion.txn.tmp";

struct CompletionTransition {
  BookReadingStats oldBook;
  BookReadingStats newBook;
  GlobalReadingStats oldGlobal;
  GlobalReadingStats newGlobal;
};

CompletionTransition completionTransition(const uint32_t completedBooks = 7) {
  CompletionTransition transition;
  transition.oldBook.totalReadingSeconds = 123;
  transition.oldBook.totalPagesTurned = 9;
  transition.oldBook.estimatedTimeLeftSeconds = 456;
  transition.newBook = transition.oldBook;
  transition.newBook.isCompleted = true;
  transition.newBook.estimatedTimeLeftSeconds = 0;
  transition.newBook.finishedDate = {2026, 7, 21};
  transition.newBook.finishedMinuteOfDay = 14u * 60u + 35u;
  transition.oldGlobal.totalReadingSeconds = 999;
  transition.oldGlobal.totalPagesTurned = 42;
  transition.oldGlobal.completedBooks = completedBooks;
  transition.newGlobal = transition.oldGlobal;
  transition.newGlobal.completedBooks =
      completedBooks == std::numeric_limits<uint32_t>::max() ? completedBooks : completedBooks + 1;
  return transition;
}

void seedCompletionTransition(const CompletionTransition& transition,
                              const std::string& cachePath = COMPLETION_CACHE_PATH) {
  Storage.reset();
  Storage.setFile(cachePath + "/stats_v6.bin", bookEnvelope(transition.oldBook));
  Storage.setFile(GLOBAL_STATS_PATH, globalEnvelope(transition.oldGlobal));
}

void expectStoredTransition(const CompletionTransition& transition, const bool completed,
                            const std::string& cachePath = COMPLETION_CACHE_PATH) {
  BookReadingStats::LoadStatus bookStatus = BookReadingStats::LoadStatus::Invalid;
  GlobalReadingStats::LoadStatus globalStatus = GlobalReadingStats::LoadStatus::Invalid;
  const BookReadingStats book = BookReadingStats::load(cachePath, &bookStatus);
  const GlobalReadingStats global = GlobalReadingStats::load(&globalStatus);
  ASSERT_TRUE(BookReadingStats::isTrustedLoadStatus(bookStatus));
  ASSERT_TRUE(GlobalReadingStats::isTrustedLoadStatus(globalStatus));
  expectBookStatsEqual(book, completed ? transition.newBook : transition.oldBook);
  expectGlobalStatsEqual(global, completed ? transition.newGlobal : transition.oldGlobal);
}

uint32_t markerCrc32(const uint8_t* data, const size_t size) {
  uint32_t crc = UINT32_MAX;
  for (size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; ++bit) crc = (crc >> 1U) ^ (0xEDB88320U & (0U - (crc & 1U)));
  }
  return ~crc;
}

void writeMarkerCrc(std::vector<uint8_t>& marker) {
  const uint32_t crc = markerCrc32(marker.data(), marker.size() - 4);
  for (size_t i = 0; i < 4; ++i) marker[marker.size() - 4 + i] = static_cast<uint8_t>(crc >> (i * 8));
}

std::vector<uint8_t> legacyV1Marker(const std::vector<uint8_t>& current) {
  constexpr size_t HEADER_SIZE = 8;
  constexpr size_t LEGACY_BOOK_SIZE = 73;
  const size_t pathSize = static_cast<size_t>(current[6]) | static_cast<size_t>(current[7]) << 8;
  const size_t currentBookSize = BookReadingStats::CURRENT_FILE_SIZE;
  const size_t globalSize = GlobalReadingStats::CURRENT_FILE_SIZE;
  const size_t sourcePayload = HEADER_SIZE + pathSize;
  std::vector<uint8_t> legacy(current.size() - 2 * (currentBookSize - LEGACY_BOOK_SIZE), 0);
  std::copy(current.begin(), current.begin() + static_cast<std::ptrdiff_t>(sourcePayload), legacy.begin());
  legacy[4] = 1;

  size_t sourceOffset = sourcePayload;
  size_t targetOffset = sourcePayload;
  for (size_t book = 0; book < 2; ++book) {
    std::copy_n(current.begin() + static_cast<std::ptrdiff_t>(sourceOffset), LEGACY_BOOK_SIZE,
                legacy.begin() + static_cast<std::ptrdiff_t>(targetOffset));
    legacy[targetOffset] = 5;
    sourceOffset += currentBookSize;
    targetOffset += LEGACY_BOOK_SIZE;
  }
  std::copy_n(current.begin() + static_cast<std::ptrdiff_t>(sourceOffset), 2 * globalSize,
              legacy.begin() + static_cast<std::ptrdiff_t>(targetOffset));
  writeMarkerCrc(legacy);
  return legacy;
}
}  // namespace

TEST(GlobalReadingStatsBackup, RoundTripsOnlyTheVerifiedLocalDeviceSnapshot) {
  Storage.reset();
  GlobalReadingStats original = globalStatsWithSeconds(1234);
  original.totalSessions = 7;
  original.completedBooks = 3;
  Storage.setFile(GLOBAL_STATS_PATH, globalEnvelope(original));

  EXPECT_EQ(GlobalReadingStats::createBackup(), GlobalReadingStats::BackupResult::Ok);
  EXPECT_TRUE(GlobalReadingStats::hasValidBackup());

  GlobalReadingStats changed = globalStatsWithSeconds(9999);
  changed.totalSessions = 40;
  Storage.setFile(GLOBAL_STATS_PATH, globalEnvelope(changed));
  EXPECT_EQ(GlobalReadingStats::restoreBackup(), GlobalReadingStats::BackupResult::Ok);

  GlobalReadingStats::LoadStatus status = GlobalReadingStats::LoadStatus::Invalid;
  const GlobalReadingStats restored = GlobalReadingStats::load(&status);
  ASSERT_TRUE(GlobalReadingStats::isTrustedLoadStatus(status));
  expectGlobalStatsEqual(restored, original);
}

TEST(GlobalReadingStatsBackup, FailedReplacementKeepsThePreviousBackupRestorable) {
  Storage.reset();
  const GlobalReadingStats first = globalStatsWithSeconds(111);
  Storage.setFile(GLOBAL_STATS_PATH, globalEnvelope(first));
  ASSERT_EQ(GlobalReadingStats::createBackup(), GlobalReadingStats::BackupResult::Ok);
  const std::vector<uint8_t> retained = Storage.file(USER_STATS_BACKUP_PATH);

  const GlobalReadingStats second = globalStatsWithSeconds(222);
  Storage.setFile(GLOBAL_STATS_PATH, globalEnvelope(second));
  Storage.shortWriteOnce();
  EXPECT_EQ(GlobalReadingStats::createBackup(), GlobalReadingStats::BackupResult::IoError);
  EXPECT_EQ(Storage.file(USER_STATS_BACKUP_PATH), retained);

  ASSERT_EQ(GlobalReadingStats::restoreBackup(), GlobalReadingStats::BackupResult::Ok);
  GlobalReadingStats::LoadStatus status = GlobalReadingStats::LoadStatus::Invalid;
  const GlobalReadingStats restored = GlobalReadingStats::load(&status);
  ASSERT_TRUE(GlobalReadingStats::isTrustedLoadStatus(status));
  expectGlobalStatsEqual(restored, first);
}

TEST(GlobalReadingStatsBackup, InvalidOrNewerBackupNeverChangesCurrentStats) {
  for (const bool newer : {false, true}) {
    SCOPED_TRACE(newer);
    Storage.reset();
    const GlobalReadingStats current = globalStatsWithSeconds(777);
    Storage.setFile(GLOBAL_STATS_PATH, globalEnvelope(current));
    if (newer) {
      auto bytes = globalEnvelope(globalStatsWithSeconds(1));
      bytes[4] = ReadingStatsEnvelope::CURRENT_VERSION + 1;
      Storage.setFile(USER_STATS_BACKUP_PATH, std::move(bytes));
      EXPECT_EQ(GlobalReadingStats::restoreBackup(), GlobalReadingStats::BackupResult::NewerFormat);
    } else {
      Storage.setFile(USER_STATS_BACKUP_PATH, {0x01, 0x02, 0x03});
      EXPECT_EQ(GlobalReadingStats::restoreBackup(), GlobalReadingStats::BackupResult::Invalid);
    }

    GlobalReadingStats::LoadStatus status = GlobalReadingStats::LoadStatus::Invalid;
    const GlobalReadingStats loaded = GlobalReadingStats::load(&status);
    ASSERT_TRUE(GlobalReadingStats::isTrustedLoadStatus(status));
    expectGlobalStatsEqual(loaded, current);
  }
}

TEST(GlobalReadingStatsBackup, MissingOrUnreadableSourceCannotCreateAFalseBackup) {
  Storage.reset();
  EXPECT_EQ(GlobalReadingStats::createBackup(), GlobalReadingStats::BackupResult::Missing);
  EXPECT_FALSE(Storage.exists(USER_STATS_BACKUP_PATH));

  Storage.setFile(GLOBAL_STATS_PATH, {0x01, 0x02});
  EXPECT_EQ(GlobalReadingStats::createBackup(), GlobalReadingStats::BackupResult::Invalid);
  EXPECT_FALSE(Storage.exists(USER_STATS_BACKUP_PATH));
}

TEST(ReadingStatsCompletionTransaction, CommitsBothFilesAndSaturatesCompletedBooks) {
  for (const uint32_t initial : {7u, std::numeric_limits<uint32_t>::max()}) {
    const CompletionTransition transition = completionTransition(initial);
    seedCompletionTransition(transition);
    EXPECT_TRUE(ReadingStatsCompletionTransaction::commit(COMPLETION_CACHE_PATH, transition.oldBook, transition.newBook,
                                                          transition.oldGlobal, transition.newGlobal));
    expectStoredTransition(transition, true);
    EXPECT_FALSE(Storage.exists(COMPLETION_MARKER_PATH));
    EXPECT_FALSE(Storage.exists(COMPLETION_MARKER_TEMP_PATH));
  }
}

TEST(ReadingStatsCompletionTransaction, RecoversCompletionForAnExactTxtCacheKey) {
  const CompletionTransition transition = completionTransition();
  seedCompletionTransition(transition, TXT_COMPLETION_CACHE_PATH);
  Storage.failRenameOnCall(2);
  EXPECT_FALSE(ReadingStatsCompletionTransaction::commit(
      TXT_COMPLETION_CACHE_PATH, transition.oldBook, transition.newBook, transition.oldGlobal, transition.newGlobal));
  Storage.resetFaultInjection();

  ASSERT_TRUE(Storage.exists(COMPLETION_MARKER_PATH));
  EXPECT_EQ(ReadingStatsCompletionTransaction::recoverPending(),
            ReadingStatsCompletionTransaction::RecoveryResult::Recovered);
  expectStoredTransition(transition, true, TXT_COMPLETION_CACHE_PATH);
  EXPECT_FALSE(Storage.exists(COMPLETION_MARKER_PATH));
  EXPECT_FALSE(Storage.exists(COMPLETION_MARKER_TEMP_PATH));
}

TEST(ReadingStatsCompletionTransaction, RecoversCompletionForAnExactXtcCacheKey) {
  const CompletionTransition transition = completionTransition();
  seedCompletionTransition(transition, XTC_COMPLETION_CACHE_PATH);
  Storage.failRenameOnCall(2);
  EXPECT_FALSE(ReadingStatsCompletionTransaction::commit(
      XTC_COMPLETION_CACHE_PATH, transition.oldBook, transition.newBook, transition.oldGlobal, transition.newGlobal));
  Storage.resetFaultInjection();

  ASSERT_TRUE(Storage.exists(COMPLETION_MARKER_PATH));
  EXPECT_EQ(ReadingStatsCompletionTransaction::recoverPending(),
            ReadingStatsCompletionTransaction::RecoveryResult::Recovered);
  expectStoredTransition(transition, true, XTC_COMPLETION_CACHE_PATH);
  EXPECT_FALSE(Storage.exists(COMPLETION_MARKER_PATH));
  EXPECT_FALSE(Storage.exists(COMPLETION_MARKER_TEMP_PATH));
}

TEST(ReadingStatsCompletionTransaction, RecoversVersionOneMarkerAfterTimestampUpgrade) {
  CompletionTransition transition = completionTransition();
  transition.newBook.finishedMinuteOfDay = BookReadingStats::INVALID_MINUTE_OF_DAY;
  seedCompletionTransition(transition);
  Storage.failRenameOnCall(2);
  ASSERT_FALSE(ReadingStatsCompletionTransaction::commit(COMPLETION_CACHE_PATH, transition.oldBook, transition.newBook,
                                                         transition.oldGlobal, transition.newGlobal));
  Storage.resetFaultInjection();
  ASSERT_TRUE(Storage.exists(COMPLETION_MARKER_PATH));
  Storage.setFile(COMPLETION_MARKER_PATH, legacyV1Marker(Storage.file(COMPLETION_MARKER_PATH)));

  EXPECT_EQ(ReadingStatsCompletionTransaction::recoverPending(),
            ReadingStatsCompletionTransaction::RecoveryResult::Recovered);
  expectStoredTransition(transition, true);
}

TEST(ReadingStatsCompletionTransaction, RejectsLookalikeAndUnsupportedCacheKeys) {
  const CompletionTransition transition = completionTransition();
  seedCompletionTransition(transition);
  for (const char* cachePath : {"/.crosspoint/txt_", "/.crosspoint/txt_12/3", "/.crosspoint/txt_-1",
                                "/.crosspoint/xtc_", "/.crosspoint/xtc_12x", "/.crosspoint/epub_12x"}) {
    EXPECT_FALSE(ReadingStatsCompletionTransaction::commit(cachePath, transition.oldBook, transition.newBook,
                                                           transition.oldGlobal, transition.newGlobal))
        << cachePath;
    EXPECT_FALSE(Storage.exists(COMPLETION_MARKER_PATH));
    EXPECT_FALSE(Storage.exists(COMPLETION_MARKER_TEMP_PATH));
  }
}

TEST(ReadingStatsCompletionTransaction, RecoversAnOldRawPayloadMarkerWhilePreservingLegacyFiles) {
  const CompletionTransition transition = completionTransition();
  seedCompletionTransition(transition);
  Storage.failRenameOnCall(2);
  ASSERT_FALSE(ReadingStatsCompletionTransaction::commit(COMPLETION_CACHE_PATH, transition.oldBook, transition.newBook,
                                                         transition.oldGlobal, transition.newGlobal));
  Storage.resetFaultInjection();
  ASSERT_TRUE(Storage.exists(COMPLETION_MARKER_PATH));
  const std::vector<uint8_t> marker = Storage.file(COMPLETION_MARKER_PATH);
  const std::vector<uint8_t> rawBook = asVector(ReadingStatsCodec::encode(transition.oldBook));
  const std::vector<uint8_t> rawGlobal = asVector(ReadingStatsCodec::encode(transition.oldGlobal));

  Storage.reset();
  Storage.setFile(COMPLETION_MARKER_PATH, marker);
  Storage.setFile(std::string(COMPLETION_CACHE_PATH) + "/stats_v5.bin", rawBook);
  Storage.setFile("/.crosspoint/global_stats.bin", rawGlobal);
  EXPECT_EQ(ReadingStatsCompletionTransaction::recoverPending(),
            ReadingStatsCompletionTransaction::RecoveryResult::Recovered);
  expectStoredTransition(transition, true);
  EXPECT_EQ(Storage.file(std::string(COMPLETION_CACHE_PATH) + "/stats_v5.bin"), rawBook);
  EXPECT_EQ(Storage.file("/.crosspoint/global_stats.bin"), rawGlobal);
  EXPECT_FALSE(Storage.exists(COMPLETION_MARKER_PATH));
}

TEST(ReadingStatsCompletionTransaction, SupportsExplicitIncompleteWithoutUnderflow) {
  CompletionTransition transition = completionTransition(0);
  transition.oldBook = transition.newBook;
  transition.oldGlobal = transition.newGlobal;
  transition.oldGlobal.completedBooks = 0;
  transition.newBook = transition.oldBook;
  transition.newBook.isCompleted = false;
  transition.newBook.finishedDate.clear();
  transition.newBook.finishedMinuteOfDay = BookReadingStats::INVALID_MINUTE_OF_DAY;
  transition.newGlobal = transition.oldGlobal;
  transition.newGlobal.completedBooks = 0;
  seedCompletionTransition(transition);

  EXPECT_TRUE(ReadingStatsCompletionTransaction::commit(COMPLETION_CACHE_PATH, transition.oldBook, transition.newBook,
                                                        transition.oldGlobal, transition.newGlobal));
  expectStoredTransition(transition, true);
  EXPECT_EQ(GlobalReadingStats::load().completedBooks, 0u);
}

TEST(ReadingStatsCompletionTransaction, RejectsUnrelatedMutations) {
  CompletionTransition transition = completionTransition();
  seedCompletionTransition(transition);
  ++transition.newGlobal.totalReadingSeconds;
  EXPECT_FALSE(ReadingStatsCompletionTransaction::commit(COMPLETION_CACHE_PATH, transition.oldBook, transition.newBook,
                                                         transition.oldGlobal, transition.newGlobal));
  EXPECT_FALSE(Storage.exists(COMPLETION_MARKER_PATH));
  expectStoredTransition(completionTransition(), false);

  transition = completionTransition();
  ++transition.newBook.sessionCount;
  EXPECT_FALSE(ReadingStatsCompletionTransaction::commit(COMPLETION_CACHE_PATH, transition.oldBook, transition.newBook,
                                                         transition.oldGlobal, transition.newGlobal));
  EXPECT_FALSE(Storage.exists(COMPLETION_MARKER_PATH));
}

TEST(ReadingStatsCompletionTransaction, RecoversEveryPublishAndBackupConsolidationBoundary) {
  const CompletionTransition transition = completionTransition();
  for (size_t failedRename = 1; failedRename <= 9; ++failedRename) {
    seedCompletionTransition(transition);
    Storage.failRenameOnCall(failedRename);
    EXPECT_FALSE(ReadingStatsCompletionTransaction::commit(
        COMPLETION_CACHE_PATH, transition.oldBook, transition.newBook, transition.oldGlobal, transition.newGlobal));
    Storage.resetFaultInjection();
    if (failedRename == 1) {
      EXPECT_EQ(ReadingStatsCompletionTransaction::recover(COMPLETION_CACHE_PATH),
                ReadingStatsCompletionTransaction::RecoveryResult::NoMarker);
      expectStoredTransition(transition, false);
    } else {
      ASSERT_TRUE(Storage.exists(COMPLETION_MARKER_PATH));
      EXPECT_EQ(ReadingStatsCompletionTransaction::recover(COMPLETION_CACHE_PATH),
                ReadingStatsCompletionTransaction::RecoveryResult::Recovered);
      expectStoredTransition(transition, true);
    }
  }
}

TEST(ReadingStatsCompletionTransaction, RecoversWriteAndSyncFailuresIncludingBackupConsolidation) {
  const CompletionTransition transition = completionTransition();
  for (size_t failedWrite = 1; failedWrite <= 5; ++failedWrite) {
    seedCompletionTransition(transition);
    Storage.shortWriteOnCall(failedWrite);
    EXPECT_FALSE(ReadingStatsCompletionTransaction::commit(
        COMPLETION_CACHE_PATH, transition.oldBook, transition.newBook, transition.oldGlobal, transition.newGlobal));
    Storage.resetFaultInjection();
    if (failedWrite == 1) {
      expectStoredTransition(transition, false);
    } else {
      EXPECT_EQ(ReadingStatsCompletionTransaction::recover(COMPLETION_CACHE_PATH),
                ReadingStatsCompletionTransaction::RecoveryResult::Recovered);
      expectStoredTransition(transition, true);
    }

    seedCompletionTransition(transition);
    Storage.failSyncOnCall(failedWrite);
    EXPECT_FALSE(ReadingStatsCompletionTransaction::commit(
        COMPLETION_CACHE_PATH, transition.oldBook, transition.newBook, transition.oldGlobal, transition.newGlobal));
    Storage.resetFaultInjection();
    if (failedWrite == 1) {
      expectStoredTransition(transition, false);
    } else {
      EXPECT_EQ(ReadingStatsCompletionTransaction::recover(COMPLETION_CACHE_PATH),
                ReadingStatsCompletionTransaction::RecoveryResult::Recovered);
      expectStoredTransition(transition, true);
    }
  }
}

TEST(ReadingStatsCompletionTransaction, ReplaysCommittedMarkerWithoutDoubleCounting) {
  const CompletionTransition transition = completionTransition();
  seedCompletionTransition(transition);
  // Old backups are removed on calls 1 and 2 while consolidating; call 3 is
  // marker cleanup after both primary/backup pairs are coherent.
  Storage.failRemoveOnCall(3);
  EXPECT_FALSE(ReadingStatsCompletionTransaction::commit(COMPLETION_CACHE_PATH, transition.oldBook, transition.newBook,
                                                         transition.oldGlobal, transition.newGlobal));
  ASSERT_TRUE(Storage.exists(COMPLETION_MARKER_PATH));
  const std::vector<uint8_t> replay = Storage.file(COMPLETION_MARKER_PATH);
  expectStoredTransition(transition, true);

  Storage.resetFaultInjection();
  EXPECT_EQ(ReadingStatsCompletionTransaction::recover(COMPLETION_CACHE_PATH),
            ReadingStatsCompletionTransaction::RecoveryResult::Recovered);
  EXPECT_EQ(Storage.writeCallCount(), 0u);
  EXPECT_EQ(Storage.renameCallCount(), 0u);
  Storage.setFile(COMPLETION_MARKER_PATH, replay);
  Storage.resetFaultInjection();
  EXPECT_EQ(ReadingStatsCompletionTransaction::recover(COMPLETION_CACHE_PATH),
            ReadingStatsCompletionTransaction::RecoveryResult::Recovered);
  EXPECT_EQ(Storage.writeCallCount(), 0u);
  EXPECT_EQ(Storage.renameCallCount(), 0u);
  expectStoredTransition(transition, true);
}

TEST(ReadingStatsCompletionTransaction, CompletedBackupsCannotResurrectPreTransitionCounts) {
  const CompletionTransition transition = completionTransition();
  for (const bool loseBookPrimary : {true, false}) {
    seedCompletionTransition(transition);
    ASSERT_TRUE(ReadingStatsCompletionTransaction::commit(COMPLETION_CACHE_PATH, transition.oldBook, transition.newBook,
                                                          transition.oldGlobal, transition.newGlobal));
    const char* primaryPath = loseBookPrimary ? BOOK_STATS_PATH : GLOBAL_STATS_PATH;
    std::vector<uint8_t> corrupt = Storage.file(primaryPath);
    corrupt[ReadingStatsEnvelope::HEADER_SIZE + 3] ^= 0x20;
    Storage.setFile(primaryPath, corrupt);

    BookReadingStats::LoadStatus bookStatus = BookReadingStats::LoadStatus::Invalid;
    GlobalReadingStats::LoadStatus globalStatus = GlobalReadingStats::LoadStatus::Invalid;
    const BookReadingStats book = BookReadingStats::load(COMPLETION_CACHE_PATH, &bookStatus);
    const GlobalReadingStats global = GlobalReadingStats::load(&globalStatus);
    expectBookStatsEqual(book, transition.newBook);
    expectGlobalStatsEqual(global, transition.newGlobal);
    EXPECT_EQ(bookStatus,
              loseBookPrimary ? BookReadingStats::LoadStatus::RecoveredBackup : BookReadingStats::LoadStatus::Ok);
    EXPECT_EQ(globalStatus,
              loseBookPrimary ? GlobalReadingStats::LoadStatus::Ok : GlobalReadingStats::LoadStatus::RecoveredBackup);
    EXPECT_FALSE(Storage.exists(COMPLETION_MARKER_PATH));
  }
}

TEST(ReadingStatsCompletionTransaction, RejectsCorruptNewerAndMismatchedMarkers) {
  const CompletionTransition transition = completionTransition();
  const auto preparePending = [&] {
    seedCompletionTransition(transition);
    Storage.failRenameOnCall(2);
    EXPECT_FALSE(ReadingStatsCompletionTransaction::commit(
        COMPLETION_CACHE_PATH, transition.oldBook, transition.newBook, transition.oldGlobal, transition.newGlobal));
    Storage.resetFaultInjection();
    ASSERT_TRUE(Storage.exists(COMPLETION_MARKER_PATH));
  };

  preparePending();
  std::vector<uint8_t> marker = Storage.file(COMPLETION_MARKER_PATH);
  marker.back() ^= 0x80;
  Storage.setFile(COMPLETION_MARKER_PATH, marker);
  EXPECT_EQ(ReadingStatsCompletionTransaction::recover(COMPLETION_CACHE_PATH),
            ReadingStatsCompletionTransaction::RecoveryResult::Blocked);
  expectStoredTransition(transition, false);

  preparePending();
  marker = Storage.file(COMPLETION_MARKER_PATH);
  marker[4] = 3;
  Storage.setFile(COMPLETION_MARKER_PATH, marker);
  EXPECT_EQ(ReadingStatsCompletionTransaction::recover(COMPLETION_CACHE_PATH),
            ReadingStatsCompletionTransaction::RecoveryResult::Blocked);
  expectStoredTransition(transition, false);

  preparePending();
  EXPECT_EQ(ReadingStatsCompletionTransaction::recover("/.crosspoint/epub_99999"),
            ReadingStatsCompletionTransaction::RecoveryResult::Blocked);
  expectStoredTransition(transition, false);

  preparePending();
  BookReadingStats different = transition.oldBook;
  ++different.totalReadingSeconds;
  Storage.setFile(BOOK_STATS_PATH, bookEnvelope(different));
  EXPECT_EQ(ReadingStatsCompletionTransaction::recover(COMPLETION_CACHE_PATH),
            ReadingStatsCompletionTransaction::RecoveryResult::Blocked);
  EXPECT_EQ(GlobalReadingStats::load().completedBooks, transition.oldGlobal.completedBooks);
}

TEST(ReadingStatsCompletionTransaction, RecoversPendingBookIndependentOfTheCurrentReader) {
  const CompletionTransition transition = completionTransition();
  seedCompletionTransition(transition);
  Storage.failRenameOnCall(2);
  EXPECT_FALSE(ReadingStatsCompletionTransaction::commit(COMPLETION_CACHE_PATH, transition.oldBook, transition.newBook,
                                                         transition.oldGlobal, transition.newGlobal));
  Storage.resetFaultInjection();

  // A caller-bound attempt for another book stays fail-closed, while the
  // marker-bound entry point used by EpubReaderActivity safely recovers A.
  EXPECT_EQ(ReadingStatsCompletionTransaction::recover("/.crosspoint/epub_99999"),
            ReadingStatsCompletionTransaction::RecoveryResult::Blocked);
  EXPECT_EQ(ReadingStatsCompletionTransaction::recoverPending(),
            ReadingStatsCompletionTransaction::RecoveryResult::Recovered);
  expectStoredTransition(transition, true);
}

TEST(ReadingStatsCompletionTransaction, RejectsValidCrcMarkerOutsideTrackedBookCacheNamespace) {
  const CompletionTransition transition = completionTransition();
  seedCompletionTransition(transition);
  Storage.failRenameOnCall(2);
  EXPECT_FALSE(ReadingStatsCompletionTransaction::commit(COMPLETION_CACHE_PATH, transition.oldBook, transition.newBook,
                                                         transition.oldGlobal, transition.newGlobal));
  Storage.resetFaultInjection();

  std::vector<uint8_t> marker = Storage.file(COMPLETION_MARKER_PATH);
  constexpr char forgedPath[] = "/.crosspoint/evil_12345";
  static_assert(sizeof(forgedPath) == sizeof(COMPLETION_CACHE_PATH));
  memcpy(marker.data() + 8, forgedPath, sizeof(forgedPath) - 1);
  writeMarkerCrc(marker);
  Storage.setFile(COMPLETION_MARKER_PATH, marker);
  EXPECT_EQ(ReadingStatsCompletionTransaction::recoverPending(),
            ReadingStatsCompletionTransaction::RecoveryResult::Blocked);
  expectStoredTransition(transition, false);
}

TEST(ReadingStatsCompletionTransaction, RecoversMarkerTempAndProtectsMissingIdentity) {
  const CompletionTransition transition = completionTransition();
  seedCompletionTransition(transition);
  Storage.failRenameOnCall(2);
  EXPECT_FALSE(ReadingStatsCompletionTransaction::commit(COMPLETION_CACHE_PATH, transition.oldBook, transition.newBook,
                                                         transition.oldGlobal, transition.newGlobal));
  Storage.resetFaultInjection();
  const std::vector<uint8_t> marker = Storage.file(COMPLETION_MARKER_PATH);
  ASSERT_TRUE(Storage.remove(COMPLETION_MARKER_PATH));
  Storage.setFile(COMPLETION_MARKER_TEMP_PATH, marker);
  EXPECT_EQ(ReadingStatsCompletionTransaction::recover(COMPLETION_CACHE_PATH),
            ReadingStatsCompletionTransaction::RecoveryResult::Recovered);
  expectStoredTransition(transition, true);

  CompletionTransition missing = completionTransition(0);
  missing.oldBook = {};
  missing.newBook = missing.oldBook;
  missing.newBook.isCompleted = true;
  missing.oldGlobal = {};
  missing.newGlobal = missing.oldGlobal;
  missing.newGlobal.completedBooks = 1;
  Storage.reset();
  Storage.failRenameOnCall(2);
  EXPECT_FALSE(ReadingStatsCompletionTransaction::commit(COMPLETION_CACHE_PATH, missing.oldBook, missing.newBook,
                                                         missing.oldGlobal, missing.newGlobal));
  Storage.resetFaultInjection();
  Storage.setFile(BOOK_STATS_PATH, bookEnvelope(missing.oldBook));
  EXPECT_EQ(ReadingStatsCompletionTransaction::recover(COMPLETION_CACHE_PATH),
            ReadingStatsCompletionTransaction::RecoveryResult::Blocked);
}

TEST(ReadingStatsCompletionTransaction, PublishesRecoveredStatsTempsBeforeRemovingMarker) {
  const CompletionTransition transition = completionTransition();
  seedCompletionTransition(transition);
  Storage.failRenameOnCall(2);
  EXPECT_FALSE(ReadingStatsCompletionTransaction::commit(COMPLETION_CACHE_PATH, transition.oldBook, transition.newBook,
                                                         transition.oldGlobal, transition.newGlobal));
  Storage.resetFaultInjection();

  ASSERT_TRUE(Storage.remove(BOOK_STATS_PATH));
  ASSERT_TRUE(Storage.remove(GLOBAL_STATS_PATH));
  Storage.setFile(std::string(BOOK_STATS_PATH) + ".tmp", bookEnvelope(transition.newBook));
  Storage.setFile(std::string(GLOBAL_STATS_PATH) + ".tmp", globalEnvelope(transition.newGlobal));
  EXPECT_EQ(ReadingStatsCompletionTransaction::recoverPending(),
            ReadingStatsCompletionTransaction::RecoveryResult::Recovered);
  EXPECT_TRUE(Storage.exists(BOOK_STATS_PATH));
  EXPECT_TRUE(Storage.exists(GLOBAL_STATS_PATH));
  const std::string bookTempPath = std::string(BOOK_STATS_PATH) + ".tmp";
  const std::string globalTempPath = std::string(GLOBAL_STATS_PATH) + ".tmp";
  EXPECT_FALSE(Storage.exists(bookTempPath.c_str()));
  EXPECT_FALSE(Storage.exists(globalTempPath.c_str()));
  expectStoredTransition(transition, true);
}

TEST(ReadingStatsCompletionTransaction, PendingMarkerBlocksUnrelatedWritesAndBookReset) {
  const CompletionTransition transition = completionTransition();
  seedCompletionTransition(transition);
  Storage.failRenameOnCall(2);
  EXPECT_FALSE(ReadingStatsCompletionTransaction::commit(COMPLETION_CACHE_PATH, transition.oldBook, transition.newBook,
                                                         transition.oldGlobal, transition.newGlobal));
  Storage.resetFaultInjection();

  GlobalReadingStats unrelatedGlobal = transition.oldGlobal;
  ++unrelatedGlobal.totalReadingSeconds;
  EXPECT_FALSE(unrelatedGlobal.save());
  BookReadingStats unrelatedBook = transition.oldBook;
  ++unrelatedBook.totalReadingSeconds;
  EXPECT_FALSE(unrelatedBook.save(COMPLETION_CACHE_PATH));
  EXPECT_FALSE(BookReadingStats::remove(COMPLETION_CACHE_PATH));
  expectStoredTransition(transition, false);
}

TEST(ReadingStatsCompletionTransaction, PreflightRejectsProtectedSiblingsBeforePublishingMarker) {
  const CompletionTransition transition = completionTransition();
  const std::array<const char*, 4> paths = {
      "/.crosspoint/epub_12345/stats_v6.bin.bak", "/.crosspoint/epub_12345/stats_v6.bin.tmp",
      "/.crosspoint/global_stats_v4.bin.bak", "/.crosspoint/global_stats_v4.bin.tmp"};
  for (size_t pathIndex = 0; pathIndex < paths.size(); ++pathIndex) {
    const bool bookPath = pathIndex < 2;
    for (size_t protection = 0; protection < 3; ++protection) {
      seedCompletionTransition(transition);
      std::vector<uint8_t> sibling = bookPath ? bookEnvelope(transition.oldBook) : globalEnvelope(transition.oldGlobal);
      if (protection == 0) sibling[4] = ReadingStatsEnvelope::CURRENT_VERSION + 1;
      if (protection == 1) {
        sibling = bookPath ? globalEnvelope(transition.oldGlobal) : bookEnvelope(transition.oldBook);
      }
      Storage.setFile(paths[pathIndex], sibling);
      if (protection == 2) Storage.makeUnreadable(paths[pathIndex]);
      const std::vector<uint8_t> bookBefore = Storage.file(BOOK_STATS_PATH);
      const std::vector<uint8_t> globalBefore = Storage.file(GLOBAL_STATS_PATH);

      EXPECT_FALSE(ReadingStatsCompletionTransaction::commit(
          COMPLETION_CACHE_PATH, transition.oldBook, transition.newBook, transition.oldGlobal, transition.newGlobal))
          << pathIndex << ":" << protection;
      EXPECT_FALSE(Storage.exists(COMPLETION_MARKER_PATH));
      EXPECT_FALSE(Storage.exists(COMPLETION_MARKER_TEMP_PATH));
      EXPECT_EQ(Storage.file(BOOK_STATS_PATH), bookBefore);
      EXPECT_EQ(Storage.file(GLOBAL_STATS_PATH), globalBefore);
      EXPECT_EQ(Storage.file(paths[pathIndex]), sibling);
    }
  }
}

TEST(ReadingStatsCompletionTransaction, InvalidTempOnlyMarkerIsCleanedButFutureTempFailsClosed) {
  const CompletionTransition transition = completionTransition();
  seedCompletionTransition(transition);
  Storage.shortWriteOnCall(2);
  ASSERT_FALSE(ReadingStatsCompletionTransaction::commit(COMPLETION_CACHE_PATH, transition.oldBook, transition.newBook,
                                                         transition.oldGlobal, transition.newGlobal));
  Storage.resetFaultInjection();
  const std::vector<uint8_t> validMarker = Storage.file(COMPLETION_MARKER_PATH);

  std::vector<uint8_t> badCrc = validMarker;
  badCrc.back() ^= 0x80;
  const std::array<std::vector<uint8_t>, 3> invalidTemps = {
      std::vector<uint8_t>{}, std::vector<uint8_t>(validMarker.begin(), validMarker.begin() + 8), badCrc};
  for (const std::vector<uint8_t>& invalid : invalidTemps) {
    Storage.reset();
    Storage.setFile(COMPLETION_MARKER_TEMP_PATH, invalid);
    EXPECT_EQ(ReadingStatsCompletionTransaction::recoverPending(),
              ReadingStatsCompletionTransaction::RecoveryResult::NoMarker);
    EXPECT_FALSE(Storage.exists(COMPLETION_MARKER_TEMP_PATH));
  }

  Storage.reset();
  std::vector<uint8_t> future = validMarker;
  future[4] = 2;
  Storage.setFile(COMPLETION_MARKER_TEMP_PATH, future);
  EXPECT_EQ(ReadingStatsCompletionTransaction::recoverPending(),
            ReadingStatsCompletionTransaction::RecoveryResult::Blocked);
  EXPECT_EQ(Storage.file(COMPLETION_MARKER_TEMP_PATH), future);
}

TEST(ReadingStatsCompletionTransaction, ValidPrimaryMarkerIgnoresStaleInvalidTemp) {
  const CompletionTransition transition = completionTransition();
  seedCompletionTransition(transition);
  Storage.shortWriteOnCall(2);
  ASSERT_FALSE(ReadingStatsCompletionTransaction::commit(COMPLETION_CACHE_PATH, transition.oldBook, transition.newBook,
                                                         transition.oldGlobal, transition.newGlobal));
  Storage.resetFaultInjection();
  ASSERT_TRUE(Storage.exists(COMPLETION_MARKER_PATH));
  Storage.setFile(COMPLETION_MARKER_TEMP_PATH, {0xA5});
  EXPECT_EQ(ReadingStatsCompletionTransaction::recoverPending(),
            ReadingStatsCompletionTransaction::RecoveryResult::Recovered);
  EXPECT_FALSE(Storage.exists(COMPLETION_MARKER_PATH));
  EXPECT_FALSE(Storage.exists(COMPLETION_MARKER_TEMP_PATH));
  expectStoredTransition(transition, true);
}

TEST(ReadingStatsCompletionTransaction, PendingMarkerBlocksGlobalResetWithoutMutation) {
  const CompletionTransition transition = completionTransition();
  seedCompletionTransition(transition);
  Storage.shortWriteOnCall(2);
  ASSERT_FALSE(ReadingStatsCompletionTransaction::commit(COMPLETION_CACHE_PATH, transition.oldBook, transition.newBook,
                                                         transition.oldGlobal, transition.newGlobal));
  Storage.resetFaultInjection();
  const std::vector<uint8_t> globalBefore = Storage.file(GLOBAL_STATS_PATH);
  EXPECT_FALSE(GlobalReadingStats::resetLocal());
  EXPECT_EQ(Storage.file(GLOBAL_STATS_PATH), globalBefore);
  EXPECT_TRUE(Storage.exists(COMPLETION_MARKER_PATH));
  EXPECT_EQ(ReadingStatsCompletionTransaction::recoverPending(),
            ReadingStatsCompletionTransaction::RecoveryResult::Recovered);
  expectStoredTransition(transition, true);
}

TEST(ReadingStatsCodec, BookV6RoundTripPreservesExactTimestamps) {
  BookReadingStats input;
  input.sessionCount = 42;
  input.totalReadingSeconds = 123456;
  input.totalPagesTurned = 9876;
  input.isCompleted = true;
  input.avgSecondsPerForwardPage = 37;
  input.paceSampleCount = 91;
  input.estimatedTimeLeftSeconds = 6543;
  input.startDateManual = true;
  input.finishedDateManual = true;
  input.startDate = {2024, 2, 29};
  input.finishedDate = {2025, 12, 31};
  input.startMinuteOfDay = 14u * 60u + 35u;
  input.finishedMinuteOfDay = 23u * 60u + 59u;
  input.timeOfDaySeconds = {1, 2, 3, 4};
  input.dayOfWeekSeconds = {5, 6, 7, 8, 9, 10, 11};

  const auto encoded = ReadingStatsCodec::encode(input);
  EXPECT_EQ(encoded.size(), 77u);
  EXPECT_EQ(encoded[0], 6);
  BookReadingStats output;
  EXPECT_EQ(ReadingStatsCodec::decode(encoded.data(), encoded.size(), output), ReadingStatsDecodeResult::Ok);
  expectBookStatsEqual(input, output);

  auto legacyV5 = encoded;
  legacyV5[0] = 5;
  EXPECT_EQ(ReadingStatsCodec::decode(legacyV5.data(), 73, output), ReadingStatsDecodeResult::Ok);
  EXPECT_EQ(output.startDate.year, input.startDate.year);
  EXPECT_EQ(output.finishedDate.year, input.finishedDate.year);
  EXPECT_EQ(output.startMinuteOfDay, BookReadingStats::INVALID_MINUTE_OF_DAY);
  EXPECT_EQ(output.finishedMinuteOfDay, BookReadingStats::INVALID_MINUTE_OF_DAY);
}

TEST(ReadingStatsCodec, BookReadsLegacyV1AndRejectsUnsafeFiles) {
  std::array<uint8_t, 11> legacy{1, 0x34, 0x12, 0x78, 0x56, 0x34, 0x12, 4, 3, 2, 1};
  BookReadingStats stats;
  EXPECT_EQ(ReadingStatsCodec::decode(legacy.data(), legacy.size(), stats), ReadingStatsDecodeResult::Ok);
  EXPECT_EQ(stats.sessionCount, 0x1234);
  EXPECT_EQ(stats.totalReadingSeconds, 0x12345678u);
  EXPECT_EQ(stats.totalPagesTurned, 0x01020304u);

  auto current = ReadingStatsCodec::encode(stats);
  EXPECT_EQ(ReadingStatsCodec::decode(current.data(), current.size() - 1, stats), ReadingStatsDecodeResult::Invalid);
  current[0] = BookReadingStats::CURRENT_FILE_VERSION + 1;
  EXPECT_EQ(ReadingStatsCodec::decode(current.data(), current.size(), stats), ReadingStatsDecodeResult::NewerFormat);
  current[0] = 0;
  EXPECT_EQ(ReadingStatsCodec::decode(current.data(), current.size(), stats), ReadingStatsDecodeResult::Invalid);
}

TEST(ReadingStatsCodec, GlobalV3RoundTripAndLegacyV1) {
  GlobalReadingStats input;
  input.totalSessions = 12;
  input.totalReadingSeconds = 3456;
  input.totalPagesTurned = 789;
  input.completedBooks = 4;
  input.timeOfDaySeconds = {10, 20, 30, 40};
  input.dayOfWeekSeconds = {1, 2, 3, 4, 5, 6, 7};
  input.readingHistoryAnchorDay = readingStatsDayIndex({2025, 7, 20});
  input.readingHistoryBits[0] = 0x07;
  input.longestReadingStreak = 3;

  const auto encoded = ReadingStatsCodec::encode(input);
  EXPECT_EQ(encoded.size(), 159u);
  EXPECT_EQ(encoded[0], 3);
  GlobalReadingStats output;
  EXPECT_EQ(ReadingStatsCodec::decode(encoded.data(), encoded.size(), output), ReadingStatsDecodeResult::Ok);
  expectGlobalStatsEqual(input, output);

  std::array<uint8_t, 13> legacy{1, 1, 0, 0, 0, 2, 0, 0, 0, 3, 0, 0, 0};
  EXPECT_EQ(ReadingStatsCodec::decode(legacy.data(), legacy.size(), output), ReadingStatsDecodeResult::Ok);
  EXPECT_EQ(output.totalSessions, 1u);
  EXPECT_EQ(output.totalReadingSeconds, 2u);
  EXPECT_EQ(output.totalPagesTurned, 3u);
  EXPECT_EQ(output.completedBooks, 0u);
}

TEST(ReadingStatsCodec, GlobalRejectsTruncatedCorruptAndNewerFormats) {
  GlobalReadingStats stats;
  auto encoded = ReadingStatsCodec::encode(stats);
  EXPECT_EQ(ReadingStatsCodec::decode(encoded.data(), 158, stats), ReadingStatsDecodeResult::Invalid);
  encoded[0] = 0;
  EXPECT_EQ(ReadingStatsCodec::decode(encoded.data(), encoded.size(), stats), ReadingStatsDecodeResult::Invalid);
  encoded[0] = GlobalReadingStats::CURRENT_FILE_VERSION + 1;
  EXPECT_EQ(ReadingStatsCodec::decode(encoded.data(), encoded.size(), stats), ReadingStatsDecodeResult::NewerFormat);
  std::vector<uint8_t> oversized(GlobalReadingStats::CURRENT_FILE_SIZE + 1, 0);
  oversized[0] = GlobalReadingStats::CURRENT_FILE_VERSION;
  EXPECT_EQ(ReadingStatsCodec::decode(oversized.data(), oversized.size(), stats),
            ReadingStatsDecodeResult::NewerFormat);
}

TEST(ReadingStatsEnvelope, UsesUniqueMagicKindLengthAndCrcToRejectEveryCorruptionClass) {
  const std::vector<uint8_t> valid = globalEnvelope(globalStatsWithSeconds(123));
  ASSERT_GE(valid.size(), ReadingStatsEnvelope::HEADER_SIZE + ReadingStatsEnvelope::CRC_SIZE);
  EXPECT_EQ(std::vector<uint8_t>(valid.begin(), valid.begin() + 4), (std::vector<uint8_t>{'C', 'V', 'S', 'E'}));
  EXPECT_EQ(valid[4], ReadingStatsEnvelope::CURRENT_VERSION);
  EXPECT_EQ(valid[5], static_cast<uint8_t>(ReadingStatsEnvelope::Kind::Global));

  const uint8_t* payload = nullptr;
  size_t payloadSize = 0;
  EXPECT_EQ(ReadingStatsEnvelope::decode(valid.data(), valid.size(), ReadingStatsEnvelope::Kind::Global, payload,
                                         payloadSize),
            ReadingStatsEnvelope::DecodeResult::Ok);
  EXPECT_EQ(payloadSize, GlobalReadingStats::CURRENT_FILE_SIZE);

  for (const size_t flippedIndex : {size_t{0}, size_t{6}, ReadingStatsEnvelope::HEADER_SIZE + 9, valid.size() - 1}) {
    std::vector<uint8_t> corrupt = valid;
    corrupt[flippedIndex] ^= 0x40;
    EXPECT_EQ(ReadingStatsEnvelope::decode(corrupt.data(), corrupt.size(), ReadingStatsEnvelope::Kind::Global, payload,
                                           payloadSize),
              ReadingStatsEnvelope::DecodeResult::Invalid)
        << flippedIndex;
  }

  EXPECT_EQ(ReadingStatsEnvelope::decode(valid.data(), valid.size(), ReadingStatsEnvelope::Kind::PeerGlobal, payload,
                                         payloadSize),
            ReadingStatsEnvelope::DecodeResult::WrongKind);
  std::vector<uint8_t> newer = valid;
  newer[4] = ReadingStatsEnvelope::CURRENT_VERSION + 1;
  EXPECT_EQ(ReadingStatsEnvelope::decode(newer.data(), newer.size(), ReadingStatsEnvelope::Kind::Global, payload,
                                         payloadSize),
            ReadingStatsEnvelope::DecodeResult::NewerFormat);
}

TEST(ReadingStatsEnvelope, ValidLargerBookPayloadIsProtectedAsAForwardFormat) {
  for (const size_t payloadSize : {BookReadingStats::CURRENT_FILE_SIZE + 1, ReadingStatsEnvelope::MAX_PAYLOAD_SIZE}) {
    std::vector<uint8_t> futurePayload(payloadSize, 0);
    futurePayload[0] = BookReadingStats::CURRENT_FILE_VERSION;
    const std::vector<uint8_t> futureEnvelope = enveloped(ReadingStatsEnvelope::Kind::Book, futurePayload);
    ASSERT_FALSE(futureEnvelope.empty());

    ReadingStatsCodec::BookBytes payload{};
    Storage.reset();
    Storage.setFile("/book/stats_v6.bin", futureEnvelope);
    const ReadingStatsEnvelope::ReadOutcome read = ReadingStatsEnvelope::read(
        "/book/stats_v6.bin", ReadingStatsEnvelope::Kind::Book, payload.data(), payload.size());
    EXPECT_EQ(read.readResult, ReadingStatsStorage::ReadResult::Ok);
    EXPECT_EQ(read.decodeResult, ReadingStatsEnvelope::DecodeResult::PayloadTooLarge);

    BookReadingStats::LoadStatus status = BookReadingStats::LoadStatus::Ok;
    EXPECT_EQ(BookReadingStats::load("/book", &status).totalReadingSeconds, 0u);
    EXPECT_EQ(status, BookReadingStats::LoadStatus::NewerFormat);
    EXPECT_FALSE(BookReadingStats{}.save("/book"));
    EXPECT_FALSE(BookReadingStats::remove("/book"));
    EXPECT_EQ(Storage.file("/book/stats_v6.bin"), futureEnvelope);
  }
}

TEST(ReadingStatsStorage, ProtectsUnreadableOversizedAndNewerSiblingFiles) {
  using ReadingStatsStorage::ReadResult;
  EXPECT_FALSE(ReadingStatsStorage::isProtectedExistingFile(ReadResult::Missing, false));
  EXPECT_FALSE(ReadingStatsStorage::isProtectedExistingFile(ReadResult::Ok, false));
  EXPECT_TRUE(ReadingStatsStorage::isProtectedExistingFile(ReadResult::Ok, true));
  EXPECT_TRUE(ReadingStatsStorage::isProtectedExistingFile(ReadResult::TooLarge, false));
  EXPECT_TRUE(ReadingStatsStorage::isProtectedExistingFile(ReadResult::IoError, false));
}

TEST(NearbyStatsPolicy, AllowsOnlyNonRegressingCumulativeSnapshots) {
  GlobalReadingStats existing;
  existing.totalSessions = 10;
  existing.totalReadingSeconds = 20;
  existing.totalPagesTurned = 30;
  existing.completedBooks = 4;
  existing.timeOfDaySeconds = {5, 6, 7, 8};
  existing.dayOfWeekSeconds = {9, 10, 11, 12, 13, 14, 15};
  existing.longestReadingStreak = 3;

  GlobalReadingStats incoming = existing;
  EXPECT_TRUE(NearbyStatsPolicy::doesNotRegress(incoming, existing));

  ++incoming.totalSessions;
  ++incoming.totalReadingSeconds;
  ++incoming.totalPagesTurned;
  ++incoming.completedBooks;
  for (uint32_t& seconds : incoming.timeOfDaySeconds) ++seconds;
  for (uint32_t& seconds : incoming.dayOfWeekSeconds) ++seconds;
  ++incoming.longestReadingStreak;
  EXPECT_TRUE(NearbyStatsPolicy::doesNotRegress(incoming, existing));

  incoming = existing;
  --incoming.totalSessions;
  EXPECT_FALSE(NearbyStatsPolicy::doesNotRegress(incoming, existing));

  incoming = existing;
  --incoming.totalReadingSeconds;
  EXPECT_FALSE(NearbyStatsPolicy::doesNotRegress(incoming, existing));

  incoming = existing;
  --incoming.totalPagesTurned;
  EXPECT_FALSE(NearbyStatsPolicy::doesNotRegress(incoming, existing));

  incoming = existing;
  --incoming.completedBooks;
  EXPECT_FALSE(NearbyStatsPolicy::doesNotRegress(incoming, existing));

  incoming = existing;
  --incoming.timeOfDaySeconds[2];
  EXPECT_FALSE(NearbyStatsPolicy::doesNotRegress(incoming, existing));

  incoming = existing;
  --incoming.dayOfWeekSeconds[5];
  EXPECT_FALSE(NearbyStatsPolicy::doesNotRegress(incoming, existing));

  incoming = existing;
  --incoming.longestReadingStreak;
  EXPECT_FALSE(NearbyStatsPolicy::doesNotRegress(incoming, existing));
}

TEST(NearbyStatsPolicy, PreservesHistoryAtEqualAnchorAndAllowsOnlyForwardWindowMovement) {
  const auto setHistoryBit = [](auto& bits, const size_t index) {
    bits[index / 8] |= static_cast<uint8_t>(1u << (index % 8));
  };
  GlobalReadingStats existing;
  existing.readingHistoryAnchorDay = 100;
  setHistoryBit(existing.readingHistoryBits, 0);
  setHistoryBit(existing.readingHistoryBits, 4);
  setHistoryBit(existing.readingHistoryBits, READING_HISTORY_DAYS - 1);

  GlobalReadingStats incoming = existing;
  setHistoryBit(incoming.readingHistoryBits, 1);
  EXPECT_TRUE(NearbyStatsPolicy::doesNotRegress(incoming, existing));

  incoming = existing;
  incoming.readingHistoryBits[0] &= static_cast<uint8_t>(~0b00000001);
  EXPECT_FALSE(NearbyStatsPolicy::doesNotRegress(incoming, existing));

  incoming = existing;
  incoming.readingHistoryAnchorDay = 99;
  EXPECT_FALSE(NearbyStatsPolicy::doesNotRegress(incoming, existing));

  incoming.readingHistoryAnchorDay = 101;
  incoming.readingHistoryBits.fill(0);
  setHistoryBit(incoming.readingHistoryBits, 1);
  setHistoryBit(incoming.readingHistoryBits, 5);
  EXPECT_TRUE(NearbyStatsPolicy::doesNotRegress(incoming, existing));

  incoming.readingHistoryBits[0] &= static_cast<uint8_t>(~0b00000010);
  EXPECT_FALSE(NearbyStatsPolicy::doesNotRegress(incoming, existing));

  incoming.readingHistoryAnchorDay = 110;
  incoming.readingHistoryBits.fill(0);
  setHistoryBit(incoming.readingHistoryBits, 10);
  setHistoryBit(incoming.readingHistoryBits, 14);
  EXPECT_TRUE(NearbyStatsPolicy::doesNotRegress(incoming, existing));

  incoming.readingHistoryAnchorDay = existing.readingHistoryAnchorDay + READING_HISTORY_DAYS;
  incoming.readingHistoryBits.fill(0xFF);
  EXPECT_FALSE(NearbyStatsPolicy::doesNotRegress(incoming, existing));

  existing = {};
  setHistoryBit(existing.readingHistoryBits, 0);  // Day-index zero is initialized, not empty.
  incoming = {};
  incoming.readingHistoryAnchorDay = 1;
  setHistoryBit(incoming.readingHistoryBits, 1);
  EXPECT_TRUE(NearbyStatsPolicy::doesNotRegress(incoming, existing));
  incoming.readingHistoryBits.fill(0);
  EXPECT_FALSE(NearbyStatsPolicy::doesNotRegress(incoming, existing));

  existing = {};
  incoming = {};
  incoming.readingHistoryAnchorDay = 100;
  setHistoryBit(incoming.readingHistoryBits, 0);
  EXPECT_TRUE(NearbyStatsPolicy::doesNotRegress(incoming, existing));
}

TEST(ReadingStatsModels, AggregationAndBucketsSaturateInsteadOfWrapping) {
  constexpr uint32_t MAX = std::numeric_limits<uint32_t>::max();
  GlobalReadingStats total;
  total.totalSessions = MAX - 1;
  total.totalReadingSeconds = MAX - 2;
  total.totalPagesTurned = MAX - 3;
  total.completedBooks = MAX - 4;
  total.timeOfDaySeconds[0] = MAX - 5;
  total.dayOfWeekSeconds[0] = MAX - 6;
  GlobalReadingStats peer;
  peer.totalSessions = peer.totalReadingSeconds = peer.totalPagesTurned = peer.completedBooks = 10;
  peer.timeOfDaySeconds[0] = 10;
  peer.dayOfWeekSeconds[0] = 10;
  total.merge(peer);
  EXPECT_EQ(total.totalSessions, MAX);
  EXPECT_EQ(total.totalReadingSeconds, MAX);
  EXPECT_EQ(total.totalPagesTurned, MAX);
  EXPECT_EQ(total.completedBooks, MAX);
  EXPECT_EQ(total.timeOfDaySeconds[0], MAX);
  EXPECT_EQ(total.dayOfWeekSeconds[0], MAX);

  std::array<uint32_t, READING_TIME_BUCKET_COUNT> buckets{MAX - 5, 0, 0, 0};
  std::array<uint32_t, READING_DAY_OF_WEEK_COUNT> days{MAX - 5, 0, 0, 0, 0, 0, 0};
  recordReadingSpanIntoBuckets(buckets, days, {{2024, 1, 1}, 6, 0, 0}, 10);
  EXPECT_EQ(buckets[0], MAX);
  EXPECT_EQ(days[0], MAX);  // 2024-01-01 was Monday.
}

TEST(ReadingStatsModels, PaceUsesBoundedRunningAverage) {
  BookReadingStats stats;
  stats.recordForwardPageRead(10);
  stats.recordForwardPageRead(20);
  EXPECT_EQ(stats.avgSecondsPerForwardPage, 15);
  EXPECT_EQ(stats.paceSampleCount, 2);

  stats.avgSecondsPerForwardPage = 100;
  stats.paceSampleCount = BookReadingStats::MAX_PACE_SAMPLE_COUNT;
  stats.recordForwardPageRead(200);
  EXPECT_EQ(stats.avgSecondsPerForwardPage, 101);
  EXPECT_EQ(stats.paceSampleCount, BookReadingStats::MAX_PACE_SAMPLE_COUNT);
  stats.recordForwardPageRead(50);
  EXPECT_EQ(stats.avgSecondsPerForwardPage, 100);
  stats.avgSecondsPerForwardPage = std::numeric_limits<uint16_t>::max();
  stats.recordForwardPageRead(1);
  EXPECT_LT(stats.avgSecondsPerForwardPage, std::numeric_limits<uint16_t>::max());
  stats.recordForwardPageRead(0);
  EXPECT_EQ(stats.paceSampleCount, BookReadingStats::MAX_PACE_SAMPLE_COUNT);
}

TEST(ReadingStatsPersistence, BookPrefersCommittedFilesThenRecoversTemp) {
  Storage.reset();
  constexpr char PRIMARY[] = "/book/stats_v6.bin";
  constexpr char BACKUP[] = "/book/stats_v6.bin.bak";
  constexpr char TEMP[] = "/book/stats_v6.bin.tmp";
  BookReadingStats primary;
  primary.totalReadingSeconds = 10;
  BookReadingStats backup;
  backup.totalReadingSeconds = 20;
  BookReadingStats temporary;
  temporary.totalReadingSeconds = 30;
  Storage.setFile(PRIMARY, bookEnvelope(primary));
  Storage.setFile(BACKUP, bookEnvelope(backup));
  Storage.setFile(TEMP, bookEnvelope(temporary));

  BookReadingStats::LoadStatus status = BookReadingStats::LoadStatus::Invalid;
  EXPECT_EQ(BookReadingStats::load("/book", &status).totalReadingSeconds, 10u);
  EXPECT_EQ(status, BookReadingStats::LoadStatus::Ok);
  ASSERT_TRUE(Storage.remove(PRIMARY));
  EXPECT_EQ(BookReadingStats::load("/book", &status).totalReadingSeconds, 20u);
  EXPECT_EQ(status, BookReadingStats::LoadStatus::RecoveredBackup);
  ASSERT_TRUE(Storage.remove(BACKUP));
  EXPECT_EQ(BookReadingStats::load("/book", &status).totalReadingSeconds, 30u);
  EXPECT_EQ(status, BookReadingStats::LoadStatus::RecoveredTemp);
}

TEST(ReadingStatsPersistence, EmptyBookFilesAreTornWritesAndDoNotBlockSafeRecovery) {
  constexpr char PRIMARY[] = "/book/stats_v6.bin";
  constexpr char BACKUP[] = "/book/stats_v6.bin.bak";
  constexpr char TEMP[] = "/book/stats_v6.bin.tmp";
  BookReadingStats::LoadStatus status = BookReadingStats::LoadStatus::Invalid;

  Storage.reset();
  Storage.setFile(PRIMARY, {});
  Storage.setFile(BACKUP, bookEnvelope(bookStatsWithSeconds(20)));
  EXPECT_EQ(BookReadingStats::load("/book", &status).totalReadingSeconds, 20u);
  EXPECT_EQ(status, BookReadingStats::LoadStatus::RecoveredBackup);

  Storage.reset();
  Storage.setFile(BACKUP, {});
  Storage.setFile(TEMP, bookEnvelope(bookStatsWithSeconds(30)));
  EXPECT_EQ(BookReadingStats::load("/book", &status).totalReadingSeconds, 30u);
  EXPECT_EQ(status, BookReadingStats::LoadStatus::RecoveredTemp);

  const std::vector<uint8_t> raw = asVector(ReadingStatsCodec::encode(bookStatsWithSeconds(40)));
  Storage.reset();
  Storage.setFile("/book/stats_v5.bin", {});
  Storage.setFile("/book/stats_v5.bin.bak", raw);
  EXPECT_EQ(BookReadingStats::load("/book", &status).totalReadingSeconds, 40u);
  EXPECT_EQ(status, BookReadingStats::LoadStatus::LoadedLegacy);
  EXPECT_TRUE(Storage.file("/book/stats_v5.bin").empty());
  EXPECT_EQ(Storage.file("/book/stats_v5.bin.bak"), raw);
}

TEST(ReadingStatsPersistence, BookDoesNotFallBackPastAnUnreadableCommittedCopy) {
  Storage.reset();
  constexpr char PRIMARY[] = "/book/stats_v6.bin";
  constexpr char BACKUP[] = "/book/stats_v6.bin.bak";
  const auto primary = bookEnvelope(bookStatsWithSeconds(10));
  const auto backup = bookEnvelope(bookStatsWithSeconds(20));
  Storage.setFile(PRIMARY, primary);
  Storage.setFile(BACKUP, backup);
  Storage.makeUnreadable(PRIMARY);

  BookReadingStats::LoadStatus status = BookReadingStats::LoadStatus::Ok;
  EXPECT_EQ(BookReadingStats::load("/book", &status).totalReadingSeconds, 0u);
  EXPECT_EQ(status, BookReadingStats::LoadStatus::IoError);
  EXPECT_FALSE(BookReadingStats::isTrustedLoadStatus(status));
  EXPECT_EQ(Storage.file(PRIMARY), primary);
  EXPECT_EQ(Storage.file(BACKUP), backup);
}

TEST(ReadingStatsPersistence, BookProtectsNewerWrongKindOversizedAndUnreadableEnvelopeSiblings) {
  constexpr char TEMP[] = "/book/stats_v6.bin.tmp";

  Storage.reset();
  std::vector<uint8_t> newer = bookEnvelope(BookReadingStats{});
  newer[4] = ReadingStatsEnvelope::CURRENT_VERSION + 1;
  Storage.setFile(TEMP, newer);
  EXPECT_FALSE(BookReadingStats{}.save("/book"));
  EXPECT_FALSE(BookReadingStats::remove("/book"));
  EXPECT_EQ(Storage.file(TEMP), newer);

  Storage.reset();
  const std::vector<uint8_t> wrongKind = globalEnvelope(GlobalReadingStats{});
  Storage.setFile(TEMP, wrongKind);
  EXPECT_FALSE(BookReadingStats{}.save("/book"));
  EXPECT_EQ(Storage.file(TEMP), wrongKind);

  Storage.reset();
  const std::vector<uint8_t> oversized(ReadingStatsEnvelope::MAX_FILE_SIZE + 1, 0xA5);
  Storage.setFile(TEMP, oversized);
  EXPECT_FALSE(BookReadingStats{}.save("/book"));
  EXPECT_EQ(Storage.file(TEMP), oversized);

  Storage.reset();
  const std::vector<uint8_t> valid = bookEnvelope(BookReadingStats{});
  Storage.setFile(TEMP, valid);
  Storage.makeUnreadable(TEMP);
  EXPECT_FALSE(BookReadingStats{}.save("/book"));
  EXPECT_EQ(Storage.file(TEMP), valid);
}

TEST(ReadingStatsPersistence, BookCorruptionRecoversEnvelopeBackupButNeverFallsBackToRawLegacy) {
  constexpr char PRIMARY[] = "/book/stats_v6.bin";
  constexpr char BACKUP[] = "/book/stats_v6.bin.bak";
  constexpr char LEGACY[] = "/book/stats_v5.bin";
  const std::vector<uint8_t> legacy = asVector(ReadingStatsCodec::encode(bookStatsWithSeconds(99)));
  std::vector<uint8_t> corrupt = bookEnvelope(bookStatsWithSeconds(10));
  corrupt[ReadingStatsEnvelope::HEADER_SIZE + 3] ^= 0x40;

  Storage.reset();
  Storage.setFile(PRIMARY, corrupt);
  Storage.setFile(LEGACY, legacy);
  BookReadingStats::LoadStatus status = BookReadingStats::LoadStatus::Ok;
  EXPECT_EQ(BookReadingStats::load("/book", &status).totalReadingSeconds, 0u);
  EXPECT_EQ(status, BookReadingStats::LoadStatus::Invalid);
  EXPECT_EQ(Storage.file(LEGACY), legacy);

  Storage.setFile(BACKUP, bookEnvelope(bookStatsWithSeconds(20)));
  EXPECT_EQ(BookReadingStats::load("/book", &status).totalReadingSeconds, 20u);
  EXPECT_EQ(status, BookReadingStats::LoadStatus::RecoveredBackup);
}

TEST(ReadingStatsPersistence, BookMigratesEverySupportedRawSourceOnceWithoutModifyingIt) {
  const std::vector<uint8_t> rawV5 = asVector(ReadingStatsCodec::encode(bookStatsWithSeconds(55)));
  std::vector<uint8_t> rawV4 = rawV5;
  rawV4.resize(69);
  rawV4[0] = 4;
  std::vector<uint8_t> rawV1(rawV5.begin(), rawV5.begin() + 11);
  rawV1[0] = 1;

  const std::array<std::pair<const char*, std::vector<uint8_t>>, 5> candidates = {
      std::pair{"/book/stats_v5.bin", rawV5}, std::pair{"/book/stats_v5.bin.bak", rawV5},
      std::pair{"/book/stats_v5.bin.tmp", rawV5}, std::pair{"/book/stats_v4.bin", rawV4},
      std::pair{"/book/stats.bin", rawV1}};
  for (const auto& [path, raw] : candidates) {
    Storage.reset();
    Storage.setFile(path, raw);
    BookReadingStats::LoadStatus status = BookReadingStats::LoadStatus::Invalid;
    EXPECT_EQ(BookReadingStats::load("/book", &status).totalReadingSeconds, 55u) << path;
    EXPECT_EQ(status, BookReadingStats::LoadStatus::LoadedLegacy) << path;
    EXPECT_EQ(Storage.file(path), raw) << path;
    ASSERT_TRUE(Storage.exists("/book/stats_v6.bin")) << path;
    const std::vector<uint8_t> canonical = Storage.file("/book/stats_v6.bin");

    EXPECT_EQ(BookReadingStats::load("/book", &status).totalReadingSeconds, 55u) << path;
    EXPECT_EQ(status, BookReadingStats::LoadStatus::Ok) << path;
    EXPECT_EQ(Storage.file("/book/stats_v6.bin"), canonical) << path;
    EXPECT_EQ(Storage.file(path), raw) << path;
  }
}

TEST(ReadingStatsPersistence, BookReplacesOnlyALoneInvalidTempDuringLegacyMigration) {
  const std::vector<uint8_t> raw = asVector(ReadingStatsCodec::encode(bookStatsWithSeconds(55)));
  std::vector<uint8_t> truncated = bookEnvelope(bookStatsWithSeconds(1));
  truncated.resize(ReadingStatsEnvelope::HEADER_SIZE + 1);
  const std::array<std::vector<uint8_t>, 3> tornTemps = {std::vector<uint8_t>{}, std::vector<uint8_t>{0xA5}, truncated};
  for (const std::vector<uint8_t>& torn : tornTemps) {
    Storage.reset();
    Storage.setFile("/book/stats_v5.bin", raw);
    Storage.setFile("/book/stats_v6.bin.tmp", torn);
    BookReadingStats::LoadStatus status = BookReadingStats::LoadStatus::Invalid;
    EXPECT_EQ(BookReadingStats::load("/book", &status).totalReadingSeconds, 55u);
    EXPECT_EQ(status, BookReadingStats::LoadStatus::LoadedLegacy);
    EXPECT_EQ(Storage.file("/book/stats_v5.bin"), raw);
    EXPECT_TRUE(Storage.exists("/book/stats_v6.bin"));
    EXPECT_FALSE(Storage.exists("/book/stats_v6.bin.tmp"));
  }

  Storage.reset();
  std::vector<uint8_t> newer = bookEnvelope(BookReadingStats{});
  newer[4] = ReadingStatsEnvelope::CURRENT_VERSION + 1;
  Storage.setFile("/book/stats_v5.bin", raw);
  Storage.setFile("/book/stats_v6.bin.tmp", newer);
  BookReadingStats::LoadStatus status = BookReadingStats::LoadStatus::Ok;
  EXPECT_EQ(BookReadingStats::load("/book", &status).totalReadingSeconds, 0u);
  EXPECT_EQ(status, BookReadingStats::LoadStatus::NewerFormat);
  EXPECT_EQ(Storage.file("/book/stats_v6.bin.tmp"), newer);
  EXPECT_EQ(Storage.file("/book/stats_v5.bin"), raw);
}

TEST(ReadingStatsPersistence, BookFutureSiblingShadowsV6AndIsNeverModified) {
  Storage.reset();
  ASSERT_TRUE(Storage.mkdir("/book"));
  const std::vector<uint8_t> current = bookEnvelope(bookStatsWithSeconds(10));
  const std::vector<uint8_t> future = {0xCA, 0xFE};
  Storage.setFile("/book/stats_v6.bin", current);
  Storage.setFile("/book/stats_v7.bin", future);

  BookReadingStats::LoadStatus status = BookReadingStats::LoadStatus::Ok;
  EXPECT_EQ(BookReadingStats::load("/book", &status).totalReadingSeconds, 0u);
  EXPECT_EQ(status, BookReadingStats::LoadStatus::NewerFormat);
  EXPECT_FALSE(bookStatsWithSeconds(20).save("/book"));
  EXPECT_FALSE(BookReadingStats::remove("/book"));
  EXPECT_EQ(Storage.file("/book/stats_v6.bin"), current);
  EXPECT_EQ(Storage.file("/book/stats_v7.bin"), future);
}

TEST(ReadingStatsPersistence, BookLegacyFallbackStopsAtNewerOrUnreadableButSkipsCorruption) {
  const std::vector<uint8_t> rawV5 = asVector(ReadingStatsCodec::encode(bookStatsWithSeconds(10)));
  std::vector<uint8_t> rawV4 = rawV5;
  rawV4.resize(69);
  rawV4[0] = 4;

  Storage.reset();
  Storage.setFile("/book/stats_v5.bin", {5});
  Storage.setFile("/book/stats_v4.bin", rawV4);
  EXPECT_EQ(BookReadingStats::load("/book").totalReadingSeconds, 10u);

  Storage.reset();
  std::vector<uint8_t> newer = rawV5;
  newer[0] = BookReadingStats::CURRENT_FILE_VERSION + 1;
  Storage.setFile("/book/stats_v5.bin", newer);
  Storage.setFile("/book/stats_v4.bin", rawV4);
  BookReadingStats::LoadStatus status = BookReadingStats::LoadStatus::Ok;
  EXPECT_EQ(BookReadingStats::load("/book", &status).totalReadingSeconds, 0u);
  EXPECT_EQ(status, BookReadingStats::LoadStatus::NewerFormat);

  Storage.reset();
  Storage.setFile("/book/stats_v5.bin", rawV5);
  Storage.setFile("/book/stats_v4.bin", rawV4);
  Storage.makeUnreadable("/book/stats_v5.bin");
  EXPECT_EQ(BookReadingStats::load("/book", &status).totalReadingSeconds, 0u);
  EXPECT_EQ(status, BookReadingStats::LoadStatus::IoError);
}

TEST(ReadingStatsPersistence, BookResetWritesTombstonePreservesLegacyAndNeverReimportsIt) {
  const std::vector<uint8_t> raw = asVector(ReadingStatsCodec::encode(bookStatsWithSeconds(77)));
  Storage.reset();
  Storage.setFile("/book/stats_v5.bin", raw);
  EXPECT_TRUE(BookReadingStats::remove("/book"));
  EXPECT_EQ(Storage.file("/book/stats_v5.bin"), raw);
  BookReadingStats::LoadStatus status = BookReadingStats::LoadStatus::Invalid;
  EXPECT_EQ(BookReadingStats::load("/book", &status).totalReadingSeconds, 0u);
  EXPECT_EQ(status, BookReadingStats::LoadStatus::Ok);
  ASSERT_TRUE(Storage.remove("/book/stats_v6.bin"));
  EXPECT_EQ(BookReadingStats::load("/book", &status).totalReadingSeconds, 0u);
  EXPECT_EQ(status, BookReadingStats::LoadStatus::RecoveredBackup);
  EXPECT_TRUE(BookReadingStats::remove("/book"));
  EXPECT_EQ(Storage.file("/book/stats_v5.bin"), raw);

  Storage.reset();
  Storage.setFile("/book/stats_v5.bin", {5});
  EXPECT_TRUE(BookReadingStats::remove("/book"));
  EXPECT_TRUE(Storage.exists("/book/stats_v6.bin"));

  Storage.reset();
  std::vector<uint8_t> newer = raw;
  newer[0] = BookReadingStats::CURRENT_FILE_VERSION + 1;
  Storage.setFile("/book/stats_v5.bin", newer);
  EXPECT_FALSE(BookReadingStats::remove("/book"));
  EXPECT_FALSE(Storage.exists("/book/stats_v6.bin"));
  Storage.makeUnreadable("/book/stats_v5.bin");
  EXPECT_FALSE(BookReadingStats::remove("/book"));

  Storage.reset();
  Storage.setFile("/book/stats_v6.bin", bookEnvelope(bookStatsWithSeconds(1)));
  Storage.setFile("/book/stats_v5.bin", newer);
  EXPECT_FALSE(BookReadingStats::remove("/book"));
  EXPECT_EQ(Storage.file("/book/stats_v5.bin"), newer);

  Storage.reset();
  Storage.setFile("/book/stats_v6.bin.tmp", {0xA5});
  Storage.setFile("/book/stats_v5.bin", newer);
  EXPECT_FALSE(BookReadingStats::remove("/book"));
  EXPECT_EQ(Storage.file("/book/stats_v6.bin.tmp"), (std::vector<uint8_t>{0xA5}));
  EXPECT_EQ(Storage.file("/book/stats_v5.bin"), newer);
}

TEST(ReadingStatsPersistence, BookResetPublishesBackupTombstoneBeforeReplacingPrimary) {
  const std::vector<uint8_t> raw = asVector(ReadingStatsCodec::encode(bookStatsWithSeconds(77)));
  for (size_t fault = 0; fault < 4; ++fault) {
    Storage.reset();
    Storage.setFile("/book/stats_v6.bin", bookEnvelope(bookStatsWithSeconds(77)));
    Storage.setFile("/book/stats_v6.bin.bak", bookEnvelope(bookStatsWithSeconds(76)));
    Storage.setFile("/book/stats_v5.bin", raw);
    if (fault == 0) Storage.shortWriteOnCall(2);
    if (fault == 1) Storage.failSyncOnCall(2);
    if (fault == 2) Storage.failRenameOnCall(2);
    if (fault == 3) Storage.corruptRenameOnCall(2);
    EXPECT_FALSE(BookReadingStats::remove("/book")) << fault;

    Storage.resetFaultInjection();
    if (Storage.exists("/book/stats_v6.bin")) {
      ASSERT_TRUE(Storage.remove("/book/stats_v6.bin"));
    }
    BookReadingStats::LoadStatus status = BookReadingStats::LoadStatus::Invalid;
    EXPECT_EQ(BookReadingStats::load("/book", &status).totalReadingSeconds, 0u) << fault;
    EXPECT_EQ(status, BookReadingStats::LoadStatus::RecoveredBackup) << fault;
    EXPECT_EQ(Storage.file("/book/stats_v5.bin"), raw) << fault;
  }

  for (size_t fault = 0; fault < 3; ++fault) {
    Storage.reset();
    Storage.setFile("/book/stats_v6.bin", bookEnvelope(bookStatsWithSeconds(77)));
    Storage.setFile("/book/stats_v6.bin.bak", bookEnvelope(bookStatsWithSeconds(76)));
    if (fault == 0) Storage.shortWriteOnCall(1);
    if (fault == 1) Storage.failSyncOnCall(1);
    if (fault == 2) Storage.failRenameOnCall(1);
    EXPECT_FALSE(BookReadingStats::remove("/book")) << fault;
    Storage.resetFaultInjection();
    EXPECT_EQ(BookReadingStats::load("/book").totalReadingSeconds, 77u) << fault;
  }
}

TEST(ReadingStatsPersistence, BookAtomicSaveRestoresTheVerifiedPreviousPrimaryOnFaults) {
  Storage.reset();
  ASSERT_TRUE(bookStatsWithSeconds(10).save("/book"));
  const std::vector<uint8_t> previous = Storage.file("/book/stats_v6.bin");

  Storage.shortWriteOnce();
  EXPECT_FALSE(bookStatsWithSeconds(20).save("/book"));
  EXPECT_EQ(Storage.file("/book/stats_v6.bin"), previous);

  Storage.resetFaultInjection();
  Storage.failSyncOnce();
  EXPECT_FALSE(bookStatsWithSeconds(20).save("/book"));
  EXPECT_EQ(Storage.file("/book/stats_v6.bin"), previous);

  Storage.resetFaultInjection();
  Storage.corruptRenameOnCall(2);
  EXPECT_FALSE(bookStatsWithSeconds(20).save("/book"));
  EXPECT_EQ(Storage.file("/book/stats_v6.bin"), previous);
}

TEST(ReadingStatsPersistence, GlobalReportsTrustedRecoverySource) {
  Storage.reset();
  constexpr char PRIMARY[] = "/.crosspoint/global_stats_v4.bin";
  constexpr char BACKUP[] = "/.crosspoint/global_stats_v4.bin.bak";
  constexpr char TEMP[] = "/.crosspoint/global_stats_v4.bin.tmp";
  Storage.setFile(PRIMARY, globalEnvelope(globalStatsWithSeconds(10)));
  Storage.setFile(BACKUP, globalEnvelope(globalStatsWithSeconds(20)));
  Storage.setFile(TEMP, globalEnvelope(globalStatsWithSeconds(30)));

  GlobalReadingStats::LoadStatus status = GlobalReadingStats::LoadStatus::Invalid;
  EXPECT_EQ(GlobalReadingStats::load(&status).totalReadingSeconds, 10u);
  EXPECT_EQ(status, GlobalReadingStats::LoadStatus::Ok);
  EXPECT_TRUE(GlobalReadingStats::isTrustedLoadStatus(status));

  ASSERT_TRUE(Storage.remove(PRIMARY));
  EXPECT_EQ(GlobalReadingStats::load(&status).totalReadingSeconds, 20u);
  EXPECT_EQ(status, GlobalReadingStats::LoadStatus::RecoveredBackup);

  ASSERT_TRUE(Storage.remove(BACKUP));
  EXPECT_EQ(GlobalReadingStats::load(&status).totalReadingSeconds, 30u);
  EXPECT_EQ(status, GlobalReadingStats::LoadStatus::RecoveredTemp);

  ASSERT_TRUE(Storage.remove(TEMP));
  EXPECT_EQ(GlobalReadingStats::load(&status).totalReadingSeconds, 0u);
  EXPECT_EQ(status, GlobalReadingStats::LoadStatus::Missing);
  EXPECT_TRUE(GlobalReadingStats::isTrustedLoadStatus(status));
}

TEST(ReadingStatsPersistence, EmptyGlobalFilesAreTornWritesAndDoNotBlockSafeRecovery) {
  constexpr char PRIMARY[] = "/.crosspoint/global_stats_v4.bin";
  constexpr char BACKUP[] = "/.crosspoint/global_stats_v4.bin.bak";
  constexpr char TEMP[] = "/.crosspoint/global_stats_v4.bin.tmp";
  GlobalReadingStats::LoadStatus status = GlobalReadingStats::LoadStatus::Invalid;

  Storage.reset();
  Storage.setFile(PRIMARY, {});
  Storage.setFile(BACKUP, globalEnvelope(globalStatsWithSeconds(20)));
  EXPECT_EQ(GlobalReadingStats::load(&status).totalReadingSeconds, 20u);
  EXPECT_EQ(status, GlobalReadingStats::LoadStatus::RecoveredBackup);

  Storage.reset();
  Storage.setFile(BACKUP, {});
  Storage.setFile(TEMP, globalEnvelope(globalStatsWithSeconds(30)));
  EXPECT_EQ(GlobalReadingStats::load(&status).totalReadingSeconds, 30u);
  EXPECT_EQ(status, GlobalReadingStats::LoadStatus::RecoveredTemp);

  const std::vector<uint8_t> raw = asVector(ReadingStatsCodec::encode(globalStatsWithSeconds(40)));
  Storage.reset();
  Storage.setFile("/.crosspoint/global_stats.bin", {});
  Storage.setFile("/.crosspoint/global_stats.bin.bak", raw);
  EXPECT_EQ(GlobalReadingStats::load(&status).totalReadingSeconds, 40u);
  EXPECT_EQ(status, GlobalReadingStats::LoadStatus::LoadedLegacy);
  EXPECT_TRUE(Storage.file("/.crosspoint/global_stats.bin").empty());
  EXPECT_EQ(Storage.file("/.crosspoint/global_stats.bin.bak"), raw);
}

TEST(ReadingStatsPersistence, GlobalRecoversPastCorruptCommittedFiles) {
  Storage.reset();
  constexpr char PRIMARY[] = "/.crosspoint/global_stats_v4.bin";
  constexpr char BACKUP[] = "/.crosspoint/global_stats_v4.bin.bak";
  constexpr char TEMP[] = "/.crosspoint/global_stats_v4.bin.tmp";
  Storage.setFile(PRIMARY, {ReadingStatsEnvelope::CURRENT_VERSION});
  Storage.setFile(BACKUP, globalEnvelope(globalStatsWithSeconds(20)));
  Storage.setFile(TEMP, globalEnvelope(globalStatsWithSeconds(30)));

  GlobalReadingStats::LoadStatus status = GlobalReadingStats::LoadStatus::Invalid;
  EXPECT_EQ(GlobalReadingStats::load(&status).totalReadingSeconds, 20u);
  EXPECT_EQ(status, GlobalReadingStats::LoadStatus::RecoveredBackup);

  Storage.setFile(BACKUP, {ReadingStatsEnvelope::CURRENT_VERSION});
  EXPECT_EQ(GlobalReadingStats::load(&status).totalReadingSeconds, 30u);
  EXPECT_EQ(status, GlobalReadingStats::LoadStatus::RecoveredTemp);
}

TEST(ReadingStatsPersistence, GlobalReportsUntrustedFilesAndPreservesProtectedTemp) {
  constexpr char PRIMARY[] = "/.crosspoint/global_stats_v4.bin";
  constexpr char TEMP[] = "/.crosspoint/global_stats_v4.bin.tmp";
  GlobalReadingStats::LoadStatus status = GlobalReadingStats::LoadStatus::Ok;

  Storage.reset();
  Storage.setFile(PRIMARY, {ReadingStatsEnvelope::CURRENT_VERSION});
  GlobalReadingStats::load(&status);
  EXPECT_EQ(status, GlobalReadingStats::LoadStatus::Invalid);
  EXPECT_FALSE(GlobalReadingStats::isTrustedLoadStatus(status));

  Storage.reset();
  std::vector<uint8_t> newerBytes = globalEnvelope(GlobalReadingStats{});
  newerBytes[4] = ReadingStatsEnvelope::CURRENT_VERSION + 1;
  Storage.setFile(TEMP, newerBytes);
  GlobalReadingStats::load(&status);
  EXPECT_EQ(status, GlobalReadingStats::LoadStatus::NewerFormat);
  EXPECT_FALSE(GlobalReadingStats{}.save());
  EXPECT_EQ(Storage.file(TEMP), newerBytes);

  Storage.reset();
  const std::vector<uint8_t> oversized(ReadingStatsEnvelope::MAX_FILE_SIZE + 1, 0xA5);
  Storage.setFile(TEMP, oversized);
  EXPECT_FALSE(GlobalReadingStats{}.save());
  EXPECT_EQ(Storage.file(TEMP), oversized);

  Storage.reset();
  const std::vector<uint8_t> valid = globalEnvelope(GlobalReadingStats{});
  Storage.setFile(TEMP, valid);
  Storage.makeUnreadable(TEMP);
  GlobalReadingStats::load(&status);
  EXPECT_EQ(status, GlobalReadingStats::LoadStatus::IoError);
  EXPECT_FALSE(GlobalReadingStats{}.save());
  EXPECT_EQ(Storage.file(TEMP), valid);

  Storage.reset();
  const auto primary = globalEnvelope(globalStatsWithSeconds(10));
  const auto backup = globalEnvelope(globalStatsWithSeconds(20));
  Storage.setFile(PRIMARY, primary);
  Storage.setFile("/.crosspoint/global_stats_v4.bin.bak", backup);
  Storage.makeUnreadable(PRIMARY);
  EXPECT_EQ(GlobalReadingStats::load(&status).totalReadingSeconds, 0u);
  EXPECT_EQ(status, GlobalReadingStats::LoadStatus::IoError);
  EXPECT_EQ(Storage.file(PRIMARY), primary);
}

TEST(ReadingStatsPersistence, GlobalMigratesRawPrimaryBackupAndTempOnceWithoutModifyingThem) {
  const std::vector<uint8_t> raw = asVector(ReadingStatsCodec::encode(globalStatsWithSeconds(66)));
  for (const char* path :
       {"/.crosspoint/global_stats.bin", "/.crosspoint/global_stats.bin.bak", "/.crosspoint/global_stats.bin.tmp"}) {
    Storage.reset();
    Storage.setFile(path, raw);
    GlobalReadingStats::LoadStatus status = GlobalReadingStats::LoadStatus::Invalid;
    EXPECT_EQ(GlobalReadingStats::load(&status).totalReadingSeconds, 66u) << path;
    EXPECT_EQ(status, GlobalReadingStats::LoadStatus::LoadedLegacy) << path;
    EXPECT_EQ(Storage.file(path), raw) << path;
    ASSERT_TRUE(Storage.exists("/.crosspoint/global_stats_v4.bin")) << path;
    const std::vector<uint8_t> canonical = Storage.file("/.crosspoint/global_stats_v4.bin");

    EXPECT_EQ(GlobalReadingStats::load(&status).totalReadingSeconds, 66u) << path;
    EXPECT_EQ(status, GlobalReadingStats::LoadStatus::Ok) << path;
    EXPECT_EQ(Storage.file("/.crosspoint/global_stats_v4.bin"), canonical) << path;
    EXPECT_EQ(Storage.file(path), raw) << path;
  }
}

TEST(ReadingStatsPersistence, GlobalReplacesOnlyALoneInvalidTempDuringLegacyMigration) {
  const std::vector<uint8_t> raw = asVector(ReadingStatsCodec::encode(globalStatsWithSeconds(66)));
  std::vector<uint8_t> truncated = globalEnvelope(globalStatsWithSeconds(1));
  truncated.resize(ReadingStatsEnvelope::HEADER_SIZE + 1);
  const std::array<std::vector<uint8_t>, 3> tornTemps = {std::vector<uint8_t>{}, std::vector<uint8_t>{0xA5}, truncated};
  for (const std::vector<uint8_t>& torn : tornTemps) {
    Storage.reset();
    Storage.setFile("/.crosspoint/global_stats.bin", raw);
    Storage.setFile("/.crosspoint/global_stats_v4.bin.tmp", torn);
    GlobalReadingStats::LoadStatus status = GlobalReadingStats::LoadStatus::Invalid;
    EXPECT_EQ(GlobalReadingStats::load(&status).totalReadingSeconds, 66u);
    EXPECT_EQ(status, GlobalReadingStats::LoadStatus::LoadedLegacy);
    EXPECT_EQ(Storage.file("/.crosspoint/global_stats.bin"), raw);
    EXPECT_TRUE(Storage.exists("/.crosspoint/global_stats_v4.bin"));
    EXPECT_FALSE(Storage.exists("/.crosspoint/global_stats_v4.bin.tmp"));
  }

  Storage.reset();
  std::vector<uint8_t> newer = globalEnvelope(GlobalReadingStats{});
  newer[4] = ReadingStatsEnvelope::CURRENT_VERSION + 1;
  Storage.setFile("/.crosspoint/global_stats.bin", raw);
  Storage.setFile("/.crosspoint/global_stats_v4.bin.tmp", newer);
  GlobalReadingStats::LoadStatus status = GlobalReadingStats::LoadStatus::Ok;
  EXPECT_EQ(GlobalReadingStats::load(&status).totalReadingSeconds, 0u);
  EXPECT_EQ(status, GlobalReadingStats::LoadStatus::NewerFormat);
  EXPECT_EQ(Storage.file("/.crosspoint/global_stats_v4.bin.tmp"), newer);
  EXPECT_EQ(Storage.file("/.crosspoint/global_stats.bin"), raw);
}

TEST(ReadingStatsPersistence, GlobalFutureSiblingShadowsV4AndIsNeverModified) {
  Storage.reset();
  ASSERT_TRUE(Storage.mkdir("/.crosspoint"));
  const std::vector<uint8_t> current = globalEnvelope(globalStatsWithSeconds(10));
  const std::vector<uint8_t> future = {0xCA, 0xFE};
  Storage.setFile("/.crosspoint/global_stats_v4.bin", current);
  Storage.setFile("/.crosspoint/global_stats_v5.bin", future);

  GlobalReadingStats::LoadStatus status = GlobalReadingStats::LoadStatus::Ok;
  EXPECT_EQ(GlobalReadingStats::load(&status).totalReadingSeconds, 0u);
  EXPECT_EQ(status, GlobalReadingStats::LoadStatus::NewerFormat);
  EXPECT_FALSE(globalStatsWithSeconds(20).save());
  EXPECT_FALSE(GlobalReadingStats::resetLocal());
  EXPECT_EQ(Storage.file("/.crosspoint/global_stats_v4.bin"), current);
  EXPECT_EQ(Storage.file("/.crosspoint/global_stats_v5.bin"), future);
}

TEST(ReadingStatsPersistence, GlobalNeverFallsBackToRawPastAnyEnvelopeArtifact) {
  constexpr char PRIMARY[] = "/.crosspoint/global_stats_v4.bin";
  constexpr char BACKUP[] = "/.crosspoint/global_stats_v4.bin.bak";
  const std::vector<uint8_t> raw = asVector(ReadingStatsCodec::encode(globalStatsWithSeconds(99)));
  std::vector<uint8_t> corrupt = globalEnvelope(globalStatsWithSeconds(10));
  corrupt[ReadingStatsEnvelope::HEADER_SIZE + 5] ^= 0x80;

  Storage.reset();
  Storage.setFile(PRIMARY, corrupt);
  Storage.setFile("/.crosspoint/global_stats.bin", raw);
  GlobalReadingStats::LoadStatus status = GlobalReadingStats::LoadStatus::Ok;
  EXPECT_EQ(GlobalReadingStats::load(&status).totalReadingSeconds, 0u);
  EXPECT_EQ(status, GlobalReadingStats::LoadStatus::Invalid);
  EXPECT_EQ(Storage.file("/.crosspoint/global_stats.bin"), raw);

  Storage.setFile(BACKUP, globalEnvelope(globalStatsWithSeconds(20)));
  EXPECT_EQ(GlobalReadingStats::load(&status).totalReadingSeconds, 20u);
  EXPECT_EQ(status, GlobalReadingStats::LoadStatus::RecoveredBackup);

  Storage.reset();
  std::vector<uint8_t> newer = globalEnvelope(globalStatsWithSeconds(10));
  newer[4] = ReadingStatsEnvelope::CURRENT_VERSION + 1;
  Storage.setFile(PRIMARY, newer);
  Storage.setFile("/.crosspoint/global_stats.bin", raw);
  EXPECT_EQ(GlobalReadingStats::load(&status).totalReadingSeconds, 0u);
  EXPECT_EQ(status, GlobalReadingStats::LoadStatus::NewerFormat);

  Storage.reset();
  Storage.setFile(PRIMARY, globalEnvelope(globalStatsWithSeconds(10)));
  Storage.setFile("/.crosspoint/global_stats.bin", raw);
  Storage.makeUnreadable(PRIMARY);
  EXPECT_EQ(GlobalReadingStats::load(&status).totalReadingSeconds, 0u);
  EXPECT_EQ(status, GlobalReadingStats::LoadStatus::IoError);
}

TEST(ReadingStatsPersistence, GlobalResetWritesTombstoneAndGuardsOnlyUnshadowedProtectedLegacy) {
  const std::vector<uint8_t> raw = asVector(ReadingStatsCodec::encode(globalStatsWithSeconds(88)));
  Storage.reset();
  Storage.setFile("/.crosspoint/global_stats.bin", raw);
  EXPECT_TRUE(GlobalReadingStats::resetLocal());
  EXPECT_EQ(Storage.file("/.crosspoint/global_stats.bin"), raw);
  GlobalReadingStats::LoadStatus status = GlobalReadingStats::LoadStatus::Invalid;
  EXPECT_EQ(GlobalReadingStats::load(&status).totalReadingSeconds, 0u);
  EXPECT_EQ(status, GlobalReadingStats::LoadStatus::Ok);
  ASSERT_TRUE(Storage.remove("/.crosspoint/global_stats_v4.bin"));
  EXPECT_EQ(GlobalReadingStats::load(&status).totalReadingSeconds, 0u);
  EXPECT_EQ(status, GlobalReadingStats::LoadStatus::RecoveredBackup);

  Storage.reset();
  Storage.setFile("/.crosspoint/global_stats.bin", {3});
  EXPECT_TRUE(GlobalReadingStats::resetLocal());
  EXPECT_TRUE(Storage.exists("/.crosspoint/global_stats_v4.bin"));

  Storage.reset();
  std::vector<uint8_t> newer = raw;
  newer[0] = GlobalReadingStats::CURRENT_FILE_VERSION + 1;
  Storage.setFile("/.crosspoint/global_stats.bin", newer);
  EXPECT_FALSE(GlobalReadingStats::resetLocal());
  EXPECT_FALSE(Storage.exists("/.crosspoint/global_stats_v4.bin"));

  Storage.reset();
  Storage.setFile("/.crosspoint/global_stats.bin", raw);
  Storage.makeUnreadable("/.crosspoint/global_stats.bin");
  EXPECT_FALSE(GlobalReadingStats::resetLocal());

  Storage.reset();
  Storage.setFile("/.crosspoint/global_stats_v4.bin", globalEnvelope(globalStatsWithSeconds(1)));
  Storage.setFile("/.crosspoint/global_stats.bin", newer);
  EXPECT_FALSE(GlobalReadingStats::resetLocal());
  EXPECT_EQ(Storage.file("/.crosspoint/global_stats.bin"), newer);

  Storage.reset();
  Storage.setFile("/.crosspoint/global_stats_v4.bin.tmp", {0xA5});
  Storage.setFile("/.crosspoint/global_stats.bin", newer);
  EXPECT_FALSE(GlobalReadingStats::resetLocal());
  EXPECT_EQ(Storage.file("/.crosspoint/global_stats_v4.bin.tmp"), (std::vector<uint8_t>{0xA5}));
  EXPECT_EQ(Storage.file("/.crosspoint/global_stats.bin"), newer);
}

TEST(ReadingStatsPersistence, GlobalResetPublishesBackupTombstoneBeforeReplacingPrimary) {
  const std::vector<uint8_t> raw = asVector(ReadingStatsCodec::encode(globalStatsWithSeconds(88)));
  for (size_t fault = 0; fault < 4; ++fault) {
    Storage.reset();
    Storage.setFile("/.crosspoint/global_stats_v4.bin", globalEnvelope(globalStatsWithSeconds(88)));
    Storage.setFile("/.crosspoint/global_stats_v4.bin.bak", globalEnvelope(globalStatsWithSeconds(87)));
    Storage.setFile("/.crosspoint/global_stats.bin", raw);
    if (fault == 0) Storage.shortWriteOnCall(2);
    if (fault == 1) Storage.failSyncOnCall(2);
    if (fault == 2) Storage.failRenameOnCall(2);
    if (fault == 3) Storage.corruptRenameOnCall(2);
    EXPECT_FALSE(GlobalReadingStats::resetLocal()) << fault;

    Storage.resetFaultInjection();
    if (Storage.exists("/.crosspoint/global_stats_v4.bin")) {
      ASSERT_TRUE(Storage.remove("/.crosspoint/global_stats_v4.bin"));
    }
    GlobalReadingStats::LoadStatus status = GlobalReadingStats::LoadStatus::Invalid;
    EXPECT_EQ(GlobalReadingStats::load(&status).totalReadingSeconds, 0u) << fault;
    EXPECT_EQ(status, GlobalReadingStats::LoadStatus::RecoveredBackup) << fault;
    EXPECT_EQ(Storage.file("/.crosspoint/global_stats.bin"), raw) << fault;
  }

  for (size_t fault = 0; fault < 3; ++fault) {
    Storage.reset();
    Storage.setFile("/.crosspoint/global_stats_v4.bin", globalEnvelope(globalStatsWithSeconds(88)));
    Storage.setFile("/.crosspoint/global_stats_v4.bin.bak", globalEnvelope(globalStatsWithSeconds(87)));
    if (fault == 0) Storage.shortWriteOnCall(1);
    if (fault == 1) Storage.failSyncOnCall(1);
    if (fault == 2) Storage.failRenameOnCall(1);
    EXPECT_FALSE(GlobalReadingStats::resetLocal()) << fault;
    Storage.resetFaultInjection();
    EXPECT_EQ(GlobalReadingStats::load().totalReadingSeconds, 88u) << fault;
  }
}

TEST(ReadingStatsPersistence, AggregationFallsBackToBackupsWithoutDoubleMerge) {
  Storage.reset();
  constexpr char DIRECTORY[] = "/.crosspoint/synced_stats";
  ASSERT_TRUE(Storage.mkdir(DIRECTORY));
  Storage.setFile(std::string(DIRECTORY) + "/device_aaaaaaaaaaaa.bin", {0});
  Storage.setFile(std::string(DIRECTORY) + "/device_aaaaaaaaaaaa.bin.bak",
                  asVector(ReadingStatsCodec::encode(globalStatsWithSeconds(10))));
  Storage.setFile(std::string(DIRECTORY) + "/device_bbbbbbbbbbbb.bin.bak",
                  asVector(ReadingStatsCodec::encode(globalStatsWithSeconds(20))));
  Storage.setFile(std::string(DIRECTORY) + "/device_cccccccccccc_v4.bin",
                  globalEnvelope(globalStatsWithSeconds(30), ReadingStatsEnvelope::Kind::PeerGlobal));
  Storage.setFile(std::string(DIRECTORY) + "/device_cccccccccccc_v4.bin.bak",
                  globalEnvelope(globalStatsWithSeconds(40), ReadingStatsEnvelope::Kind::PeerGlobal));
  Storage.setFile(std::string(DIRECTORY) + "/device_001122334455.bin",
                  asVector(ReadingStatsCodec::encode(globalStatsWithSeconds(100))));

  const GlobalReadingStatsAggregation report = GlobalReadingStats::loadAggregatedWithReport(globalStatsWithSeconds(1));
  EXPECT_EQ(report.stats.totalReadingSeconds, 61u);
  EXPECT_EQ(report.validPeerCount, 3u);
  EXPECT_EQ(report.skippedPeerCount, 0u);
  EXPECT_TRUE(report.scanComplete);
}

TEST(ReadingStatsPersistence, EmptyOrInvalidSyncedDirectoriesDoNotPretendToContainPeers) {
  Storage.reset();
  constexpr char DIRECTORY[] = "/.crosspoint/synced_stats";
  EXPECT_FALSE(GlobalReadingStats::hasSyncedStats());
  ASSERT_TRUE(Storage.mkdir(DIRECTORY));
  EXPECT_TRUE(GlobalReadingStats::hasSyncedStats());

  GlobalReadingStatsAggregation report = GlobalReadingStats::loadAggregatedWithReport(globalStatsWithSeconds(7));
  EXPECT_EQ(report.stats.totalReadingSeconds, 7u);
  EXPECT_EQ(report.validPeerCount, 0u);
  EXPECT_EQ(report.skippedPeerCount, 0u);
  EXPECT_TRUE(report.scanComplete);

  Storage.setFile(std::string(DIRECTORY) + "/device_aaaaaaaaaaaa.bin", {0});
  report = GlobalReadingStats::loadAggregatedWithReport(globalStatsWithSeconds(7));
  EXPECT_EQ(report.stats.totalReadingSeconds, 7u);
  EXPECT_EQ(report.validPeerCount, 0u);
  EXPECT_EQ(report.skippedPeerCount, 1u);
  EXPECT_TRUE(report.scanComplete);
}

TEST(ReadingStatsPersistence, ExistingInvalidSyncedPathRemainsVisibleAsIncompleteInsteadOfMissing) {
  Storage.reset();
  constexpr char DIRECTORY[] = "/.crosspoint/synced_stats";
  Storage.setFile(DIRECTORY, {0xA5});
  EXPECT_TRUE(GlobalReadingStats::hasSyncedStats());
  const GlobalReadingStatsAggregation report = GlobalReadingStats::loadAggregatedWithReport(globalStatsWithSeconds(7));
  EXPECT_EQ(report.stats.totalReadingSeconds, 7u);
  EXPECT_EQ(report.validPeerCount, 0u);
  EXPECT_FALSE(report.scanComplete);
}

TEST(ReadingStatsPersistence, NewerCanonicalSnapshotIsNeverReplacedByAnOlderBackup) {
  Storage.reset();
  constexpr char DIRECTORY[] = "/.crosspoint/synced_stats";
  const std::string canonicalPath = std::string(DIRECTORY) + "/device_aaaaaaaaaaaa_v4.bin";
  const std::string backupPath = canonicalPath + ".bak";
  ASSERT_TRUE(Storage.mkdir(DIRECTORY));
  std::vector<uint8_t> newer = globalEnvelope(globalStatsWithSeconds(100), ReadingStatsEnvelope::Kind::PeerGlobal);
  newer[4] = ReadingStatsEnvelope::CURRENT_VERSION + 1;
  const std::vector<uint8_t> backup =
      globalEnvelope(globalStatsWithSeconds(10), ReadingStatsEnvelope::Kind::PeerGlobal);
  Storage.setFile(canonicalPath, newer);
  Storage.setFile(backupPath, backup);

  const GlobalReadingStatsAggregation report = GlobalReadingStats::loadAggregatedWithReport(globalStatsWithSeconds(1));
  EXPECT_EQ(report.stats.totalReadingSeconds, 1u);
  EXPECT_EQ(report.validPeerCount, 0u);
  EXPECT_EQ(report.skippedPeerCount, 1u);
  EXPECT_TRUE(report.scanComplete);
  EXPECT_EQ(Storage.file(canonicalPath), newer);
  EXPECT_EQ(Storage.file(backupPath), backup);
}

TEST(ReadingStatsPersistence, FuturePeerSiblingMakesAggregateExplicitlyIncomplete) {
  Storage.reset();
  constexpr char DIRECTORY[] = "/.crosspoint/synced_stats";
  ASSERT_TRUE(Storage.mkdir(DIRECTORY));
  Storage.setFile(std::string(DIRECTORY) + "/device_aaaaaaaaaaaa_v4.bin",
                  globalEnvelope(globalStatsWithSeconds(20), ReadingStatsEnvelope::Kind::PeerGlobal));
  const std::vector<uint8_t> future = {0xCA, 0xFE};
  Storage.setFile(std::string(DIRECTORY) + "/device_aaaaaaaaaaaa_v5.bin", future);

  const GlobalReadingStatsAggregation report = GlobalReadingStats::loadAggregatedWithReport(globalStatsWithSeconds(1));
  EXPECT_EQ(report.stats.totalReadingSeconds, 1u);
  EXPECT_EQ(report.validPeerCount, 0u);
  EXPECT_EQ(report.skippedPeerCount, 1u);
  EXPECT_FALSE(report.scanComplete);
  EXPECT_EQ(Storage.file(std::string(DIRECTORY) + "/device_aaaaaaaaaaaa_v5.bin"), future);
}

TEST(ReadingStatsPersistence, MissingLocalMacFailsClosedWithoutDoubleCountingSelf) {
  Storage.reset();
  constexpr char DIRECTORY[] = "/.crosspoint/synced_stats";
  ASSERT_TRUE(Storage.mkdir(DIRECTORY));
  Storage.setFile(std::string(DIRECTORY) + "/device_001122334455.bin",
                  asVector(ReadingStatsCodec::encode(globalStatsWithSeconds(20))));
  setEspMacFailureForTest(true);
  const GlobalReadingStatsAggregation report = GlobalReadingStats::loadAggregatedWithReport(globalStatsWithSeconds(1));
  setEspMacFailureForTest(false);
  EXPECT_EQ(report.stats.totalReadingSeconds, 1u);
  EXPECT_EQ(report.validPeerCount, 0u);
  EXPECT_FALSE(report.scanComplete);
}

TEST(ReadingStatsPersistence, LoneInvalidPeerTempDoesNotShadowRetainedRawSnapshot) {
  Storage.reset();
  constexpr char DIRECTORY[] = "/.crosspoint/synced_stats";
  ASSERT_TRUE(Storage.mkdir(DIRECTORY));
  Storage.setFile(std::string(DIRECTORY) + "/device_aaaaaaaaaaaa_v4.bin.tmp", {});
  Storage.setFile(std::string(DIRECTORY) + "/device_aaaaaaaaaaaa.bin",
                  asVector(ReadingStatsCodec::encode(globalStatsWithSeconds(20))));

  const GlobalReadingStatsAggregation report = GlobalReadingStats::loadAggregatedWithReport(globalStatsWithSeconds(1));
  EXPECT_EQ(report.stats.totalReadingSeconds, 21u);
  EXPECT_EQ(report.validPeerCount, 1u);
  EXPECT_EQ(report.skippedPeerCount, 0u);
  EXPECT_TRUE(report.scanComplete);
  EXPECT_TRUE(Storage.file(std::string(DIRECTORY) + "/device_aaaaaaaaaaaa_v4.bin.tmp").empty());
}

TEST(ReadingStatsPersistence, AggregationReportsDirectoryIterationFailureInsteadOfReturningACompleteTotal) {
  Storage.reset();
  constexpr char DIRECTORY[] = "/.crosspoint/synced_stats";
  ASSERT_TRUE(Storage.mkdir(DIRECTORY));
  Storage.setFile(std::string(DIRECTORY) + "/device_aaaaaaaaaaaa.bin",
                  asVector(ReadingStatsCodec::encode(globalStatsWithSeconds(10))));
  Storage.setFile(std::string(DIRECTORY) + "/device_bbbbbbbbbbbb.bin",
                  asVector(ReadingStatsCodec::encode(globalStatsWithSeconds(20))));
  Storage.failDirectoryIterationAfter(1);

  const GlobalReadingStatsAggregation report = GlobalReadingStats::loadAggregatedWithReport(globalStatsWithSeconds(1));
  EXPECT_EQ(report.stats.totalReadingSeconds, 11u);
  EXPECT_EQ(report.validPeerCount, 1u);
  EXPECT_EQ(report.skippedPeerCount, 0u);
  EXPECT_FALSE(report.scanComplete);
}

TEST(ReadingStatsPersistence, AggregationReportsUnreadableEntryNameInsteadOfSilentlySkippingIt) {
  Storage.reset();
  constexpr char DIRECTORY[] = "/.crosspoint/synced_stats";
  const std::string unreadableName = std::string(DIRECTORY) + "/device_aaaaaaaaaaaa.bin";
  ASSERT_TRUE(Storage.mkdir(DIRECTORY));
  Storage.setFile(unreadableName, asVector(ReadingStatsCodec::encode(globalStatsWithSeconds(10))));
  Storage.failGetNameFor(unreadableName);

  const GlobalReadingStatsAggregation report = GlobalReadingStats::loadAggregatedWithReport(globalStatsWithSeconds(1));
  EXPECT_EQ(report.stats.totalReadingSeconds, 1u);
  EXPECT_EQ(report.validPeerCount, 0u);
  EXPECT_EQ(report.skippedPeerCount, 0u);
  EXPECT_FALSE(report.scanComplete);
}

TEST(ReadingStatsPersistence, AggregationReportsEntryAndDirectoryCloseFailures) {
  constexpr char DIRECTORY[] = "/.crosspoint/synced_stats";
  const std::string peerPath = std::string(DIRECTORY) + "/device_aaaaaaaaaaaa.bin";
  for (const std::string& failedPath : {peerPath, std::string(DIRECTORY)}) {
    Storage.reset();
    ASSERT_TRUE(Storage.mkdir(DIRECTORY));
    Storage.setFile(peerPath, asVector(ReadingStatsCodec::encode(globalStatsWithSeconds(10))));
    Storage.failCloseFor(failedPath);
    const GlobalReadingStatsAggregation report =
        GlobalReadingStats::loadAggregatedWithReport(globalStatsWithSeconds(1));
    EXPECT_EQ(report.stats.totalReadingSeconds, 11u);
    EXPECT_EQ(report.validPeerCount, 1u);
    EXPECT_FALSE(report.scanComplete);
  }
}

TEST(NearbyStatsStorage, RawWireIsWrappedAtRestAndLegacySourceRemainsUntouched) {
  Storage.reset();
  constexpr char CANONICAL[] = "/.crosspoint/synced_stats/device_aaaaaaaaaaaa_v4.bin";
  constexpr char LEGACY[] = "/.crosspoint/synced_stats/device_aaaaaaaaaaaa.bin";
  const auto wire = ReadingStatsCodec::encode(globalStatsWithSeconds(20));
  const std::vector<uint8_t> legacy = asVector(ReadingStatsCodec::encode(globalStatsWithSeconds(10)));
  Storage.setFile(LEGACY, legacy);

  NearbyStatsStorage::Inspection inspection;
  ASSERT_TRUE(NearbyStatsStorage::saveRawSnapshot(CANONICAL, LEGACY, wire.data(), wire.size(), &inspection));
  EXPECT_FALSE(inspection.protectedStorage);
  EXPECT_FALSE(inspection.regresses);
  EXPECT_EQ(Storage.file(LEGACY), legacy);
  EXPECT_EQ(asVector(wire), asVector(ReadingStatsCodec::encode(globalStatsWithSeconds(20))));

  ReadingStatsCodec::GlobalBytes payload{};
  const ReadingStatsEnvelope::ReadOutcome stored =
      ReadingStatsEnvelope::read(CANONICAL, ReadingStatsEnvelope::Kind::PeerGlobal, payload.data(), payload.size());
  ASSERT_EQ(stored.readResult, ReadingStatsStorage::ReadResult::Ok);
  ASSERT_EQ(stored.decodeResult, ReadingStatsEnvelope::DecodeResult::Ok);
  GlobalReadingStats decoded;
  ASSERT_EQ(ReadingStatsCodec::decode(payload.data(), stored.payloadSize, decoded), ReadingStatsDecodeResult::Ok);
  EXPECT_EQ(decoded.totalReadingSeconds, 20u);

  const std::vector<uint8_t> firstCanonical = Storage.file(CANONICAL);
  EXPECT_TRUE(NearbyStatsStorage::saveRawSnapshot(CANONICAL, LEGACY, wire.data(), wire.size()));
  EXPECT_EQ(Storage.file(CANONICAL), firstCanonical);
  EXPECT_EQ(Storage.file(std::string(CANONICAL) + ".bak"), firstCanonical);
  EXPECT_EQ(Storage.file(LEGACY), legacy);
}

TEST(NearbyStatsStorage, BlocksRegressionNewerWrongKindAndIoWithoutTouchingFiles) {
  constexpr char CANONICAL[] = "/.crosspoint/synced_stats/device_aaaaaaaaaaaa_v4.bin";
  constexpr char LEGACY[] = "/.crosspoint/synced_stats/device_aaaaaaaaaaaa.bin";
  const auto incoming = ReadingStatsCodec::encode(globalStatsWithSeconds(20));
  NearbyStatsStorage::Inspection inspection;

  Storage.reset();
  const std::vector<uint8_t> largerLegacy = asVector(ReadingStatsCodec::encode(globalStatsWithSeconds(30)));
  Storage.setFile(LEGACY, largerLegacy);
  EXPECT_FALSE(NearbyStatsStorage::saveRawSnapshot(CANONICAL, LEGACY, incoming.data(), incoming.size(), &inspection));
  EXPECT_TRUE(inspection.regresses);
  EXPECT_EQ(Storage.file(LEGACY), largerLegacy);

  Storage.reset();
  std::vector<uint8_t> newer = globalEnvelope(globalStatsWithSeconds(10), ReadingStatsEnvelope::Kind::PeerGlobal);
  newer[4] = ReadingStatsEnvelope::CURRENT_VERSION + 1;
  Storage.setFile(CANONICAL, newer);
  EXPECT_FALSE(NearbyStatsStorage::saveRawSnapshot(CANONICAL, LEGACY, incoming.data(), incoming.size(), &inspection));
  EXPECT_TRUE(inspection.protectedStorage);
  EXPECT_EQ(Storage.file(CANONICAL), newer);

  Storage.reset();
  const std::vector<uint8_t> wrongKind = globalEnvelope(globalStatsWithSeconds(10));
  Storage.setFile(CANONICAL, wrongKind);
  EXPECT_FALSE(NearbyStatsStorage::saveRawSnapshot(CANONICAL, LEGACY, incoming.data(), incoming.size(), &inspection));
  EXPECT_TRUE(inspection.protectedStorage);
  EXPECT_EQ(Storage.file(CANONICAL), wrongKind);

  Storage.reset();
  Storage.setFile(LEGACY, asVector(ReadingStatsCodec::encode(globalStatsWithSeconds(10))));
  Storage.makeUnreadable(LEGACY);
  EXPECT_FALSE(NearbyStatsStorage::saveRawSnapshot(CANONICAL, LEGACY, incoming.data(), incoming.size(), &inspection));
  EXPECT_TRUE(inspection.protectedStorage);
}

TEST(NearbyStatsStorage, MonotonicIncomingRepairsCorruptPrimaryAgainstValidBackup) {
  Storage.reset();
  constexpr char CANONICAL[] = "/.crosspoint/synced_stats/device_aaaaaaaaaaaa_v4.bin";
  constexpr char LEGACY[] = "/.crosspoint/synced_stats/device_aaaaaaaaaaaa.bin";
  std::vector<uint8_t> corrupt = globalEnvelope(globalStatsWithSeconds(10), ReadingStatsEnvelope::Kind::PeerGlobal);
  corrupt[ReadingStatsEnvelope::HEADER_SIZE + 7] ^= 0x20;
  const std::vector<uint8_t> backup =
      globalEnvelope(globalStatsWithSeconds(20), ReadingStatsEnvelope::Kind::PeerGlobal);
  Storage.setFile(CANONICAL, corrupt);
  Storage.setFile(std::string(CANONICAL) + ".bak", backup);
  const auto incoming = ReadingStatsCodec::encode(globalStatsWithSeconds(30));

  ASSERT_TRUE(NearbyStatsStorage::saveRawSnapshot(CANONICAL, LEGACY, incoming.data(), incoming.size()));
  EXPECT_EQ(Storage.file(std::string(CANONICAL) + ".bak"), backup);
  ReadingStatsCodec::GlobalBytes payload{};
  const ReadingStatsEnvelope::ReadOutcome stored =
      ReadingStatsEnvelope::read(CANONICAL, ReadingStatsEnvelope::Kind::PeerGlobal, payload.data(), payload.size());
  ASSERT_EQ(stored.decodeResult, ReadingStatsEnvelope::DecodeResult::Ok);
  GlobalReadingStats decoded;
  ASSERT_EQ(ReadingStatsCodec::decode(payload.data(), stored.payloadSize, decoded), ReadingStatsDecodeResult::Ok);
  EXPECT_EQ(decoded.totalReadingSeconds, 30u);
}

TEST(NearbyStatsStorage, EmptyAndTempOnlyTornFilesRemainSafelyReplaceable) {
  constexpr char CANONICAL[] = "/.crosspoint/synced_stats/device_aaaaaaaaaaaa_v4.bin";
  constexpr char LEGACY[] = "/.crosspoint/synced_stats/device_aaaaaaaaaaaa.bin";
  const auto incoming = ReadingStatsCodec::encode(globalStatsWithSeconds(30));

  Storage.reset();
  Storage.setFile(CANONICAL, {});
  const std::vector<uint8_t> canonicalBackup =
      globalEnvelope(globalStatsWithSeconds(20), ReadingStatsEnvelope::Kind::PeerGlobal);
  Storage.setFile(std::string(CANONICAL) + ".bak", canonicalBackup);
  EXPECT_TRUE(NearbyStatsStorage::saveRawSnapshot(CANONICAL, LEGACY, incoming.data(), incoming.size()));
  EXPECT_EQ(Storage.file(std::string(CANONICAL) + ".bak"), canonicalBackup);

  Storage.reset();
  Storage.setFile(LEGACY, {});
  const std::vector<uint8_t> rawBackup = asVector(ReadingStatsCodec::encode(globalStatsWithSeconds(20)));
  Storage.setFile(std::string(LEGACY) + ".bak", rawBackup);
  EXPECT_TRUE(NearbyStatsStorage::saveRawSnapshot(CANONICAL, LEGACY, incoming.data(), incoming.size()));
  EXPECT_EQ(Storage.file(std::string(LEGACY) + ".bak"), rawBackup);

  Storage.reset();
  Storage.setFile(std::string(CANONICAL) + ".tmp", {0xA5});
  Storage.setFile(LEGACY, rawBackup);
  EXPECT_TRUE(NearbyStatsStorage::saveRawSnapshot(CANONICAL, LEGACY, incoming.data(), incoming.size()));
  EXPECT_EQ(Storage.file(LEGACY), rawBackup);
  const std::string tempPath = std::string(CANONICAL) + ".tmp";
  EXPECT_FALSE(Storage.exists(tempPath.c_str()));
}

TEST(NearbyStatsStorage, MatchingFutureSiblingBlocksReceiveWithoutModification) {
  Storage.reset();
  constexpr char DIRECTORY[] = "/.crosspoint/synced_stats";
  constexpr char CANONICAL[] = "/.crosspoint/synced_stats/device_aaaaaaaaaaaa_v4.bin";
  constexpr char LEGACY[] = "/.crosspoint/synced_stats/device_aaaaaaaaaaaa.bin";
  ASSERT_TRUE(Storage.mkdir(DIRECTORY));
  const std::vector<uint8_t> future = {0xCA, 0xFE};
  Storage.setFile(std::string(DIRECTORY) + "/device_aaaaaaaaaaaa_v5.bin", future);
  const auto incoming = ReadingStatsCodec::encode(globalStatsWithSeconds(30));
  NearbyStatsStorage::Inspection inspection;
  EXPECT_FALSE(NearbyStatsStorage::saveRawSnapshot(CANONICAL, LEGACY, incoming.data(), incoming.size(), &inspection));
  EXPECT_TRUE(inspection.protectedStorage);
  EXPECT_FALSE(Storage.exists(CANONICAL));
  EXPECT_EQ(Storage.file(std::string(DIRECTORY) + "/device_aaaaaaaaaaaa_v5.bin"), future);
}

TEST(ReadingStatsPersistence, PeerEnvelopeShadowsLegacyAndInvalidEnvelopeNeverFallsBackToRaw) {
  Storage.reset();
  constexpr char DIRECTORY[] = "/.crosspoint/synced_stats";
  constexpr char CANONICAL[] = "/.crosspoint/synced_stats/device_aaaaaaaaaaaa_v4.bin";
  constexpr char LEGACY[] = "/.crosspoint/synced_stats/device_aaaaaaaaaaaa.bin";
  ASSERT_TRUE(Storage.mkdir(DIRECTORY));
  Storage.setFile(LEGACY, asVector(ReadingStatsCodec::encode(globalStatsWithSeconds(100))));
  Storage.setFile(CANONICAL, globalEnvelope(globalStatsWithSeconds(20), ReadingStatsEnvelope::Kind::PeerGlobal));
  GlobalReadingStatsAggregation report = GlobalReadingStats::loadAggregatedWithReport(globalStatsWithSeconds(1));
  EXPECT_EQ(report.stats.totalReadingSeconds, 21u);
  EXPECT_EQ(report.validPeerCount, 1u);

  std::vector<uint8_t> corrupt = Storage.file(CANONICAL);
  corrupt.back() ^= 0x80;
  Storage.setFile(CANONICAL, corrupt);
  report = GlobalReadingStats::loadAggregatedWithReport(globalStatsWithSeconds(1));
  EXPECT_EQ(report.stats.totalReadingSeconds, 1u);
  EXPECT_EQ(report.validPeerCount, 0u);
  EXPECT_EQ(report.skippedPeerCount, 1u);
}

TEST(ReadingStatsPersistence, AggregationStopsFailClosedAtFixedPeerCapacity) {
  Storage.reset();
  constexpr char DIRECTORY[] = "/.crosspoint/synced_stats";
  ASSERT_TRUE(Storage.mkdir(DIRECTORY));
  for (size_t index = 1; index <= GlobalReadingStats::MAX_SYNCED_DEVICE_SNAPSHOTS + 1; ++index) {
    char path[96];
    snprintf(path, sizeof(path), "%s/device_%012llx.bin", DIRECTORY, static_cast<unsigned long long>(index));
    Storage.setFile(path, asVector(ReadingStatsCodec::encode(globalStatsWithSeconds(1))));
  }

  const GlobalReadingStatsAggregation report = GlobalReadingStats::loadAggregatedWithReport(globalStatsWithSeconds(1));
  EXPECT_EQ(report.stats.totalReadingSeconds, 1u + GlobalReadingStats::MAX_SYNCED_DEVICE_SNAPSHOTS);
  EXPECT_EQ(report.validPeerCount, GlobalReadingStats::MAX_SYNCED_DEVICE_SNAPSHOTS);
  EXPECT_FALSE(report.scanComplete);
}

TEST(ReadingStatsDates, ValidateLeapDaysAndSplitAcrossBoundaries) {
  EXPECT_TRUE((ReadingStatsDate{2024, 2, 29}.isValid()));
  EXPECT_FALSE((ReadingStatsDate{2023, 2, 29}.isValid()));
  EXPECT_FALSE((ReadingStatsDateTime{{2024, 1, 1}, 24, 0, 0}).isValid());
  ReadingStatsDate date{2024, 2, 28};
  addDaysToReadingStatsDate(date, 1);
  EXPECT_EQ(date.day, 29);
  EXPECT_EQ(readingStatsDayOfWeekIndex({2024, 1, 1}), 0);
  const uint32_t minuteIndex = readingStatsMinuteIndex({2026, 7, 21}, 14u * 60u + 35u);
  ReadingStatsDateTime restored;
  ASSERT_TRUE(readingStatsDateTimeFromMinuteIndex(minuteIndex, restored));
  EXPECT_EQ(restored.date.year, 2026);
  EXPECT_EQ(restored.date.month, 7);
  EXPECT_EQ(restored.date.day, 21);
  EXPECT_EQ(restored.hour, 14);
  EXPECT_EQ(restored.minute, 35);

  std::array<uint32_t, READING_TIME_BUCKET_COUNT> buckets{};
  std::array<uint32_t, READING_DAY_OF_WEEK_COUNT> days{};
  recordReadingSpanIntoBuckets(buckets, days, {{2024, 1, 1}, 11, 59, 30}, 61);
  EXPECT_EQ(buckets[static_cast<size_t>(ReadingTimeBucket::Morning)], 30u);
  EXPECT_EQ(buckets[static_cast<size_t>(ReadingTimeBucket::Afternoon)], 31u);
  EXPECT_EQ(days[0], 61u);
}

TEST(ReadingStatsHistory, TracksCrossMidnightAndStreakExpiry) {
  GlobalReadingStats stats;
  stats.recordReadingSpan({{2024, 1, 1}, 23, 59, 50}, 20);
  const ReadingStatsDate januarySecond{2024, 1, 2};
  EXPECT_EQ(stats.readingHistoryAnchorDay, readingStatsDayIndex(januarySecond));
  EXPECT_EQ(stats.currentReadingStreak(&januarySecond), 2);
  EXPECT_EQ(stats.displayLongestReadingStreak(), 2);
  uint32_t januarySecondSeconds = 0;
  ASSERT_TRUE(stats.readingSecondsForDate(januarySecond, januarySecondSeconds));
  EXPECT_EQ(januarySecondSeconds, 10u);

  const ReadingStatsDate januaryFourth{2024, 1, 4};
  EXPECT_EQ(stats.currentReadingStreak(&januaryFourth), 0);
}

TEST(ReadingStatsDailySummary, PersistsExactCurrentDayWithoutChangingTheCompatibleGlobalPayload) {
  Storage.reset();
  const ReadingStatsDate today{2026, 7, 23};
  GlobalReadingStats stats;
  stats.totalSessions = 1;
  stats.totalReadingSeconds = 12u * 60u;
  stats.recordReadingSession(today);
  stats.recordReadingSpan({today, 12, 0, 0}, 12u * 60u);

  ASSERT_TRUE(stats.save());
  ASSERT_TRUE(Storage.exists(DAILY_STATS_PATH));
  GlobalReadingStats::LoadStatus status = GlobalReadingStats::LoadStatus::Invalid;
  const GlobalReadingStats loaded = GlobalReadingStats::load(&status);
  ASSERT_EQ(status, GlobalReadingStats::LoadStatus::Ok);
  uint32_t seconds = 0;
  uint32_t sessions = 0;
  ASSERT_TRUE(loaded.readingSummaryForDate(today, seconds, sessions));
  EXPECT_EQ(seconds, 12u * 60u);
  EXPECT_EQ(sessions, 1u);
  EXPECT_EQ(ReadingStatsCodec::encode(loaded), ReadingStatsCodec::encode(stats));
}

TEST(ReadingStatsDailySummary, CountsOnlySessionsExplicitlyRecordedForThatDay) {
  const ReadingStatsDate today{2026, 7, 23};
  GlobalReadingStats stats;
  stats.totalSessions = 2;
  stats.recordReadingSession(today);
  stats.recordReadingSpan({today, 9, 0, 0}, 90);
  stats.recordReadingSession(today);
  stats.recordReadingSpan({today, 12, 0, 0}, 30);

  uint32_t seconds = 0;
  uint32_t sessions = 0;
  ASSERT_TRUE(stats.readingSummaryForDate(today, seconds, sessions));
  EXPECT_EQ(seconds, 120u);
  EXPECT_EQ(sessions, 2u);
}

TEST(ReadingStatsDailySummary, NeverPresentsAStaleSidecarAsToday) {
  Storage.reset();
  const ReadingStatsDate today{2026, 7, 23};
  GlobalReadingStats oldStats;
  oldStats.totalReadingSeconds = 120;
  oldStats.recordReadingSpan({today, 10, 0, 0}, 120);
  ASSERT_TRUE(oldStats.save());

  GlobalReadingStats replacement;
  replacement.totalReadingSeconds = 600;
  replacement.recordReadingSpan({today, 11, 0, 0}, 600);
  Storage.setFile(GLOBAL_STATS_PATH, globalEnvelope(replacement));

  GlobalReadingStats::LoadStatus status = GlobalReadingStats::LoadStatus::Invalid;
  const GlobalReadingStats loaded = GlobalReadingStats::load(&status);
  ASSERT_EQ(status, GlobalReadingStats::LoadStatus::Ok);
  uint32_t seconds = 0;
  EXPECT_FALSE(loaded.readingSecondsForDate(today, seconds));
}

TEST(ReadingStatsDailySummary, SidecarFailureKeepsCumulativeStatsAndHidesUnknownToday) {
  Storage.reset();
  const ReadingStatsDate today{2026, 7, 23};
  GlobalReadingStats stats;
  stats.totalReadingSeconds = 90;
  stats.recordReadingSpan({today, 12, 0, 0}, 90);
  Storage.shortWriteOnCall(2);  // Main envelope succeeds; optional daily sidecar fails.

  ASSERT_TRUE(stats.save());
  EXPECT_FALSE(Storage.exists(DAILY_STATS_PATH));
  GlobalReadingStats::LoadStatus status = GlobalReadingStats::LoadStatus::Invalid;
  const GlobalReadingStats loaded = GlobalReadingStats::load(&status);
  ASSERT_EQ(status, GlobalReadingStats::LoadStatus::Ok);
  EXPECT_EQ(loaded.totalReadingSeconds, 90u);
  uint32_t seconds = 0;
  EXPECT_FALSE(loaded.readingSecondsForDate(today, seconds));
}

TEST(ReadingStatsDailySummary, ProvesZeroForAnUnrecordedDay) {
  GlobalReadingStats stats;
  stats.recordReadingSpan({{2026, 7, 22}, 12, 0, 0}, 60);
  uint32_t seconds = 99;
  uint32_t sessions = 99;
  ASSERT_TRUE(stats.readingSummaryForDate({2026, 7, 23}, seconds, sessions));
  EXPECT_EQ(seconds, 0u);
  EXPECT_EQ(sessions, 0u);
}

TEST(ReadingStatsDailySummary, DoesNotCallUnattributedLegacyReadingAnExactZero) {
  GlobalReadingStats legacy;
  legacy.totalReadingSeconds = 600;
  uint32_t seconds = 0;
  uint32_t sessions = 0;
  EXPECT_FALSE(legacy.readingSummaryForDate({2026, 7, 23}, seconds, sessions));

  GlobalReadingStats fresh;
  ASSERT_TRUE(fresh.readingSummaryForDate({2026, 7, 23}, seconds, sessions));
  EXPECT_EQ(seconds, 0u);
  EXPECT_EQ(sessions, 0u);
}

TEST(ReadingStatsDailySummary, DoesNotInventLegacySameDaySessionCounts) {
  const ReadingStatsDate today{2026, 7, 23};
  GlobalReadingStats legacy;
  recordReadingSpanIntoHistory(legacy.readingHistoryAnchorDay, legacy.readingHistoryBits, {today, 12, 0, 0}, 60);
  legacy.totalSessions = 7;
  legacy.recordReadingSession(today);

  uint32_t seconds = 0;
  uint32_t sessions = 0;
  EXPECT_FALSE(legacy.readingSummaryForDate(today, seconds, sessions));
}

TEST(ReadingStatsHistory, RejectsFutureDatedHistoryAsCurrent) {
  GlobalReadingStats stats;
  stats.recordReadingSpan({{2024, 1, 2}, 12, 0, 0}, 60);

  const ReadingStatsDate januaryFirst{2024, 1, 1};
  EXPECT_EQ(stats.currentReadingStreak(&januaryFirst), 0);

  const ReadingStatsDate januarySecond{2024, 1, 2};
  EXPECT_EQ(stats.currentReadingStreak(&januarySecond), 1);
}

TEST(ReadingStatsHistory, FutureClockCannotEraseOrHideKnownStreak) {
  GlobalReadingStats stats;
  stats.recordReadingSpan({{2024, 1, 1}, 12, 0, 0}, 60);
  stats.recordReadingSpan({{2024, 1, 2}, 12, 0, 0}, 60);

  const uint32_t validAnchor = stats.readingHistoryAnchorDay;
  const auto validBits = stats.readingHistoryBits;
  stats.recordReadingSpan({{2099, 1, 1}, 12, 0, 0}, 60);
  EXPECT_EQ(stats.readingHistoryAnchorDay, validAnchor);
  EXPECT_EQ(stats.readingHistoryBits, validBits);

  const ReadingStatsDate januaryThird{2024, 1, 3};
  stats.recordReadingSpan({januaryThird, 12, 0, 0}, 60);
  EXPECT_EQ(stats.currentReadingStreak(&januaryThird), 3);

  // A smaller clock jump remains representable in the rolling window. Ignore
  // its future bit when showing the streak for the actual current day.
  stats.recordReadingSpan({{2024, 1, 10}, 12, 0, 0}, 60);
  EXPECT_EQ(stats.currentReadingStreak(&januaryThird), 3);
}

TEST(ReadingStatsHistory, DistantPeerClockCannotReanchorLocalHistory) {
  GlobalReadingStats local;
  local.totalReadingSeconds = std::numeric_limits<uint32_t>::max() - 5;
  local.recordReadingSpan({{2024, 1, 1}, 23, 59, 59}, 2);
  const uint32_t validAnchor = local.readingHistoryAnchorDay;
  const auto validBits = local.readingHistoryBits;

  GlobalReadingStats peer;
  peer.totalReadingSeconds = 10;
  peer.recordReadingSpan({{2099, 1, 1}, 12, 0, 0}, 60);
  local.merge(peer);

  EXPECT_EQ(local.totalReadingSeconds, std::numeric_limits<uint32_t>::max());
  EXPECT_EQ(local.readingHistoryAnchorDay, validAnchor);
  EXPECT_EQ(local.readingHistoryBits, validBits);
  const ReadingStatsDate januarySecond{2024, 1, 2};
  EXPECT_EQ(local.currentReadingStreak(&januarySecond), 2);
}

TEST(ReadingSessionTracker, CountsOnlyVisiblePagesAndOnlyForwardDwellAsPageRead) {
  ReadingSessionTracker tracker;
  EXPECT_EQ(tracker.stop(5000, true).seconds, 0u);

  EXPECT_TRUE(tracker.pageVisible(1000));
  EXPECT_FALSE(tracker.pageVisible(1500));  // A no-op redraw must not reset the timer.
  auto backward = tracker.stop(4500, false);
  EXPECT_EQ(backward.seconds, 3u);
  EXPECT_FALSE(backward.forwardPageRead);

  tracker.pageVisible(5000);
  auto tooFast = tracker.stop(6999, true);
  EXPECT_EQ(tooFast.seconds, 1u);
  EXPECT_FALSE(tooFast.forwardPageRead);

  tracker.pageVisible(7000);
  auto forward = tracker.stop(9000, true);
  EXPECT_EQ(forward.seconds, 2u);
  EXPECT_TRUE(forward.forwardPageRead);
}

TEST(ReadingSessionTracker, RejectsIdleIntervalsAndHandlesMillisWrap) {
  ReadingSessionTracker tracker;
  tracker.pageVisible(1000);
  EXPECT_FALSE(tracker.discardIfIdle(1000 + ReadingSessionTracker::MAX_ACTIVE_INTERVAL_MS));
  EXPECT_TRUE(tracker.discardIfIdle(1001 + ReadingSessionTracker::MAX_ACTIVE_INTERVAL_MS));
  EXPECT_FALSE(tracker.isActive());
  EXPECT_EQ(tracker.stop(400000, true).seconds, 0u);

  tracker.pageVisible(std::numeric_limits<uint32_t>::max() - 1000u);
  const auto wrapped = tracker.stop(1500u, true);
  EXPECT_EQ(wrapped.seconds, 2u);
  EXPECT_TRUE(wrapped.forwardPageRead);
}
