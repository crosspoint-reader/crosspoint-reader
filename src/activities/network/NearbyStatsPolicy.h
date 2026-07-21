#pragma once

#include "activities/reader/GlobalReadingStats.h"

namespace NearbyStatsPolicy {

// A peer snapshot is a cumulative record for one device. Replacing it is safe
// only when every cumulative counter is at least as large as the saved copy.
// The rolling history window is intentionally excluded because it advances as
// days pass and therefore is not monotonic.
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
  return true;
}

}  // namespace NearbyStatsPolicy
