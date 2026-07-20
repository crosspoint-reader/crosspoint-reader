#include "ReadingStatsUtils.h"

#include <algorithm>
#include <cstdio>
#include <limits>

namespace {
constexpr const char* MONTH_NAMES[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                       "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

bool isBitSet(const std::array<uint8_t, READING_HISTORY_BYTES>& bits, const size_t bitIndex) {
  return bitIndex < READING_HISTORY_DAYS && (bits[bitIndex / 8] & static_cast<uint8_t>(1u << (bitIndex % 8))) != 0;
}

void setBit(std::array<uint8_t, READING_HISTORY_BYTES>& bits, const size_t bitIndex) {
  if (bitIndex < READING_HISTORY_DAYS) {
    bits[bitIndex / 8] |= static_cast<uint8_t>(1u << (bitIndex % 8));
  }
}

void shiftHistoryOlder(std::array<uint8_t, READING_HISTORY_BYTES>& bits, const size_t shiftDays) {
  if (shiftDays == 0) return;
  if (shiftDays >= READING_HISTORY_DAYS) {
    bits.fill(0);
    return;
  }

  std::array<uint8_t, READING_HISTORY_BYTES> shifted{};
  for (size_t bitIndex = 0; bitIndex + shiftDays < READING_HISTORY_DAYS; ++bitIndex) {
    if (isBitSet(bits, bitIndex)) setBit(shifted, bitIndex + shiftDays);
  }
  bits = shifted;
}

uint32_t secondsUntilNextBucketBoundary(const ReadingStatsDateTime& dateTime) {
  const uint32_t current =
      static_cast<uint32_t>(dateTime.hour) * 3600u + static_cast<uint32_t>(dateTime.minute) * 60u + dateTime.second;
  uint32_t boundary = 24u * 3600u;
  if (dateTime.hour < 5) {
    boundary = 5u * 3600u;
  } else if (dateTime.hour < 12) {
    boundary = 12u * 3600u;
  } else if (dateTime.hour < 17) {
    boundary = 17u * 3600u;
  } else if (dateTime.hour < 21) {
    boundary = 21u * 3600u;
  }
  return boundary > current ? boundary - current : 1u;
}
}  // namespace

bool ReadingStatsDate::isValid() const { return isValidReadingStatsDate(*this); }

void ReadingStatsDate::clear() { *this = {}; }

bool ReadingStatsDateTime::isValid() const { return date.isValid() && hour < 24 && minute < 60 && second < 60; }

uint32_t addReadingStatsSaturated(const uint32_t lhs, const uint32_t rhs) {
  const uint32_t max = std::numeric_limits<uint32_t>::max();
  return max - lhs < rhs ? max : lhs + rhs;
}

bool isLeapYear(const uint16_t year) { return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0; }

uint8_t daysInMonth(const uint16_t year, const uint8_t month) {
  static constexpr uint8_t DAYS[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month < 1 || month > 12) return 0;
  return month == 2 && isLeapYear(year) ? 29 : DAYS[month - 1];
}

bool isValidReadingStatsDate(const ReadingStatsDate& date) {
  if (date.year < 2000 || date.year > 2099) return false;
  const uint8_t monthDays = daysInMonth(date.year, date.month);
  return monthDays > 0 && date.day >= 1 && date.day <= monthDays;
}

int compareReadingStatsDate(const ReadingStatsDate& lhs, const ReadingStatsDate& rhs) {
  if (lhs.year != rhs.year) return lhs.year < rhs.year ? -1 : 1;
  if (lhs.month != rhs.month) return lhs.month < rhs.month ? -1 : 1;
  if (lhs.day != rhs.day) return lhs.day < rhs.day ? -1 : 1;
  return 0;
}

uint32_t readingStatsDayIndex(const ReadingStatsDate& date) {
  if (!date.isValid()) return 0;
  uint32_t dayIndex = 0;
  for (uint16_t year = 2000; year < date.year; ++year) dayIndex += isLeapYear(year) ? 366u : 365u;
  for (uint8_t month = 1; month < date.month; ++month) dayIndex += daysInMonth(date.year, month);
  return dayIndex + static_cast<uint32_t>(date.day - 1);
}

bool readingStatsDateFromDayIndex(uint32_t dayIndex, ReadingStatsDate& outDate) {
  outDate = {};
  uint16_t year = 2000;
  while (year <= 2099) {
    const uint32_t yearDays = isLeapYear(year) ? 366u : 365u;
    if (dayIndex < yearDays) break;
    dayIndex -= yearDays;
    ++year;
  }
  if (year > 2099) return false;

  uint8_t month = 1;
  while (month <= 12) {
    const uint8_t monthDays = daysInMonth(year, month);
    if (dayIndex < monthDays) break;
    dayIndex -= monthDays;
    ++month;
  }
  if (month > 12) return false;

  outDate = {year, month, static_cast<uint8_t>(dayIndex + 1u)};
  return true;
}

void addDaysToReadingStatsDate(ReadingStatsDate& date, const int delta) {
  if (!date.isValid() || delta == 0) return;
  const int64_t lastDay = readingStatsDayIndex({2099, 12, 31});
  const int64_t target = std::clamp<int64_t>(static_cast<int64_t>(readingStatsDayIndex(date)) + delta, 0, lastDay);
  readingStatsDateFromDayIndex(static_cast<uint32_t>(target), date);
}

void addSecondsToReadingStatsDateTime(ReadingStatsDateTime& dateTime, const uint32_t seconds) {
  if (!dateTime.isValid() || seconds == 0) return;
  constexpr uint64_t SECONDS_PER_DAY = 24u * 3600u;
  const uint64_t current = static_cast<uint64_t>(readingStatsDayIndex(dateTime.date)) * SECONDS_PER_DAY +
                           static_cast<uint32_t>(dateTime.hour) * 3600u + static_cast<uint32_t>(dateTime.minute) * 60u +
                           dateTime.second;
  const uint64_t max = (static_cast<uint64_t>(readingStatsDayIndex({2099, 12, 31})) + 1u) * SECONDS_PER_DAY - 1u;
  const uint64_t next = std::min(current + seconds, max);
  readingStatsDateFromDayIndex(static_cast<uint32_t>(next / SECONDS_PER_DAY), dateTime.date);
  uint32_t secondOfDay = static_cast<uint32_t>(next % SECONDS_PER_DAY);
  dateTime.hour = static_cast<uint8_t>(secondOfDay / 3600u);
  secondOfDay %= 3600u;
  dateTime.minute = static_cast<uint8_t>(secondOfDay / 60u);
  dateTime.second = static_cast<uint8_t>(secondOfDay % 60u);
}

uint8_t readingStatsDayOfWeekIndex(const ReadingStatsDate& date) {
  // 2000-01-01 was Saturday. Monday is index 0.
  return static_cast<uint8_t>((5u + readingStatsDayIndex(date)) % 7u);
}

ReadingTimeBucket readingTimeBucketForHour(const uint8_t hour) {
  if (hour >= 5 && hour < 12) return ReadingTimeBucket::Morning;
  if (hour >= 12 && hour < 17) return ReadingTimeBucket::Afternoon;
  if (hour >= 17 && hour < 21) return ReadingTimeBucket::Evening;
  return ReadingTimeBucket::Night;
}

uint16_t readingSpanDaysInclusive(const ReadingStatsDate& start, const ReadingStatsDate& end) {
  if (!start.isValid() || !end.isValid() || compareReadingStatsDate(end, start) < 0) return 0;
  return static_cast<uint16_t>(readingStatsDayIndex(end) - readingStatsDayIndex(start) + 1u);
}

uint16_t readingSpanDaysElapsed(const ReadingStatsDate& start, const ReadingStatsDate& end) {
  if (!start.isValid() || !end.isValid() || compareReadingStatsDate(end, start) < 0) return 0;
  return static_cast<uint16_t>(readingStatsDayIndex(end) - readingStatsDayIndex(start));
}

void formatReadingStatsShortDate(const ReadingStatsDate& date, char* buffer, const size_t length) {
  if (!buffer || length == 0) return;
  if (!date.isValid()) {
    snprintf(buffer, length, "-");
    return;
  }
  snprintf(buffer, length, "%s %u", MONTH_NAMES[date.month - 1], static_cast<unsigned>(date.day));
}

void formatReadingStatsMonthToken(const ReadingStatsDate& date, char* buffer, const size_t length) {
  if (!buffer || length == 0) return;
  snprintf(buffer, length, "%s", date.isValid() ? MONTH_NAMES[date.month - 1] : "-");
}

void recordReadingSpanIntoBuckets(std::array<uint32_t, READING_TIME_BUCKET_COUNT>& timeOfDaySeconds,
                                  std::array<uint32_t, READING_DAY_OF_WEEK_COUNT>& dayOfWeekSeconds,
                                  const ReadingStatsDateTime& localStart, const uint32_t seconds) {
  if (!localStart.isValid() || seconds == 0) return;
  ReadingStatsDateTime cursor = localStart;
  uint32_t remaining = seconds;
  while (remaining > 0) {
    const uint8_t bucket = static_cast<uint8_t>(readingTimeBucketForHour(cursor.hour));
    const uint8_t day = readingStatsDayOfWeekIndex(cursor.date);
    const uint32_t segment = std::min(remaining, secondsUntilNextBucketBoundary(cursor));
    timeOfDaySeconds[bucket] = addReadingStatsSaturated(timeOfDaySeconds[bucket], segment);
    dayOfWeekSeconds[day] = addReadingStatsSaturated(dayOfWeekSeconds[day], segment);
    remaining -= segment;
    const uint32_t previousDay = readingStatsDayIndex(cursor.date);
    const uint32_t previousSecond =
        static_cast<uint32_t>(cursor.hour) * 3600u + static_cast<uint32_t>(cursor.minute) * 60u + cursor.second;
    addSecondsToReadingStatsDateTime(cursor, segment);
    if (remaining > 0 && previousDay == readingStatsDayIndex(cursor.date) &&
        previousSecond ==
            static_cast<uint32_t>(cursor.hour) * 3600u + static_cast<uint32_t>(cursor.minute) * 60u + cursor.second) {
      break;  // The supported calendar ends at 2099-12-31.
    }
  }
}

void markReadingHistoryDay(uint32_t& anchorDay, std::array<uint8_t, READING_HISTORY_BYTES>& bits,
                           const uint32_t dayIndex) {
  if (anchorDay == 0 && !isBitSet(bits, 0)) {
    anchorDay = dayIndex;
    bits.fill(0);
    setBit(bits, 0);
    return;
  }
  if (dayIndex > anchorDay) {
    shiftHistoryOlder(bits, dayIndex - anchorDay);
    anchorDay = dayIndex;
  }
  if (anchorDay >= dayIndex) {
    const uint32_t delta = anchorDay - dayIndex;
    if (delta < READING_HISTORY_DAYS) setBit(bits, static_cast<size_t>(delta));
  }
}

void recordReadingSpanIntoHistory(uint32_t& anchorDay, std::array<uint8_t, READING_HISTORY_BYTES>& bits,
                                  const ReadingStatsDateTime& localStart, const uint32_t seconds) {
  if (!localStart.isValid() || seconds == 0) return;
  ReadingStatsDateTime cursor = localStart;
  uint32_t remaining = seconds;
  while (remaining > 0) {
    markReadingHistoryDay(anchorDay, bits, readingStatsDayIndex(cursor.date));
    const uint32_t secondOfDay =
        static_cast<uint32_t>(cursor.hour) * 3600u + static_cast<uint32_t>(cursor.minute) * 60u + cursor.second;
    const uint32_t segment = std::min(remaining, 24u * 3600u - secondOfDay);
    remaining -= segment;
    const uint32_t previousDay = readingStatsDayIndex(cursor.date);
    addSecondsToReadingStatsDateTime(cursor, segment);
    if (remaining > 0 && previousDay == readingStatsDayIndex(cursor.date) &&
        secondOfDay ==
            static_cast<uint32_t>(cursor.hour) * 3600u + static_cast<uint32_t>(cursor.minute) * 60u + cursor.second) {
      break;  // The supported calendar ends at 2099-12-31.
    }
  }
}

void mergeReadingHistory(uint32_t& targetAnchorDay, std::array<uint8_t, READING_HISTORY_BYTES>& targetBits,
                         const uint32_t sourceAnchorDay, const std::array<uint8_t, READING_HISTORY_BYTES>& sourceBits) {
  if (sourceAnchorDay == 0 && !isBitSet(sourceBits, 0)) return;
  if (targetAnchorDay == 0 && !isBitSet(targetBits, 0)) {
    targetAnchorDay = sourceAnchorDay;
    targetBits = sourceBits;
    return;
  }
  if (sourceAnchorDay > targetAnchorDay) {
    shiftHistoryOlder(targetBits, sourceAnchorDay - targetAnchorDay);
    targetAnchorDay = sourceAnchorDay;
  }
  for (size_t bitIndex = 0; bitIndex < READING_HISTORY_DAYS && bitIndex <= sourceAnchorDay; ++bitIndex) {
    if (!isBitSet(sourceBits, bitIndex)) continue;
    const uint32_t dayIndex = sourceAnchorDay - static_cast<uint32_t>(bitIndex);
    if (dayIndex > targetAnchorDay) continue;
    const uint32_t delta = targetAnchorDay - dayIndex;
    if (delta < READING_HISTORY_DAYS) setBit(targetBits, static_cast<size_t>(delta));
  }
}

uint16_t computeReadingHistoryLongestStreak(const uint32_t anchorDay,
                                            const std::array<uint8_t, READING_HISTORY_BYTES>& bits) {
  if (anchorDay == 0 && !isBitSet(bits, 0)) return 0;
  uint16_t best = 0;
  uint16_t current = 0;
  for (int bitIndex = static_cast<int>(READING_HISTORY_DAYS) - 1; bitIndex >= 0; --bitIndex) {
    if (isBitSet(bits, static_cast<size_t>(bitIndex))) {
      best = std::max(best, ++current);
    } else {
      current = 0;
    }
  }
  return best;
}

uint16_t computeReadingHistoryCurrentStreak(const uint32_t anchorDay,
                                            const std::array<uint8_t, READING_HISTORY_BYTES>& bits,
                                            const ReadingStatsDate* today) {
  if ((anchorDay == 0 && !isBitSet(bits, 0)) || !isBitSet(bits, 0)) return 0;
  if (today && today->isValid()) {
    const uint32_t todayDay = readingStatsDayIndex(*today);
    if (todayDay < anchorDay || todayDay > anchorDay + 1u) return 0;
  }
  uint16_t streak = 0;
  while (streak < READING_HISTORY_DAYS && isBitSet(bits, streak)) ++streak;
  return streak;
}
