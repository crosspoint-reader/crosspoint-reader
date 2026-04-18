#include "JustifyRemainderAllocator.h"

#include <cstdlib>
#include <limits>

namespace {

int nearestPreviousGapDistance(const std::vector<int16_t>& previousLineGapCenters, const int16_t candidateCenter) {
  if (previousLineGapCenters.empty()) {
    return std::numeric_limits<int>::max();
  }

  int bestDistance = std::numeric_limits<int>::max();
  for (const int16_t previousCenter : previousLineGapCenters) {
    const int distance = std::abs(static_cast<int>(candidateCenter) - static_cast<int>(previousCenter));
    if (distance < bestDistance) {
      bestDistance = distance;
    }
  }

  return bestDistance;
}

}  // namespace

std::vector<int> allocateJustifyRemainderBonuses(const std::vector<int16_t>& previousLineGapCenters,
                                                 const std::vector<int16_t>& candidateGapCenters,
                                                 const int justifyRemainder) {
  std::vector<int> gapBonuses(candidateGapCenters.size(), 0);
  if (justifyRemainder <= 0 || candidateGapCenters.empty()) {
    return gapBonuses;
  }

  for (int pixel = 0; pixel < justifyRemainder; ++pixel) {
    size_t bestIndex = 0;
    int bestDistance = std::numeric_limits<int>::min();

    for (size_t i = 0; i < candidateGapCenters.size(); ++i) {
      const int candidateDistance = nearestPreviousGapDistance(previousLineGapCenters, candidateGapCenters[i]);

      if (candidateDistance > bestDistance) {
        bestDistance = candidateDistance;
        bestIndex = i;
        continue;
      }

      if (candidateDistance != bestDistance) {
        continue;
      }

      if (gapBonuses[i] < gapBonuses[bestIndex]) {
        bestIndex = i;
      }
    }

    gapBonuses[bestIndex]++;
  }

  return gapBonuses;
}
