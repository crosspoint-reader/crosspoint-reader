#pragma once

#include <cstdint>
#include <vector>

// Single source of truth for the river-aware remainder allocator used by EPUB
// full-justification layout. Baseline justifyExtra = spareSpace / gaps is applied
// elsewhere; this helper distributes only the leftover pixels
// (spareSpace % gaps) across existing gap slots, preferring the candidate gap
// whose center X is farthest from the nearest previous-line gap center.
// Ties break first on lower current bonus, then stably left-to-right.
// Both the runtime EPUB layout path (ParsedText.cpp) and the host allocator
// test link against this same implementation so behavior stays in one place.
std::vector<int> allocateJustifyRemainderBonuses(const std::vector<int16_t>& previousLineGapCenters,
                                                 const std::vector<int16_t>& candidateGapCenters, int justifyRemainder);
