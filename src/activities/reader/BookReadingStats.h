#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "ReadingStatsUtils.h"

// Per-book reading statistics, persisted in the book cache as stats_v5.bin.
// The binary layout remains compatible with CrossInk v1.4.
struct BookReadingStats {
  static constexpr uint8_t CURRENT_FILE_VERSION = 5;
  static constexpr size_t CURRENT_FILE_SIZE = 73;
  static constexpr uint16_t MAX_PACE_SAMPLE_COUNT = 1000;

  uint16_t sessionCount = 0;
  uint32_t totalReadingSeconds = 0;
  uint32_t totalPagesTurned = 0;
  bool isCompleted = false;
  uint16_t avgSecondsPerForwardPage = 0;
  uint16_t paceSampleCount = 0;
  uint32_t estimatedTimeLeftSeconds = 0;
  bool startDateManual = false;
  bool finishedDateManual = false;
  ReadingStatsDate startDate;
  ReadingStatsDate finishedDate;
  std::array<uint32_t, READING_TIME_BUCKET_COUNT> timeOfDaySeconds{};
  std::array<uint32_t, READING_DAY_OF_WEEK_COUNT> dayOfWeekSeconds{};

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

  static constexpr bool isTrustedLoadStatus(const LoadStatus status) {
    return status == LoadStatus::Ok || status == LoadStatus::Missing || status == LoadStatus::RecoveredBackup ||
           status == LoadStatus::RecoveredTemp || status == LoadStatus::LoadedLegacy;
  }

  static BookReadingStats load(const std::string& cachePath, LoadStatus* status = nullptr);
  bool save(const std::string& cachePath) const;
  static bool remove(const std::string& cachePath);

  void recordForwardPageRead(uint32_t seconds);
  void recordReadingSpan(const ReadingStatsDateTime& localStart, uint32_t seconds);
  static void formatDuration(uint32_t seconds, char* buffer, size_t length);
};
