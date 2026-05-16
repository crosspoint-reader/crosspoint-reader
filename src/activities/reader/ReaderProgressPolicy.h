#pragma once

#include <cstdint>
#include <limits>

namespace ReaderProgressPolicy {

struct ResumeRemapDecision {
  bool deferUntilComplete = false;
  bool applyPage = false;
  int pageNumber = 0;
};

inline constexpr uint16_t persistablePageCount(const bool sectionComplete, const uint32_t finalPageCount) {
  if (!sectionComplete || finalPageCount == 0) {
    return 0;
  }
  if (finalPageCount > std::numeric_limits<uint16_t>::max()) {
    return std::numeric_limits<uint16_t>::max();
  }
  return static_cast<uint16_t>(finalPageCount);
}

inline ResumeRemapDecision decideResumeRemap(const int currentSpineIndex, const int cachedSpineIndex,
                                             const int currentPage, const uint32_t cachedPageCount,
                                             const bool sectionComplete, const uint32_t finalPageCount) {
  ResumeRemapDecision decision;
  decision.pageNumber = currentPage;

  if (cachedPageCount == 0 || currentSpineIndex != cachedSpineIndex) {
    return decision;
  }

  if (!sectionComplete) {
    decision.deferUntilComplete = true;
    return decision;
  }

  if (finalPageCount == 0 || finalPageCount == cachedPageCount) {
    return decision;
  }

  const float progress = static_cast<float>(currentPage) / static_cast<float>(cachedPageCount);
  int remappedPage = static_cast<int>(progress * static_cast<float>(finalPageCount));
  if (remappedPage < 0) {
    remappedPage = 0;
  } else if (remappedPage >= static_cast<int>(finalPageCount)) {
    remappedPage = static_cast<int>(finalPageCount - 1);
  }

  decision.applyPage = true;
  decision.pageNumber = remappedPage;
  return decision;
}

}  // namespace ReaderProgressPolicy
