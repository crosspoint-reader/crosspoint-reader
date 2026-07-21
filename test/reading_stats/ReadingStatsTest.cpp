#include <HalStorage.h>
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "BookReadingStats.h"
#include "GlobalReadingStats.h"
#include "ReadingSessionTracker.h"
#include "ReadingStatsCodec.h"
#include "ReadingStatsCompletionTransaction.h"
#include "ReadingStatsStorage.h"
#include "ReadingStatsUtils.h"
#include "activities/network/NearbyStatsPolicy.h"

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
}

template <typename Bytes>
std::vector<uint8_t> asVector(const Bytes& bytes) {
  return {bytes.begin(), bytes.end()};
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
constexpr char BOOK_STATS_PATH[] = "/.crosspoint/epub_12345/stats_v5.bin";
constexpr char GLOBAL_STATS_PATH[] = "/.crosspoint/global_stats.bin";
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
  transition.oldGlobal.totalReadingSeconds = 999;
  transition.oldGlobal.totalPagesTurned = 42;
  transition.oldGlobal.completedBooks = completedBooks;
  transition.newGlobal = transition.oldGlobal;
  transition.newGlobal.completedBooks =
      completedBooks == std::numeric_limits<uint32_t>::max() ? completedBooks : completedBooks + 1;
  return transition;
}

void seedCompletionTransition(const CompletionTransition& transition) {
  Storage.reset();
  Storage.setFile(BOOK_STATS_PATH, asVector(ReadingStatsCodec::encode(transition.oldBook)));
  Storage.setFile(GLOBAL_STATS_PATH, asVector(ReadingStatsCodec::encode(transition.oldGlobal)));
}

void expectStoredTransition(const CompletionTransition& transition, const bool completed) {
  BookReadingStats::LoadStatus bookStatus = BookReadingStats::LoadStatus::Invalid;
  GlobalReadingStats::LoadStatus globalStatus = GlobalReadingStats::LoadStatus::Invalid;
  const BookReadingStats book = BookReadingStats::load(COMPLETION_CACHE_PATH, &bookStatus);
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
}  // namespace

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

TEST(ReadingStatsCompletionTransaction, SupportsExplicitIncompleteWithoutUnderflow) {
  CompletionTransition transition = completionTransition(0);
  transition.oldBook = transition.newBook;
  transition.oldGlobal = transition.newGlobal;
  transition.oldGlobal.completedBooks = 0;
  transition.newBook = transition.oldBook;
  transition.newBook.isCompleted = false;
  transition.newBook.finishedDate.clear();
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

TEST(ReadingStatsCompletionTransaction, RecoversEveryPublishBoundary) {
  const CompletionTransition transition = completionTransition();
  for (size_t failedRename = 1; failedRename <= 5; ++failedRename) {
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

TEST(ReadingStatsCompletionTransaction, RecoversWriteAndSyncFailuresAtEveryFile) {
  const CompletionTransition transition = completionTransition();
  for (size_t failedWrite = 1; failedWrite <= 3; ++failedWrite) {
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
  Storage.failRemoveOnCall(1);
  EXPECT_FALSE(ReadingStatsCompletionTransaction::commit(COMPLETION_CACHE_PATH, transition.oldBook, transition.newBook,
                                                         transition.oldGlobal, transition.newGlobal));
  ASSERT_TRUE(Storage.exists(COMPLETION_MARKER_PATH));
  const std::vector<uint8_t> replay = Storage.file(COMPLETION_MARKER_PATH);
  expectStoredTransition(transition, true);

  Storage.resetFaultInjection();
  EXPECT_EQ(ReadingStatsCompletionTransaction::recover(COMPLETION_CACHE_PATH),
            ReadingStatsCompletionTransaction::RecoveryResult::Recovered);
  Storage.setFile(COMPLETION_MARKER_PATH, replay);
  EXPECT_EQ(ReadingStatsCompletionTransaction::recover(COMPLETION_CACHE_PATH),
            ReadingStatsCompletionTransaction::RecoveryResult::Recovered);
  expectStoredTransition(transition, true);
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
  marker[4] = 2;
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
  Storage.setFile(BOOK_STATS_PATH, asVector(ReadingStatsCodec::encode(different)));
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

TEST(ReadingStatsCompletionTransaction, RejectsValidCrcMarkerOutsideEpubCacheNamespace) {
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
  Storage.setFile(BOOK_STATS_PATH, asVector(ReadingStatsCodec::encode(missing.oldBook)));
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
  Storage.setFile(std::string(BOOK_STATS_PATH) + ".tmp", asVector(ReadingStatsCodec::encode(transition.newBook)));
  Storage.setFile(std::string(GLOBAL_STATS_PATH) + ".tmp", asVector(ReadingStatsCodec::encode(transition.newGlobal)));
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

TEST(ReadingStatsCodec, BookV5RoundTripPreservesCrossInkLayout) {
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
  input.timeOfDaySeconds = {1, 2, 3, 4};
  input.dayOfWeekSeconds = {5, 6, 7, 8, 9, 10, 11};

  const auto encoded = ReadingStatsCodec::encode(input);
  EXPECT_EQ(encoded.size(), 73u);
  EXPECT_EQ(encoded[0], 5);
  BookReadingStats output;
  EXPECT_EQ(ReadingStatsCodec::decode(encoded.data(), encoded.size(), output), ReadingStatsDecodeResult::Ok);
  expectBookStatsEqual(input, output);
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

TEST(NearbyStatsPolicy, IgnoresTheNonMonotonicRollingHistoryWindow) {
  GlobalReadingStats existing;
  existing.readingHistoryAnchorDay = 100;
  existing.readingHistoryBits.fill(0xFF);

  GlobalReadingStats incoming = existing;
  incoming.readingHistoryAnchorDay = 200;
  incoming.readingHistoryBits.fill(0);
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
  constexpr char PRIMARY[] = "/book/stats_v5.bin";
  constexpr char BACKUP[] = "/book/stats_v5.bin.bak";
  constexpr char TEMP[] = "/book/stats_v5.bin.tmp";
  BookReadingStats primary;
  primary.totalReadingSeconds = 10;
  BookReadingStats backup;
  backup.totalReadingSeconds = 20;
  BookReadingStats temporary;
  temporary.totalReadingSeconds = 30;
  Storage.setFile(PRIMARY, asVector(ReadingStatsCodec::encode(primary)));
  Storage.setFile(BACKUP, asVector(ReadingStatsCodec::encode(backup)));
  Storage.setFile(TEMP, asVector(ReadingStatsCodec::encode(temporary)));

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

TEST(ReadingStatsPersistence, BookDoesNotFallBackPastAnUnreadableCommittedCopy) {
  Storage.reset();
  constexpr char PRIMARY[] = "/book/stats_v5.bin";
  constexpr char BACKUP[] = "/book/stats_v5.bin.bak";
  const auto primary = asVector(ReadingStatsCodec::encode(bookStatsWithSeconds(10)));
  const auto backup = asVector(ReadingStatsCodec::encode(bookStatsWithSeconds(20)));
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

TEST(ReadingStatsPersistence, BookNeverDeletesProtectedFiles) {
  Storage.reset();
  constexpr char TEMP[] = "/book/stats_v5.bin.tmp";
  auto newer = ReadingStatsCodec::encode(BookReadingStats{});
  newer[0] = BookReadingStats::CURRENT_FILE_VERSION + 1;
  const std::vector<uint8_t> expected = asVector(newer);

  for (const char* protectedPath : {"/book/stats_v5.bin", "/book/stats_v5.bin.bak"}) {
    Storage.reset();
    Storage.setFile(protectedPath, expected);
    EXPECT_FALSE(BookReadingStats::remove("/book"));
    EXPECT_EQ(Storage.file(protectedPath), expected);
  }

  Storage.reset();
  Storage.setFile(TEMP, expected);

  EXPECT_FALSE(BookReadingStats{}.save("/book"));
  EXPECT_EQ(Storage.file(TEMP), expected);
  EXPECT_FALSE(BookReadingStats::remove("/book"));
  EXPECT_EQ(Storage.file(TEMP), expected);

  Storage.reset();
  const std::vector<uint8_t> oversized(BookReadingStats::CURRENT_FILE_SIZE + 1, 0xA5);
  Storage.setFile(TEMP, oversized);
  EXPECT_FALSE(BookReadingStats{}.save("/book"));
  EXPECT_FALSE(BookReadingStats::remove("/book"));
  EXPECT_EQ(Storage.file(TEMP), oversized);

  Storage.reset();
  const std::vector<uint8_t> valid = asVector(ReadingStatsCodec::encode(BookReadingStats{}));
  Storage.setFile(TEMP, valid);
  Storage.makeUnreadable(TEMP);
  EXPECT_FALSE(BookReadingStats{}.save("/book"));
  EXPECT_FALSE(BookReadingStats::remove("/book"));
  EXPECT_EQ(Storage.file(TEMP), valid);
}

TEST(ReadingStatsPersistence, BookRemovePreflightsCorruptAndNewerLegacyFiles) {
  const std::vector<uint8_t> valid = asVector(ReadingStatsCodec::encode(BookReadingStats{}));
  auto newerBytes = valid;
  newerBytes[0] = BookReadingStats::CURRENT_FILE_VERSION + 1;

  for (const char* legacyPath : {"/book/stats_v4.bin", "/book/stats.bin"}) {
    Storage.reset();
    Storage.setFile("/book/stats_v5.bin", valid);
    Storage.setFile(legacyPath, {BookReadingStats::CURRENT_FILE_VERSION});
    EXPECT_FALSE(BookReadingStats::remove("/book"));
    EXPECT_EQ(Storage.file("/book/stats_v5.bin"), valid);
    EXPECT_EQ(Storage.file(legacyPath), std::vector<uint8_t>{BookReadingStats::CURRENT_FILE_VERSION});

    Storage.reset();
    Storage.setFile("/book/stats_v5.bin", valid);
    Storage.setFile(legacyPath, newerBytes);
    EXPECT_FALSE(BookReadingStats::remove("/book"));
    EXPECT_EQ(Storage.file("/book/stats_v5.bin"), valid);
    EXPECT_EQ(Storage.file(legacyPath), newerBytes);
  }

  Storage.reset();
  Storage.setFile("/book/stats_v5.bin", valid);
  Storage.setFile("/book/stats.bin", valid);
  Storage.makeUnreadable("/book/stats.bin");
  EXPECT_FALSE(BookReadingStats::remove("/book"));
  EXPECT_EQ(Storage.file("/book/stats_v5.bin"), valid);
  EXPECT_EQ(Storage.file("/book/stats.bin"), valid);

  Storage.reset();
  auto previous = valid;
  previous.resize(69);
  previous[0] = 4;
  const std::vector<uint8_t> legacy{1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  for (const char* path : {"/book/stats_v5.bin", "/book/stats_v5.bin.tmp", "/book/stats_v5.bin.bak"}) {
    Storage.setFile(path, valid);
  }
  Storage.setFile("/book/stats_v4.bin", previous);
  Storage.setFile("/book/stats.bin", legacy);
  EXPECT_TRUE(BookReadingStats::remove("/book"));
  for (const char* path : {"/book/stats_v5.bin", "/book/stats_v5.bin.tmp", "/book/stats_v5.bin.bak",
                           "/book/stats_v4.bin", "/book/stats.bin"}) {
    EXPECT_FALSE(Storage.exists(path));
  }
}

TEST(ReadingStatsPersistence, GlobalReportsTrustedRecoverySource) {
  Storage.reset();
  constexpr char PRIMARY[] = "/.crosspoint/global_stats.bin";
  constexpr char BACKUP[] = "/.crosspoint/global_stats.bin.bak";
  constexpr char TEMP[] = "/.crosspoint/global_stats.bin.tmp";
  Storage.setFile(PRIMARY, asVector(ReadingStatsCodec::encode(globalStatsWithSeconds(10))));
  Storage.setFile(BACKUP, asVector(ReadingStatsCodec::encode(globalStatsWithSeconds(20))));
  Storage.setFile(TEMP, asVector(ReadingStatsCodec::encode(globalStatsWithSeconds(30))));

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

TEST(ReadingStatsPersistence, GlobalRecoversPastCorruptCommittedFiles) {
  Storage.reset();
  constexpr char PRIMARY[] = "/.crosspoint/global_stats.bin";
  constexpr char BACKUP[] = "/.crosspoint/global_stats.bin.bak";
  constexpr char TEMP[] = "/.crosspoint/global_stats.bin.tmp";
  Storage.setFile(PRIMARY, {GlobalReadingStats::CURRENT_FILE_VERSION});
  Storage.setFile(BACKUP, asVector(ReadingStatsCodec::encode(globalStatsWithSeconds(20))));
  Storage.setFile(TEMP, asVector(ReadingStatsCodec::encode(globalStatsWithSeconds(30))));

  GlobalReadingStats::LoadStatus status = GlobalReadingStats::LoadStatus::Invalid;
  EXPECT_EQ(GlobalReadingStats::load(&status).totalReadingSeconds, 20u);
  EXPECT_EQ(status, GlobalReadingStats::LoadStatus::RecoveredBackup);

  Storage.setFile(BACKUP, {GlobalReadingStats::CURRENT_FILE_VERSION});
  EXPECT_EQ(GlobalReadingStats::load(&status).totalReadingSeconds, 30u);
  EXPECT_EQ(status, GlobalReadingStats::LoadStatus::RecoveredTemp);
}

TEST(ReadingStatsPersistence, GlobalReportsUntrustedFilesAndPreservesProtectedTemp) {
  constexpr char PRIMARY[] = "/.crosspoint/global_stats.bin";
  constexpr char TEMP[] = "/.crosspoint/global_stats.bin.tmp";
  GlobalReadingStats::LoadStatus status = GlobalReadingStats::LoadStatus::Ok;

  Storage.reset();
  Storage.setFile(PRIMARY, {GlobalReadingStats::CURRENT_FILE_VERSION});
  GlobalReadingStats::load(&status);
  EXPECT_EQ(status, GlobalReadingStats::LoadStatus::Invalid);
  EXPECT_FALSE(GlobalReadingStats::isTrustedLoadStatus(status));

  Storage.reset();
  auto newer = ReadingStatsCodec::encode(GlobalReadingStats{});
  newer[0] = GlobalReadingStats::CURRENT_FILE_VERSION + 1;
  const std::vector<uint8_t> newerBytes = asVector(newer);
  Storage.setFile(TEMP, newerBytes);
  GlobalReadingStats::load(&status);
  EXPECT_EQ(status, GlobalReadingStats::LoadStatus::NewerFormat);
  EXPECT_FALSE(GlobalReadingStats{}.save());
  EXPECT_EQ(Storage.file(TEMP), newerBytes);

  Storage.reset();
  const std::vector<uint8_t> oversized(GlobalReadingStats::CURRENT_FILE_SIZE + 1, 0xA5);
  Storage.setFile(TEMP, oversized);
  EXPECT_FALSE(GlobalReadingStats{}.save());
  EXPECT_EQ(Storage.file(TEMP), oversized);

  Storage.reset();
  const std::vector<uint8_t> valid = asVector(ReadingStatsCodec::encode(GlobalReadingStats{}));
  Storage.setFile(TEMP, valid);
  Storage.makeUnreadable(TEMP);
  GlobalReadingStats::load(&status);
  EXPECT_EQ(status, GlobalReadingStats::LoadStatus::IoError);
  EXPECT_FALSE(GlobalReadingStats{}.save());
  EXPECT_EQ(Storage.file(TEMP), valid);

  Storage.reset();
  const auto primary = asVector(ReadingStatsCodec::encode(globalStatsWithSeconds(10)));
  const auto backup = asVector(ReadingStatsCodec::encode(globalStatsWithSeconds(20)));
  Storage.setFile(PRIMARY, primary);
  Storage.setFile("/.crosspoint/global_stats.bin.bak", backup);
  Storage.makeUnreadable(PRIMARY);
  EXPECT_EQ(GlobalReadingStats::load(&status).totalReadingSeconds, 0u);
  EXPECT_EQ(status, GlobalReadingStats::LoadStatus::IoError);
  EXPECT_EQ(Storage.file(PRIMARY), primary);
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
  Storage.setFile(std::string(DIRECTORY) + "/device_cccccccccccc.bin",
                  asVector(ReadingStatsCodec::encode(globalStatsWithSeconds(30))));
  Storage.setFile(std::string(DIRECTORY) + "/device_cccccccccccc.bin.bak",
                  asVector(ReadingStatsCodec::encode(globalStatsWithSeconds(40))));
  Storage.setFile(std::string(DIRECTORY) + "/device_001122334455.bin",
                  asVector(ReadingStatsCodec::encode(globalStatsWithSeconds(100))));

  const GlobalReadingStats aggregated = GlobalReadingStats::loadAggregated(globalStatsWithSeconds(1));
  EXPECT_EQ(aggregated.totalReadingSeconds, 61u);
}

TEST(ReadingStatsDates, ValidateLeapDaysAndSplitAcrossBoundaries) {
  EXPECT_TRUE((ReadingStatsDate{2024, 2, 29}.isValid()));
  EXPECT_FALSE((ReadingStatsDate{2023, 2, 29}.isValid()));
  EXPECT_FALSE((ReadingStatsDateTime{{2024, 1, 1}, 24, 0, 0}).isValid());
  ReadingStatsDate date{2024, 2, 28};
  addDaysToReadingStatsDate(date, 1);
  EXPECT_EQ(date.day, 29);
  EXPECT_EQ(readingStatsDayOfWeekIndex({2024, 1, 1}), 0);

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

  const ReadingStatsDate januaryFourth{2024, 1, 4};
  EXPECT_EQ(stats.currentReadingStreak(&januaryFourth), 0);
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
