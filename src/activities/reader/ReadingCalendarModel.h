#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "ReadingStatsUtils.h"

struct ReadingCalendarSnapshot {
  ReadingStatsDate today;
  uint32_t anchorDay = 0;
  std::array<uint8_t, READING_HISTORY_BYTES> historyBits{};
  uint16_t readingDays = 0;
  uint16_t currentStreak = 0;
  bool clockValid = false;
  bool historyAvailable = false;
};

struct ReadingCalendarCell {
  ReadingStatsDate date;
  bool inMonth = false;
  bool tracked = false;
  bool read = false;
  bool today = false;
};

class ReadingCalendarModel {
 public:
  explicit ReadingCalendarModel(ReadingCalendarSnapshot snapshot);

  bool isAvailable() const { return snapshot_.clockValid && snapshot_.historyAvailable; }
  const ReadingStatsDate& visibleMonth() const { return visibleMonth_; }
  const ReadingCalendarSnapshot& snapshot() const { return snapshot_; }
  ReadingCalendarCell cellAt(size_t index) const;

  bool canMovePrevious() const;
  bool canMoveNext() const;
  bool movePrevious();
  bool moveNext();

 private:
  uint32_t earliestTrackedDay() const;
  bool isReadDay(uint32_t dayIndex) const;
  bool moveMonth(int delta);

  ReadingCalendarSnapshot snapshot_;
  ReadingStatsDate visibleMonth_;
};
