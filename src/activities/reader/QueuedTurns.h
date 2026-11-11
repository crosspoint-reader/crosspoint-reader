#pragma once

#include <algorithm>
#include <cstdint>

// Page-turn presses that arrive while the panel is busy are counted, up to four either way, and
// applied together once it is free.
constexpr int8_t kMaxQueuedTurns = 4;

constexpr int8_t queueTurn(const int8_t pending, const bool forward) {
  return static_cast<int8_t>(std::clamp(pending + (forward ? 1 : -1), -kMaxQueuedTurns, +kMaxQueuedTurns));
}

// Queued forward turns: those to apply now, and those to hold until the page has been drawn.
struct QueuedForwardTurns {
  int now;
  int held;
};

// A chapter still being laid out may end before the pages past pageCount exist. One turn may step
// onto the first of them, as a single press does: the render then builds up to it, or settles on
// the last page if the chapter ended there. The rest wait for that render, so they carry on from
// where the chapter really ends instead of being spent on pages that never appear.
constexpr QueuedForwardTurns splitQueuedForwardTurns(const int queued, const bool building, const int currentPage,
                                                     const int pageCount) {
  if (!building) return {queued, 0};
  const int onBuiltPages = std::max(0, pageCount - 1 - currentPage);
  const int now = std::min(queued, onBuiltPages + 1);
  return {now, queued - now};
}
