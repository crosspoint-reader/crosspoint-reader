#pragma once

#include <algorithm>

// The reader's link back-stack, kept free of reader state so the host tests can drive it.
//
// entries[0 .. depth-1] are the positions links were followed FROM, oldest first: entries[depth-1]
// is where the newest jump started, and Back returns there. A full stack drops its oldest entry, so
// the newest is always at entries[depth-1] and the one below it at entries[depth-2].
namespace link_stack {

struct Position {
  int spineIndex;
  int pageNumber;
};

inline bool samePage(const Position& a, const Position& b) {
  return a.spineIndex == b.spineIndex && a.pageNumber == b.pageNumber;
}

// Records `origin` as the newest entry, dropping the oldest one first when the stack is full.
inline void push(Position* entries, int& depth, const int capacity, const Position& origin) {
  if (depth >= capacity) {
    std::copy(entries + 1, entries + capacity, entries);
    depth = capacity - 1;
  }
  entries[depth++] = origin;
}

// The depth once the link just pushed as entries[depth-1] has landed on `landed`.
//
// Landing on a page any jump on the stack started from (entries[i], i < depth) is a return:
// typically a note's own back-link, followed like any other link. Back pressed (depth - i) times
// from the landing would have arrived at exactly entries[i] with depth i, so the stack is cut to
// that: entries[i] and every jump above it go, and Back next returns to where the jump INTO
// entries[i]'s page started. Intent does not matter: however the reader got there, keeping the
// entries would only make a later Back bounce to pages already left. That includes the entry just
// pushed when the link lands on the page it was tapped on: Back would only have stayed put. When
// the page appears more than once, the newest match wins, since that is the one repeated Back
// presses would stop at first. A landing on no recorded origin is a further jump and keeps the
// stack.
inline int depthAfterLanding(const Position* entries, const int depth, const Position& landed) {
  for (int i = depth - 1; i >= 0; i--) {
    if (samePage(entries[i], landed)) return i;
  }
  return depth;
}

}  // namespace link_stack
