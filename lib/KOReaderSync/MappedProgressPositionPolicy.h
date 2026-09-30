#pragma once

#include <cstdint>

enum class MappedProgressPositionOrder : uint8_t {
  INVALID,
  LOCAL_AHEAD,
  SAME_PAGE,
  REMOTE_AHEAD,
};

class MappedProgressPositionPolicy {
 public:
  // Positions on the same spine and within one page of each other are treated
  // as the same reading spot. The resume path can roll back by one page at a
  // page boundary (the same physical offset restores to the previous page), so
  // a strict page compare would flag that rollback as "remote ahead" and show
  // a spurious prompt.
  static MappedProgressPositionOrder compare(const int localSpineIndex, const int localPage, const int remoteSpineIndex,
                                             const int remotePage) {
    if (localSpineIndex < 0 || localPage < 0 || remoteSpineIndex < 0 || remotePage < 0) {
      return MappedProgressPositionOrder::INVALID;
    }
    if (localSpineIndex != remoteSpineIndex) {
      return localSpineIndex > remoteSpineIndex ? MappedProgressPositionOrder::LOCAL_AHEAD
                                                : MappedProgressPositionOrder::REMOTE_AHEAD;
    }
    const int pageDelta = localPage - remotePage;
    if (pageDelta >= -1 && pageDelta <= 1) {
      return MappedProgressPositionOrder::SAME_PAGE;
    }
    return pageDelta > 0 ? MappedProgressPositionOrder::LOCAL_AHEAD : MappedProgressPositionOrder::REMOTE_AHEAD;
  }
};
