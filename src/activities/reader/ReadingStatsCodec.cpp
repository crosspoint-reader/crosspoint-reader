#include "ReadingStatsCodec.h"

#include <algorithm>
#include <cstring>

namespace {
constexpr size_t BOOK_SIZE_V1 = 11;
constexpr size_t BOOK_SIZE_V2 = 12;
constexpr size_t BOOK_SIZE_V3 = 16;
constexpr size_t BOOK_SIZE_V4 = 69;
constexpr size_t GLOBAL_SIZE_V1 = 13;
constexpr size_t GLOBAL_SIZE_V2 = 17;
constexpr uint8_t FLAG_START_DATE_MANUAL = 1u << 0;
constexpr uint8_t FLAG_FINISHED_DATE_MANUAL = 1u << 1;

uint16_t readLe16(const uint8_t* data, const size_t offset) {
  return static_cast<uint16_t>(data[offset]) | static_cast<uint16_t>(data[offset + 1]) << 8;
}

uint32_t readLe32(const uint8_t* data, const size_t offset) {
  return static_cast<uint32_t>(data[offset]) | static_cast<uint32_t>(data[offset + 1]) << 8 |
         static_cast<uint32_t>(data[offset + 2]) << 16 | static_cast<uint32_t>(data[offset + 3]) << 24;
}

void writeLe16(uint8_t* data, const size_t offset, const uint16_t value) {
  data[offset] = static_cast<uint8_t>(value);
  data[offset + 1] = static_cast<uint8_t>(value >> 8);
}

void writeLe32(uint8_t* data, const size_t offset, const uint32_t value) {
  data[offset] = static_cast<uint8_t>(value);
  data[offset + 1] = static_cast<uint8_t>(value >> 8);
  data[offset + 2] = static_cast<uint8_t>(value >> 16);
  data[offset + 3] = static_cast<uint8_t>(value >> 24);
}

ReadingStatsDate readDate(const uint8_t* data, const size_t offset) {
  ReadingStatsDate date{readLe16(data, offset), data[offset + 2], data[offset + 3]};
  if (!date.isValid()) date.clear();
  return date;
}

void readBookCommon(const uint8_t* data, BookReadingStats& stats) {
  stats.sessionCount = readLe16(data, 1);
  stats.totalReadingSeconds = readLe32(data, 3);
  stats.totalPagesTurned = readLe32(data, 7);
}

void normalizePace(BookReadingStats& stats) {
  if (stats.avgSecondsPerForwardPage == 0 || stats.paceSampleCount == 0) {
    stats.avgSecondsPerForwardPage = 0;
    stats.paceSampleCount = 0;
  } else {
    stats.paceSampleCount = std::min(stats.paceSampleCount, BookReadingStats::MAX_PACE_SAMPLE_COUNT);
  }
}

void readGlobalCommon(const uint8_t* data, GlobalReadingStats& stats) {
  stats.totalSessions = readLe32(data, 1);
  stats.totalReadingSeconds = readLe32(data, 5);
  stats.totalPagesTurned = readLe32(data, 9);
}
}  // namespace

namespace ReadingStatsCodec {

BookBytes encode(const BookReadingStats& stats) {
  BookBytes data{};
  data[0] = BookReadingStats::CURRENT_FILE_VERSION;
  writeLe16(data.data(), 1, stats.sessionCount);
  writeLe32(data.data(), 3, stats.totalReadingSeconds);
  writeLe32(data.data(), 7, stats.totalPagesTurned);
  data[11] = stats.isCompleted ? 1 : 0;
  writeLe16(data.data(), 12, stats.avgSecondsPerForwardPage);
  writeLe16(data.data(), 14, std::min(stats.paceSampleCount, BookReadingStats::MAX_PACE_SAMPLE_COUNT));
  data[16] = (stats.startDateManual && stats.startDate.isValid() ? FLAG_START_DATE_MANUAL : 0u) |
             (stats.finishedDateManual && stats.finishedDate.isValid() ? FLAG_FINISHED_DATE_MANUAL : 0u);
  writeLe16(data.data(), 17, stats.startDate.isValid() ? stats.startDate.year : 0);
  data[19] = stats.startDate.isValid() ? stats.startDate.month : 0;
  data[20] = stats.startDate.isValid() ? stats.startDate.day : 0;
  writeLe16(data.data(), 21, stats.finishedDate.isValid() ? stats.finishedDate.year : 0);
  data[23] = stats.finishedDate.isValid() ? stats.finishedDate.month : 0;
  data[24] = stats.finishedDate.isValid() ? stats.finishedDate.day : 0;
  for (size_t i = 0; i < stats.timeOfDaySeconds.size(); ++i)
    writeLe32(data.data(), 25 + i * 4, stats.timeOfDaySeconds[i]);
  for (size_t i = 0; i < stats.dayOfWeekSeconds.size(); ++i)
    writeLe32(data.data(), 41 + i * 4, stats.dayOfWeekSeconds[i]);
  writeLe32(data.data(), 69, stats.estimatedTimeLeftSeconds);
  return data;
}

GlobalBytes encode(const GlobalReadingStats& stats) {
  GlobalBytes data{};
  data[0] = GlobalReadingStats::CURRENT_FILE_VERSION;
  writeLe32(data.data(), 1, stats.totalSessions);
  writeLe32(data.data(), 5, stats.totalReadingSeconds);
  writeLe32(data.data(), 9, stats.totalPagesTurned);
  writeLe32(data.data(), 13, stats.completedBooks);
  for (size_t i = 0; i < stats.timeOfDaySeconds.size(); ++i)
    writeLe32(data.data(), 17 + i * 4, stats.timeOfDaySeconds[i]);
  for (size_t i = 0; i < stats.dayOfWeekSeconds.size(); ++i)
    writeLe32(data.data(), 33 + i * 4, stats.dayOfWeekSeconds[i]);
  writeLe32(data.data(), 61, stats.readingHistoryAnchorDay);
  memcpy(data.data() + 65, stats.readingHistoryBits.data(), stats.readingHistoryBits.size());
  data[156] &= 0x03;  // Only 730 of the 736 allocated bits belong to the history.
  writeLe16(data.data(), 157, std::min<uint16_t>(stats.longestReadingStreak, READING_HISTORY_DAYS));
  return data;
}

ReadingStatsDecodeResult decode(const uint8_t* data, const size_t size, BookReadingStats& stats) {
  stats = {};
  if (!data || size == 0) return ReadingStatsDecodeResult::Invalid;
  if (data[0] > BookReadingStats::CURRENT_FILE_VERSION || size > BookReadingStats::CURRENT_FILE_SIZE) {
    return ReadingStatsDecodeResult::NewerFormat;
  }

  BookReadingStats decoded;
  if (size == BOOK_SIZE_V1 && data[0] == 1) {
    readBookCommon(data, decoded);
  } else if (size == BOOK_SIZE_V2 && data[0] == 2) {
    readBookCommon(data, decoded);
    decoded.isCompleted = data[11] != 0;
  } else if (size == BOOK_SIZE_V3 && data[0] == 3) {
    readBookCommon(data, decoded);
    decoded.isCompleted = data[11] != 0;
    decoded.avgSecondsPerForwardPage = readLe16(data, 12);
    decoded.paceSampleCount = readLe16(data, 14);
  } else if ((size == BOOK_SIZE_V4 && data[0] == 4) ||
             (size == BookReadingStats::CURRENT_FILE_SIZE && data[0] == BookReadingStats::CURRENT_FILE_VERSION)) {
    readBookCommon(data, decoded);
    decoded.isCompleted = data[11] != 0;
    decoded.avgSecondsPerForwardPage = readLe16(data, 12);
    decoded.paceSampleCount = readLe16(data, 14);
    const uint8_t flags = data[16];
    decoded.startDate = readDate(data, 17);
    decoded.finishedDate = readDate(data, 21);
    decoded.startDateManual = decoded.startDate.isValid() && (flags & FLAG_START_DATE_MANUAL) != 0;
    decoded.finishedDateManual = decoded.finishedDate.isValid() && (flags & FLAG_FINISHED_DATE_MANUAL) != 0;
    for (size_t i = 0; i < decoded.timeOfDaySeconds.size(); ++i)
      decoded.timeOfDaySeconds[i] = readLe32(data, 25 + i * 4);
    for (size_t i = 0; i < decoded.dayOfWeekSeconds.size(); ++i)
      decoded.dayOfWeekSeconds[i] = readLe32(data, 41 + i * 4);
    if (size == BookReadingStats::CURRENT_FILE_SIZE) decoded.estimatedTimeLeftSeconds = readLe32(data, 69);
  } else {
    return ReadingStatsDecodeResult::Invalid;
  }
  normalizePace(decoded);
  stats = decoded;
  return ReadingStatsDecodeResult::Ok;
}

ReadingStatsDecodeResult decode(const uint8_t* data, const size_t size, GlobalReadingStats& stats) {
  stats = {};
  if (!data || size == 0) return ReadingStatsDecodeResult::Invalid;
  if (data[0] > GlobalReadingStats::CURRENT_FILE_VERSION || size > GlobalReadingStats::CURRENT_FILE_SIZE) {
    return ReadingStatsDecodeResult::NewerFormat;
  }

  GlobalReadingStats decoded;
  if (size == GLOBAL_SIZE_V1 && data[0] == 1) {
    readGlobalCommon(data, decoded);
  } else if (size == GLOBAL_SIZE_V2 && data[0] == 2) {
    readGlobalCommon(data, decoded);
    decoded.completedBooks = readLe32(data, 13);
  } else if (size == GlobalReadingStats::CURRENT_FILE_SIZE && data[0] == GlobalReadingStats::CURRENT_FILE_VERSION) {
    readGlobalCommon(data, decoded);
    decoded.completedBooks = readLe32(data, 13);
    for (size_t i = 0; i < decoded.timeOfDaySeconds.size(); ++i)
      decoded.timeOfDaySeconds[i] = readLe32(data, 17 + i * 4);
    for (size_t i = 0; i < decoded.dayOfWeekSeconds.size(); ++i)
      decoded.dayOfWeekSeconds[i] = readLe32(data, 33 + i * 4);
    decoded.readingHistoryAnchorDay = readLe32(data, 61);
    memcpy(decoded.readingHistoryBits.data(), data + 65, decoded.readingHistoryBits.size());
    decoded.readingHistoryBits.back() &= 0x03;
    decoded.longestReadingStreak = std::min<uint16_t>(readLe16(data, 157), READING_HISTORY_DAYS);
    ReadingStatsDate anchorDate;
    if (!readingStatsDateFromDayIndex(decoded.readingHistoryAnchorDay, anchorDate)) {
      decoded.readingHistoryAnchorDay = 0;
      decoded.readingHistoryBits.fill(0);
      decoded.longestReadingStreak = 0;
    }
  } else {
    return ReadingStatsDecodeResult::Invalid;
  }
  stats = decoded;
  return ReadingStatsDecodeResult::Ok;
}

}  // namespace ReadingStatsCodec
