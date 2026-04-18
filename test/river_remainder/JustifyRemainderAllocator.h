#pragma once

#include <cstdint>
#include <vector>

// CrossPoint's current EPUB justification path computes:
//   justifyExtra = spareSpace / actualGapCount
// and drops:
//   spareSpace % actualGapCount
//
// This host-side helper is prep work for a future river-aware heuristic that
// distributes those remainder pixels across existing gaps without changing line
// breaking or runtime behavior yet.
std::vector<int> allocateJustifyRemainderBonuses(const std::vector<int16_t>& previousLineGapCenters,
                                                 const std::vector<int16_t>& candidateGapCenters,
                                                 int justifyRemainder);
