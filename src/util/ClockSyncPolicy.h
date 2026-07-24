#pragma once

namespace ClockSyncPolicy {

inline bool shouldSyncFromNetwork(const bool syncedBefore, const bool systemTimeValid) {
  return !syncedBefore || !systemTimeValid;
}

}  // namespace ClockSyncPolicy
