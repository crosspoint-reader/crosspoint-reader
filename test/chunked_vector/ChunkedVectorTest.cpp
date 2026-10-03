#include <gtest/gtest.h>

#include <string>
#include <utility>

#include "ChunkedVector.h"

namespace {

template <typename V>
void fillAndCheck(V& v, const size_t count) {
  for (size_t i = 0; i < count; ++i) {
    ASSERT_TRUE(v.push_back(static_cast<int>(i))) << "at " << i;
  }
  ASSERT_EQ(v.size(), count);
  for (size_t i = 0; i < count; ++i) {
    EXPECT_EQ(v[i], static_cast<int>(i)) << "at " << i;
  }
  size_t i = 0;
  for (const int x : v) {
    EXPECT_EQ(x, static_cast<int>(i++));
  }
  EXPECT_EQ(i, count);
}

}  // namespace

TEST(ChunkedVectorTest, GrowingChunksHoldEveryIndexUpToCapacity) {
  // Chunks of 4, 8, 16, then 16 each: 28 + 3 * 16.
  using V = ChunkedVector<int, 4, 16, 6>;
  static_assert(V::maxSize() == 76);
  V v;
  fillAndCheck(v, V::maxSize());
  EXPECT_FALSE(v.push_back(-1));
  EXPECT_EQ(v.size(), V::maxSize());
  EXPECT_EQ(v.back(), static_cast<int>(V::maxSize() - 1));
}

TEST(ChunkedVectorTest, FixedChunksWhenFirstEqualsMax) {
  using V = ChunkedVector<int, 8, 8, 3>;
  static_assert(V::maxSize() == 24);
  V v;
  fillAndCheck(v, V::maxSize());
  EXPECT_FALSE(v.push_back(-1));
}

TEST(ChunkedVectorTest, ReaderInstantiationsCoverTheirCaps) {
  static_assert(ChunkedVector<int, 16, 128, 131>::maxSize() >= 16384);
  static_assert(ChunkedVector<std::pair<std::string, uint16_t>, 4, 64, 23>::maxSize() >= 1024 + 128);
  ChunkedVector<int, 16, 128, 131> lut;
  fillAndCheck(lut, 3000);
}

TEST(ChunkedVectorTest, MovesNonTrivialValues) {
  ChunkedVector<std::pair<std::string, uint16_t>, 4, 64, 23> anchors;
  for (int i = 0; i < 200; ++i) {
    ASSERT_TRUE(anchors.push_back({"anchor-with-a-long-enough-id-" + std::to_string(i), static_cast<uint16_t>(i)}));
  }
  for (int i = 0; i < 200; ++i) {
    EXPECT_EQ(anchors[i].first, "anchor-with-a-long-enough-id-" + std::to_string(i));
    EXPECT_EQ(anchors[i].second, i);
  }
}
