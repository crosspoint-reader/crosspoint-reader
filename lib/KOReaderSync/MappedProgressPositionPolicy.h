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
  static MappedProgressPositionOrder compare(const int localSpineIndex, const int localPage, const int remoteSpineIndex,
                                             const int remotePage) {
    if (localSpineIndex < 0 || localPage < 0 || remoteSpineIndex < 0 || remotePage < 0) {
      return MappedProgressPositionOrder::INVALID;
    }
    if (localSpineIndex != remoteSpineIndex) {
      return localSpineIndex > remoteSpineIndex ? MappedProgressPositionOrder::LOCAL_AHEAD
                                                : MappedProgressPositionOrder::REMOTE_AHEAD;
    }
    if (localPage != remotePage) {
      return localPage > remotePage ? MappedProgressPositionOrder::LOCAL_AHEAD
                                    : MappedProgressPositionOrder::REMOTE_AHEAD;
    }
    return MappedProgressPositionOrder::SAME_PAGE;
  }
};
