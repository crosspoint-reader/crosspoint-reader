#include <gtest/gtest.h>

#include "MappedProgressPositionPolicy.h"

TEST(MappedProgressPositionPolicy, ComparesSpineBeforePage) {
  EXPECT_EQ(MappedProgressPositionPolicy::compare(10, 0, 9, 99), MappedProgressPositionOrder::LOCAL_AHEAD);
  EXPECT_EQ(MappedProgressPositionPolicy::compare(9, 99, 10, 0), MappedProgressPositionOrder::REMOTE_AHEAD);
}

TEST(MappedProgressPositionPolicy, ComparesPagesWithinSameSpine) {
  EXPECT_EQ(MappedProgressPositionPolicy::compare(10, 41, 10, 19), MappedProgressPositionOrder::LOCAL_AHEAD);
  EXPECT_EQ(MappedProgressPositionPolicy::compare(10, 19, 10, 41), MappedProgressPositionOrder::REMOTE_AHEAD);
  EXPECT_EQ(MappedProgressPositionPolicy::compare(10, 41, 10, 41), MappedProgressPositionOrder::SAME_PAGE);
}

TEST(MappedProgressPositionPolicy, RejectsInvalidPositions) {
  EXPECT_EQ(MappedProgressPositionPolicy::compare(-1, 0, 0, 0), MappedProgressPositionOrder::INVALID);
  EXPECT_EQ(MappedProgressPositionPolicy::compare(0, -1, 0, 0), MappedProgressPositionOrder::INVALID);
  EXPECT_EQ(MappedProgressPositionPolicy::compare(0, 0, -1, 0), MappedProgressPositionOrder::INVALID);
  EXPECT_EQ(MappedProgressPositionPolicy::compare(0, 0, 0, -1), MappedProgressPositionOrder::INVALID);
}

TEST(MappedProgressPositionPolicy, ContentOffsetToleratesSubPageDrift) {
  // Same spine, offsets within tolerance -> same spot even if pages differ.
  EXPECT_EQ(MappedProgressPositionPolicy::compareContent(10, 41, 1000, true, 10, 42, 1050, true),
            MappedProgressPositionOrder::SAME_PAGE);
  // Offset clearly ahead -> local ahead.
  EXPECT_EQ(MappedProgressPositionPolicy::compareContent(10, 41, 3000, true, 10, 40, 1000, true),
            MappedProgressPositionOrder::LOCAL_AHEAD);
  // Offset clearly behind -> remote ahead.
  EXPECT_EQ(MappedProgressPositionPolicy::compareContent(10, 41, 1000, true, 10, 42, 3000, true),
            MappedProgressPositionOrder::REMOTE_AHEAD);
  // Different spine still wins regardless of offset.
  EXPECT_EQ(MappedProgressPositionPolicy::compareContent(11, 0, 0, true, 10, 99, 99999, true),
            MappedProgressPositionOrder::LOCAL_AHEAD);
}

TEST(MappedProgressPositionPolicy, ContentOffsetFallsBackToPagesWithoutOffsets) {
  // No offsets -> strict page compare.
  EXPECT_EQ(MappedProgressPositionPolicy::compareContent(10, 41, 0, false, 10, 42, 0, false),
            MappedProgressPositionOrder::REMOTE_AHEAD);
  EXPECT_EQ(MappedProgressPositionPolicy::compareContent(10, 41, 0, false, 10, 41, 0, false),
            MappedProgressPositionOrder::SAME_PAGE);
}
