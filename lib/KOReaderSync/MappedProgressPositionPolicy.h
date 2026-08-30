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
  // Positions whose content offsets differ by at most this many visible
  // characters are treated as the same spot. Resolving a remote XPath lands a
  // few characters off the locally-tracked offset; without this margin that
  // sub-page drift reads as "one page ahead" and triggers a spurious prompt.
  static constexpr int64_t CONTENT_TOLERANCE_CHARS = 200;

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

  // Content-aware comparison. When both sides carry a visible-text offset (the
  // authoritative position), compare those with CONTENT_TOLERANCE_CHARS of
  // slack instead of layout-dependent page numbers; otherwise fall back to the
  // strict page compare above.
  static MappedProgressPositionOrder compareContent(const int localSpineIndex, const int localPage,
                                                    const uint32_t localOffset, const bool localHasOffset,
                                                    const int remoteSpineIndex, const int remotePage,
                                                    const uint32_t remoteOffset, const bool remoteHasOffset) {
    if (localHasOffset && remoteHasOffset) {
      if (localSpineIndex != remoteSpineIndex) {
        return localSpineIndex > remoteSpineIndex ? MappedProgressPositionOrder::LOCAL_AHEAD
                                                  : MappedProgressPositionOrder::REMOTE_AHEAD;
      }
      const int64_t delta = static_cast<int64_t>(localOffset) - static_cast<int64_t>(remoteOffset);
      if (delta > CONTENT_TOLERANCE_CHARS) {
        return MappedProgressPositionOrder::LOCAL_AHEAD;
      }
      if (delta < -CONTENT_TOLERANCE_CHARS) {
        return MappedProgressPositionOrder::REMOTE_AHEAD;
      }
      return MappedProgressPositionOrder::SAME_PAGE;
    }
    return compare(localSpineIndex, localPage, remoteSpineIndex, remotePage);
  }
};
