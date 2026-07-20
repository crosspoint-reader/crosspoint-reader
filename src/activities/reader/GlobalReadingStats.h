#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "ReadingStatsUtils.h"

// Cumulative statistics for this device, stored at
// /.crosspoint/global_stats.bin. The binary layout remains compatible with
// CrossInk v1.4 and Nearby Stats Sync.
struct GlobalReadingStats {
  static constexpr uint8_t CURRENT_FILE_VERSION = 3;
  static constexpr size_t CURRENT_FILE_SIZE = 159;
  static constexpr size_t MIN_SUPPORTED_FILE_SIZE = 13;

  uint32_t totalSessions = 0;
  uint32_t totalReadingSeconds = 0;
  uint32_t totalPagesTurned = 0;
  uint32_t completedBooks = 0;
  std::array<uint32_t, READING_TIME_BUCKET_COUNT> timeOfDaySeconds{};
  std::array<uint32_t, READING_DAY_OF_WEEK_COUNT> dayOfWeekSeconds{};
  uint32_t readingHistoryAnchorDay = 0;
  std::array<uint8_t, READING_HISTORY_BYTES> readingHistoryBits{};
  uint16_t longestReadingStreak = 0;

  enum class LoadStatus : uint8_t {
    Ok,
    Missing,
    RecoveredBackup,
    RecoveredTemp,
    Invalid,
    NewerFormat,
    IoError,
  };

  static constexpr bool isTrustedLoadStatus(const LoadStatus status) {
    return status == LoadStatus::Ok || status == LoadStatus::Missing || status == LoadStatus::RecoveredBackup ||
           status == LoadStatus::RecoveredTemp;
  }

  static GlobalReadingStats load(LoadStatus* status = nullptr);
  static bool hasSyncedStats();
  static GlobalReadingStats loadAggregated();
  static GlobalReadingStats loadAggregated(const GlobalReadingStats& localStats);
  bool save() const;
  static bool resetLocal();

  void merge(const GlobalReadingStats& other);
  void recordReadingSpan(const ReadingStatsDateTime& localStart, uint32_t seconds);
  uint16_t currentReadingStreak(const ReadingStatsDate* today) const;
  uint16_t displayLongestReadingStreak() const;
};
