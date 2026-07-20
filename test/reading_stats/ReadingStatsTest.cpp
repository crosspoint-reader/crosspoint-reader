#include <HalStorage.h>
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "BookReadingStats.h"
#include "GlobalReadingStats.h"
#include "ReadingSessionTracker.h"
#include "ReadingStatsCodec.h"
#include "ReadingStatsStorage.h"
#include "ReadingStatsUtils.h"

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
}  // namespace

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
