// Host-side litmus for LookupChain — the compact back-navigation stack.
//
// Verifies the two load-bearing invariants (distance-from-newest addressing;
// non-contiguous-subset under back-then-forward) against the design's worked
// example, plus eviction/depth-cap. Pure logic — no SD, no settings.
//
// Build/run: test/lookup-chain/run.sh

#include <cstdio>

#include "util/LookupChain.h"

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond, msg)                                              \
  do {                                                                \
    ++g_checks;                                                       \
    if (!(cond)) {                                                    \
      ++g_failures;                                                   \
      std::printf("  FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); \
    }                                                                 \
  } while (0)

// The design's worked example: A→B→C, back to B, forward to D.
// Expected log [A,B,C,D]; stack [A@3, B@2], skipping the abandoned gap C@1.
static void testWorkedTraceNonContiguousSubset() {
  std::printf("testWorkedTraceNonContiguousSubset\n");
  LookupChain c;
  c.reset(100);
  c.setCurrentHistIndex(0);  // viewing A, A is newest in the log

  // A -> B (B appended)
  c.onForward(/*page=*/0, /*appended=*/true);
  CHECK(c.depth() == 1, "after A->B: one entry");
  CHECK(c.at(0).histIndex == 1, "A at distance 1");
  CHECK(c.currentHistIndex() == 0, "B is newest");

  // B -> C (C appended)
  c.onForward(0, true);
  CHECK(c.depth() == 2, "after B->C: two entries");
  CHECK(c.at(0).histIndex == 2, "A at distance 2");
  CHECK(c.at(1).histIndex == 1, "B at distance 1");

  // back to B
  LookupChain::Entry popped = c.pop();
  CHECK(popped.histIndex == 1, "popped B@1");
  CHECK(c.depth() == 1, "back leaves one entry");
  CHECK(c.at(0).histIndex == 2, "A still at distance 2");
  CHECK(c.currentHistIndex() == 1, "now viewing B at distance 1");

  // B -> D (D appended) — the abandoned C stays in the log as a gap (C@1)
  c.onForward(0, true);
  CHECK(c.depth() == 2, "after B->D: two entries");
  CHECK(c.at(0).histIndex == 3, "A@3 (log [A,B,C,D])");
  CHECK(c.at(1).histIndex == 2, "B@2, skipping gap C@1");
  CHECK(c.currentHistIndex() == 0, "D is newest");
}

// Indices must address from the newest end so they survive front-eviction, and
// entries whose word leaves the capped log must be dropped (always-resolvable).
static void testEvictionDropsBottom() {
  std::printf("testEvictionDropsBottom\n");
  LookupChain c;
  c.reset(3);                // cap 3
  c.setCurrentHistIndex(0);  // viewing A, log [A]

  c.onForward(0, true);  // A->B: [A@1], log [A,B]
  c.onForward(0, true);  // B->C: [A@2,B@1], log [A,B,C] (full)
  CHECK(c.depth() == 2, "two entries before overflow");

  c.onForward(0, true);  // C->D: A would be evicted from log -> dropped from chain
  CHECK(c.depth() == 2, "depth stays bounded at eviction");
  CHECK(c.at(0).histIndex == 2, "B@2 now the bottom (A evicted)");
  CHECK(c.at(1).histIndex == 1, "C@1");
  CHECK(c.currentHistIndex() == 0, "D newest");
}

// Page is carried through forward->back.
static void testPageRoundTrip() {
  std::printf("testPageRoundTrip\n");
  LookupChain c;
  c.reset(100);
  c.setCurrentHistIndex(0);
  c.onForward(/*page=*/7, true);  // left a word on page 7
  LookupChain::Entry e = c.pop();
  CHECK(e.page == 7, "page restored");
}

// Current word not in the history log (-1) cannot be referenced: no entry pushed.
static void testUnloggedCurrentNotPushed() {
  std::printf("testUnloggedCurrentNotPushed\n");
  LookupChain c;
  c.reset(100);
  c.setCurrentHistIndex(-1);  // current word not in log
  c.onForward(0, true);       // can't reference it -> nothing pushed
  CHECK(c.empty(), "no entry pushed for an unreferenceable word");
  CHECK(c.currentHistIndex() == 0, "new word is newest");
}

int main() {
  std::printf("=== LookupChain host litmus ===\n");
  testWorkedTraceNonContiguousSubset();
  testEvictionDropsBottom();
  testPageRoundTrip();
  testUnloggedCurrentNotPushed();
  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
