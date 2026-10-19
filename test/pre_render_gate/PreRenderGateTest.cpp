// PreRenderGate: the free heap the next page's pre-render asks for, from what the passes measured.
#include <gtest/gtest.h>

#include "activities/reader/PreRenderGate.h"

namespace {

using PreRenderGate::kTroughBytes;
using PreRenderGate::minFreeHeap;

TEST(PreRenderGate, UntilAPassIsMeasuredTheFloorIsTheFixedOneItReplaced) {
  EXPECT_EQ(34u * 1024, minFreeHeap(0));  // 21 KB trough + 13 KB, half again the X4's 8.5 KB
}

TEST(PreRenderGate, AMeasuredCostKeepsTheTroughAndHalfAgainOnTop) {
  EXPECT_EQ(kTroughBytes + 9000u, minFreeHeap(6000));
  EXPECT_EQ(kTroughBytes + 1u, minFreeHeap(1));  // rounded down, never below the trough
  // The X3's measured passes: four TrueType families at 6.4 KB come in under the old 34 KB floor, built-in Bookerly's
  // 9.9 KB prewarm above it.
  EXPECT_LT(minFreeHeap(6428), 34u * 1024);
  EXPECT_GT(minFreeHeap(9876), 34u * 1024);
}

TEST(PreRenderGate, AnExpectedMissAsksForWhatAPassWithMissesCost) {
  // Four TrueType families in SD mode on the X3: 4.6 KB a pass from the store, 20.9 KB one that drew its glyphs.
  EXPECT_EQ(kTroughBytes + 4600u + 2300u, minFreeHeap(4600));
  EXPECT_EQ(kTroughBytes + 20900u, minFreeHeap(4600, 20900));  // no half again: that memory does not vary
  EXPECT_EQ(minFreeHeap(4600), minFreeHeap(4600, 0));          // none expected
  EXPECT_EQ(minFreeHeap(9876), minFreeHeap(9876, 9000));       // never below the floor without misses
  EXPECT_EQ(kTroughBytes + 26000u, minFreeHeap(0, 26000));     // measured before any pass without misses
}

TEST(PreRenderGate, ACostlierPassRaisesTheFloor) {
  uint32_t previous = minFreeHeap(1);
  for (uint32_t cost = 512; cost <= 32 * 1024; cost += 512) {
    EXPECT_GT(minFreeHeap(cost), previous) << cost;
    previous = minFreeHeap(cost);
  }
}

}  // namespace
