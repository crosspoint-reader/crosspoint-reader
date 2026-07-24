#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "ReadingStatsUtils.h"

struct GlobalReadingStatsAggregation;

// Cumulative statistics for this device, stored in a CRC-protected CrossVi
// envelope at /.crosspoint/global_stats_v4.bin. Its payload remains compatible
// with CrossInk v1.4 and Nearby Stats Sync.
struct GlobalReadingStats {
  static constexpr uint8_t CURRENT_FILE_VERSION = 3;
  static constexpr size_t CURRENT_FILE_SIZE = 159;
  static constexpr size_t MIN_SUPPORTED_FILE_SIZE = 13;
  static constexpr size_t MAX_SYNCED_DEVICE_SNAPSHOTS = 32;

  uint32_t totalSessions = 0;
  uint32_t totalReadingSeconds = 0;
  uint32_t totalPagesTurned = 0;
  uint32_t completedBooks = 0;
  std::array<uint32_t, READING_TIME_BUCKET_COUNT> timeOfDaySeconds{};
  std::array<uint32_t, READING_DAY_OF_WEEK_COUNT> dayOfWeekSeconds{};
  uint32_t readingHistoryAnchorDay = 0;
  std::array<uint8_t, READING_HISTORY_BYTES> readingHistoryBits{};
  uint16_t longestReadingStreak = 0;
  // Exact time and session count for the most recent locally recorded calendar
  // day. These live in a separate CRC-protected sidecar so the CrossInk-
  // compatible global payload and Nearby Sync contract remain unchanged.
  uint32_t latestReadingDay = 0;
  uint32_t latestDayReadingSeconds = 0;
  uint32_t latestDaySessions = 0;
  bool hasLatestDayReadingSeconds = false;

  enum class LoadStatus : uint8_t {
    Ok,
    Missing,
    RecoveredBackup,
    RecoveredTemp,
    LoadedLegacy,
    Invalid,
    NewerFormat,
    IoError,
  };

  enum class BackupResult : uint8_t { Ok, Missing, Invalid, NewerFormat, IoError, Protected };

  static constexpr bool isTrustedLoadStatus(const LoadStatus status) {
    return status == LoadStatus::Ok || status == LoadStatus::Missing || status == LoadStatus::RecoveredBackup ||
           status == LoadStatus::RecoveredTemp || status == LoadStatus::LoadedLegacy;
  }

  static GlobalReadingStats load(LoadStatus* status = nullptr);
  static bool canPublish();
  static bool hasSyncedStats();
  static GlobalReadingStats loadAggregated();
  static GlobalReadingStats loadAggregated(const GlobalReadingStats& localStats);
  static GlobalReadingStatsAggregation loadAggregatedWithReport(const GlobalReadingStats& localStats);
  bool save() const;
  bool saveRedundant() const;
  static bool resetLocal();
  static BackupResult createBackup();
  static BackupResult restoreBackup();
  static bool hasValidBackup();

  void merge(const GlobalReadingStats& other);
  void recordReadingSpan(const ReadingStatsDateTime& localStart, uint32_t seconds);
  void recordReadingSession(const ReadingStatsDate& localDate);
  bool readingSecondsForDate(const ReadingStatsDate& date, uint32_t& seconds) const;
  bool readingSummaryForDate(const ReadingStatsDate& date, uint32_t& seconds, uint32_t& sessions) const;
  uint16_t currentReadingStreak(const ReadingStatsDate* today) const;
  uint16_t displayLongestReadingStreak() const;
};

// A single, immutable view of the synced-stats directory. Consumers can use
// validPeerCount to decide whether an "all synced" view really exists instead
// of treating an empty directory as data. skippedPeerCount covers recognized
// peer snapshots that could not be decoded, including a failed backup fallback.
// scanComplete is false when the directory could not be opened, enumerated, or
// named without an SD error. In that case stats is only a verified subtotal and
// must not be presented as a complete aggregate.
struct GlobalReadingStatsAggregation {
  GlobalReadingStats stats;
  uint16_t validPeerCount = 0;
  uint16_t skippedPeerCount = 0;
  bool scanComplete = true;
};
