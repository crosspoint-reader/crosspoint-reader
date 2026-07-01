// Host-side litmus for LookupChain — the compact back-navigation stack.
//
// Verifies the two load-bearing invariants (distance-from-newest addressing;
// non-contiguous-subset under back-then-forward) against the design's worked
// example, plus eviction/depth-cap. Pure logic — no SD, no settings.
//

#include <gtest/gtest.h>

#include "util/LookupChain.h"

// The design's worked example: A→B→C, back to B, forward to D.
// Expected log [A,B,C,D]; stack [A@3, B@2], skipping the abandoned gap C@1.
TEST(LookupChain, WorkedTraceNonContiguousSubset) {
  LookupChain c;
  c.reset(100);
  c.setCurrentHistIndex(0);  // viewing A, A is newest in the log

  // A -> B (B appended)
  c.onForward(/*page=*/0, /*appended=*/true);
  EXPECT_EQ(c.depth(), 1) << "after A->B: one entry";
  EXPECT_EQ(c.at(0).histIndex, 1) << "A at distance 1";
  EXPECT_EQ(c.currentHistIndex(), 0) << "B is newest";

  // B -> C (C appended)
  c.onForward(0, true);
  EXPECT_EQ(c.depth(), 2) << "after B->C: two entries";
  EXPECT_EQ(c.at(0).histIndex, 2) << "A at distance 2";
  EXPECT_EQ(c.at(1).histIndex, 1) << "B at distance 1";

  // back to B
  LookupChain::Entry popped = c.pop();
  EXPECT_EQ(popped.histIndex, 1) << "popped B@1";
  EXPECT_EQ(c.depth(), 1) << "back leaves one entry";
  EXPECT_EQ(c.at(0).histIndex, 2) << "A still at distance 2";
  EXPECT_EQ(c.currentHistIndex(), 1) << "now viewing B at distance 1";

  // B -> D (D appended) — the abandoned C stays in the log as a gap (C@1)
  c.onForward(0, true);
  EXPECT_EQ(c.depth(), 2) << "after B->D: two entries";
  EXPECT_EQ(c.at(0).histIndex, 3) << "A@3 (log [A,B,C,D])";
  EXPECT_EQ(c.at(1).histIndex, 2) << "B@2, skipping gap C@1";
  EXPECT_EQ(c.currentHistIndex(), 0) << "D is newest";
}

// Indices must address from the newest end so they survive front-eviction, and
// entries whose word leaves the capped log must be dropped (always-resolvable).
TEST(LookupChain, EvictionDropsBottom) {
  LookupChain c;
  c.reset(3);                // cap 3
  c.setCurrentHistIndex(0);  // viewing A, log [A]

  c.onForward(0, true);  // A->B: [A@1], log [A,B]
  c.onForward(0, true);  // B->C: [A@2,B@1], log [A,B,C] (full)
  EXPECT_EQ(c.depth(), 2) << "two entries before overflow";

  c.onForward(0, true);  // C->D: A would be evicted from log -> dropped from chain
  EXPECT_EQ(c.depth(), 2) << "depth stays bounded at eviction";
  EXPECT_EQ(c.at(0).histIndex, 2) << "B@2 now the bottom (A evicted)";
  EXPECT_EQ(c.at(1).histIndex, 1) << "C@1";
  EXPECT_EQ(c.currentHistIndex(), 0) << "D newest";
}

// Page is carried through forward->back.
TEST(LookupChain, PageRoundTrip) {
  LookupChain c;
  c.reset(100);
  c.setCurrentHistIndex(0);
  c.onForward(/*page=*/7, true);  // left a word on page 7
  LookupChain::Entry e = c.pop();
  EXPECT_EQ(e.page, 7) << "page restored";
}

// Current word not in the history log (-1) cannot be referenced: no entry pushed.
TEST(LookupChain, UnloggedCurrentNotPushed) {
  LookupChain c;
  c.reset(100);
  c.setCurrentHistIndex(-1);  // current word not in log
  c.onForward(0, true);       // can't reference it -> nothing pushed
  EXPECT_TRUE(c.empty()) << "no entry pushed for an unreferenceable word";
  EXPECT_EQ(c.currentHistIndex(), 0) << "new word is newest";
}
