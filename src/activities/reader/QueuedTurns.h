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

// Turns to keep for a later pass, out of those not applied in this one and those held back. They
// are kept when the pass ran out of chapter (the next one has to load first) or applied all it was
// given. A turn that failed, such as one before the first page, drops what was queued behind it.
constexpr int queuedTurnsToCarry(const int unapplied, const int held, const bool leftTheChapter) {
  return (unapplied == 0 || leftTheChapter) ? unapplied + held : 0;
}

// After a turn that left the chapter, how long queued turns may sit with no chapter loaded. The
// render task normally takes its lock within milliseconds and then covers the whole load.
constexpr unsigned long kQueuedTurnsSectionWaitMs = 1000;

constexpr bool queuedTurnsAwaitSection(const unsigned long msSinceTurn) {
  return msSinceTurn < kQueuedTurnsSectionWaitMs;
}
