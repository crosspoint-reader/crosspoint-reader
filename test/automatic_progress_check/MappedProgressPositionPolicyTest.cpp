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

TEST(MappedProgressPositionPolicy, TreatsOnePageDriftAsSamePosition) {
  // The resume path can roll back by one page at a page boundary; a one-page
  // difference must not read as "ahead".
  EXPECT_EQ(MappedProgressPositionPolicy::compare(10, 41, 10, 42), MappedProgressPositionOrder::SAME_PAGE);
  EXPECT_EQ(MappedProgressPositionPolicy::compare(10, 42, 10, 41), MappedProgressPositionOrder::SAME_PAGE);
  // Two pages is a real difference.
  EXPECT_EQ(MappedProgressPositionPolicy::compare(10, 40, 10, 42), MappedProgressPositionOrder::REMOTE_AHEAD);
  EXPECT_EQ(MappedProgressPositionPolicy::compare(10, 42, 10, 40), MappedProgressPositionOrder::LOCAL_AHEAD);
}

TEST(MappedProgressPositionPolicy, RejectsInvalidPositions) {
  EXPECT_EQ(MappedProgressPositionPolicy::compare(-1, 0, 0, 0), MappedProgressPositionOrder::INVALID);
  EXPECT_EQ(MappedProgressPositionPolicy::compare(0, -1, 0, 0), MappedProgressPositionOrder::INVALID);
  EXPECT_EQ(MappedProgressPositionPolicy::compare(0, 0, -1, 0), MappedProgressPositionOrder::INVALID);
  EXPECT_EQ(MappedProgressPositionPolicy::compare(0, 0, 0, -1), MappedProgressPositionOrder::INVALID);
}
