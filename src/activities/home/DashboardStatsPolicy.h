#pragma once

#include <cstdint>

// Dashboard data is prepared before rendering. These states keep a verified
// zero distinct from a missing file, and both distinct from unreadable data.
enum class DashboardMetricState : uint8_t { Available, NoData, Unavailable, NotTracked };

struct DashboardStatsPolicyInput {
  bool isEpub = false;
  bool epubVerified = false;
  bool bookStatsTrusted = false;
  bool bookStatsMissing = false;
  bool localStatsTrusted = false;
  bool localStatsMissing = false;
  bool hasSyncedDirectory = false;
  bool syncedScanComplete = false;
  uint16_t validPeerCount = 0;
  uint16_t skippedPeerCount = 0;
};

struct DashboardStatsPolicyResult {
  DashboardMetricState bookStats = DashboardMetricState::NotTracked;
  DashboardMetricState globalStats = DashboardMetricState::Unavailable;
  DashboardMetricState syncedStats = DashboardMetricState::NoData;
  bool useAllSynced = false;
};

namespace DashboardStatsPolicy {

constexpr DashboardMetricState fromLoadStatus(const bool trusted, const bool missing) {
  if (!trusted) return DashboardMetricState::Unavailable;
  return missing ? DashboardMetricState::NoData : DashboardMetricState::Available;
}

constexpr DashboardStatsPolicyResult evaluate(const DashboardStatsPolicyInput& input) {
  DashboardStatsPolicyResult result;
  if (input.isEpub) {
    result.bookStats = input.epubVerified ? fromLoadStatus(input.bookStatsTrusted, input.bookStatsMissing)
                                          : DashboardMetricState::Unavailable;
  }

  const DashboardMetricState localStats = fromLoadStatus(input.localStatsTrusted, input.localStatsMissing);
  const bool cleanSyncedAggregate = input.hasSyncedDirectory && input.localStatsTrusted && input.syncedScanComplete &&
                                    input.skippedPeerCount == 0 && input.validPeerCount > 0;
  result.useAllSynced = cleanSyncedAggregate;
  result.globalStats = cleanSyncedAggregate ? DashboardMetricState::Available : localStats;

  if (!input.hasSyncedDirectory) {
    result.syncedStats = DashboardMetricState::NoData;
  } else if (cleanSyncedAggregate) {
    result.syncedStats = DashboardMetricState::Available;
  } else if (!input.localStatsTrusted || !input.syncedScanComplete || input.skippedPeerCount > 0) {
    result.syncedStats = DashboardMetricState::Unavailable;
  } else {
    result.syncedStats = DashboardMetricState::NoData;
  }
  return result;
}

}  // namespace DashboardStatsPolicy
