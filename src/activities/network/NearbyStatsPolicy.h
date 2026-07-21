#pragma once

#include "activities/reader/GlobalReadingStats.h"

namespace NearbyStatsPolicy {

// A peer snapshot is a cumulative record for one device. Replacing it is safe
// only when every cumulative counter is at least as large as the saved copy.
// The rolling history may advance to a newer anchor, but an equal-anchor
// replacement must retain every known day and must never erase an initialized
// window by replacing it with an empty anchor.
inline bool doesNotRegress(const GlobalReadingStats& incoming, const GlobalReadingStats& existing) {
  if (incoming.totalSessions < existing.totalSessions || incoming.totalReadingSeconds < existing.totalReadingSeconds ||
      incoming.totalPagesTurned < existing.totalPagesTurned || incoming.completedBooks < existing.completedBooks ||
      incoming.longestReadingStreak < existing.longestReadingStreak) {
    return false;
  }
  for (size_t index = 0; index < incoming.timeOfDaySeconds.size(); ++index) {
    if (incoming.timeOfDaySeconds[index] < existing.timeOfDaySeconds[index]) return false;
  }
  for (size_t index = 0; index < incoming.dayOfWeekSeconds.size(); ++index) {
    if (incoming.dayOfWeekSeconds[index] < existing.dayOfWeekSeconds[index]) return false;
  }
  const auto bitIsSet = [](const auto& bits, const size_t index) {
    return index < READING_HISTORY_DAYS && (bits[index / 8] & static_cast<uint8_t>(1u << (index % 8))) != 0;
  };
  const bool existingHistoryInitialized =
      existing.readingHistoryAnchorDay != 0 || bitIsSet(existing.readingHistoryBits, 0);
  const bool incomingHistoryInitialized =
      incoming.readingHistoryAnchorDay != 0 || bitIsSet(incoming.readingHistoryBits, 0);
  if (existingHistoryInitialized) {
    if (!incomingHistoryInitialized || incoming.readingHistoryAnchorDay < existing.readingHistoryAnchorDay) {
      return false;
    }
    const uint32_t shift = incoming.readingHistoryAnchorDay - existing.readingHistoryAnchorDay;
    // The history engine deliberately rejects such a distant re-anchor as an
    // untrusted clock jump, so Nearby must not accept it as a replacement.
    if (shift >= READING_HISTORY_DAYS) return false;
    for (size_t oldIndex = 0; oldIndex + shift < READING_HISTORY_DAYS; ++oldIndex) {
      if (bitIsSet(existing.readingHistoryBits, oldIndex) && !bitIsSet(incoming.readingHistoryBits, oldIndex + shift)) {
        return false;
      }
    }
  }
  return true;
}

}  // namespace NearbyStatsPolicy
