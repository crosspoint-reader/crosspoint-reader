#include "ProgressComparison.h"

#include <cmath>

namespace {
constexpr float SAME_PROGRESS_EPSILON = 0.001f;

ProgressComparison compareOrdered(const uint32_t local, const uint32_t remote) {
  if (local > remote) return ProgressComparison::LocalAhead;
  if (local < remote) return ProgressComparison::RemoteAhead;
  return ProgressComparison::Synchronized;
}
}  // namespace

ProgressComparison compareProgress(const CrossPointPosition& local, const float localPercentage,
                                   const CrossPointPosition& remote, const float remotePercentage) {
  if (local.hasResolvedSpineIndex && remote.hasResolvedSpineIndex) {
    if (local.spineIndex != remote.spineIndex) {
      return local.spineIndex > remote.spineIndex ? ProgressComparison::LocalAhead : ProgressComparison::RemoteAhead;
    }

    if (local.hasMappedPage && remote.hasMappedPage && local.pageNumber == remote.pageNumber) {
      return ProgressComparison::Synchronized;
    }

    if (local.hasVisibleTextOffset && remote.hasVisibleTextOffset) {
      return compareOrdered(local.visibleTextOffset, remote.visibleTextOffset);
    }

    if (local.hasMappedPage && remote.hasMappedPage) {
      if (local.pageNumber > remote.pageNumber) return ProgressComparison::LocalAhead;
      if (local.pageNumber < remote.pageNumber) return ProgressComparison::RemoteAhead;
      return ProgressComparison::Synchronized;
    }
  }

  if (std::isfinite(localPercentage) && std::isfinite(remotePercentage)) {
    const float delta = localPercentage - remotePercentage;
    if (std::fabs(delta) <= SAME_PROGRESS_EPSILON) return ProgressComparison::Synchronized;
    return delta > 0.0f ? ProgressComparison::LocalAhead : ProgressComparison::RemoteAhead;
  }

  return ProgressComparison::Unknown;
}

RemoteRecordChoice selectRemoteRecord(const CrossPointPosition& primary, const float primaryPercentage,
                                      const CrossPointPosition& alternate, const float alternatePercentage) {
  return compareProgress(primary, primaryPercentage, alternate, alternatePercentage) == ProgressComparison::RemoteAhead
             ? RemoteRecordChoice::Alternate
             : RemoteRecordChoice::Primary;
}
