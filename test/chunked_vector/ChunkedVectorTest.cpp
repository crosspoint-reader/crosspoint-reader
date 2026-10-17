// ChunkedVector (lib/Memory): the non-relocating table behind the section build's per-page LUTs.
// Growth across many chunks, the reserve-sized first chunk, and -- through the host-only fault
// injection -- what a refused chunk allocation leaves behind.
//
// Based on the tests in crosspoint-reader PR #3027 (Sidney San Martín, @s4y), extended for the
// reserve, pop_back, move and failure behaviour this tree's version adds.
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <utility>

#include "ChunkedVector.h"

namespace {

// Restores "never refuse" however the test ends.
struct AllocationsLeft {
  explicit AllocationsLeft(const size_t n) { chunkedVectorAllocationsLeft() = n; }
  ~AllocationsLeft() { chunkedVectorAllocationsLeft() = SIZE_MAX; }
  AllocationsLeft(const AllocationsLeft&) = delete;
  AllocationsLeft& operator=(const AllocationsLeft&) = delete;
};

template <typename V>
void fillAndCheck(V& v, const size_t count) {
  for (size_t i = v.size(); i < count; ++i) {
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
  // Chunks of 4, 8, then 16 each: 12 + 3 * 16 covers the 60 asked for.
  using V = ChunkedVector<int, 4, 16, 60>;
  static_assert(V::minMaxSize() == 60);
  V v;
  EXPECT_EQ(v.maxSize(), 60u);
  fillAndCheck(v, v.maxSize());
  EXPECT_FALSE(v.push_back(-1)) << "past maxSize()";
  EXPECT_EQ(v.size(), v.maxSize());
  EXPECT_EQ(v.back(), static_cast<int>(v.maxSize() - 1));
}

TEST(ChunkedVectorTest, FixedChunksWhenFirstEqualsMax) {
  using V = ChunkedVector<int, 8, 8, 24>;
  static_assert(V::minMaxSize() == 24);
  V v;
  fillAndCheck(v, v.maxSize());
  EXPECT_FALSE(v.push_back(-1));
}

TEST(ChunkedVectorTest, ReserveSizesTheFirstChunkAndTakesOneBlockForIt) {
  using V = ChunkedVector<int, 4, 16, 60>;
  V v;
  {
    // The reserve's chunk and nothing else: every element up to the reserve fits in it.
    const AllocationsLeft one(1);
    ASSERT_TRUE(v.reserve(37));
    fillAndCheck(v, 37);
    EXPECT_FALSE(v.push_back(37)) << "element 38 needs a second chunk";
    EXPECT_EQ(v.size(), 37u);
  }
  // The chunks after a reserved one keep their sizes: the capacity grows by what the reserve
  // added over the default first chunk.
  EXPECT_EQ(v.maxSize(), 60u - 4u + 37u);
  fillAndCheck(v, v.maxSize());
  EXPECT_FALSE(v.push_back(-1));
}

TEST(ChunkedVectorTest, ReserveRulesFollowAVectorsOnlyWhereTheyAreCheap) {
  using V = ChunkedVector<int, 4, 16, 60>;
  V v;
  EXPECT_TRUE(v.reserve(0)) << "nothing to reserve";
  ASSERT_TRUE(v.reserve(1));
  EXPECT_EQ(v.maxSize(), 60u) << "a reserve below the first chunk keeps the first chunk's size";
  V w;
  ASSERT_TRUE(w.push_back(1));
  EXPECT_FALSE(w.reserve(30)) << "the first chunk exists already";
  EXPECT_EQ(w.size(), 1u);
  EXPECT_EQ(w[0], 1);
}

TEST(ChunkedVectorTest, AFailedReserveLeavesAUsableTable) {
  using V = ChunkedVector<int, 4, 16, 60>;
  V v;
  {
    const AllocationsLeft none(0);
    EXPECT_FALSE(v.reserve(40));
  }
  EXPECT_EQ(v.maxSize(), 60u) << "the first chunk is back to its default size";
  fillAndCheck(v, v.maxSize());
}

TEST(ChunkedVectorTest, ARefusedChunkStoresNothingAndTheTableCarriesOnAfterIt) {
  using V = ChunkedVector<int, 4, 16, 60>;
  V v;
  {
    // Chunks of 4 and 8: twelve elements, then the third chunk is refused.
    const AllocationsLeft two(2);
    fillAndCheck(v, 12);
    EXPECT_FALSE(v.push_back(12));
    EXPECT_FALSE(v.push_back(12)) << "refused again, never half-stored";
    EXPECT_EQ(v.size(), 12u);
  }
  // Nothing was lost or shifted by the refusals, and the next chunk can still be had.
  fillAndCheck(v, v.maxSize());
}

TEST(ChunkedVectorTest, PopBackReusesTheSlot) {
  using V = ChunkedVector<int, 4, 16, 60>;
  V v;
  fillAndCheck(v, 13);  // the 13th element opened the third chunk
  v.pop_back();
  EXPECT_EQ(v.size(), 12u);
  EXPECT_EQ(v.back(), 11);
  {
    const AllocationsLeft none(0);
    ASSERT_TRUE(v.push_back(12)) << "the chunk stayed allocated";
  }
  fillAndCheck(v, 20);
  V empty;
  empty.pop_back();  // no-op
  EXPECT_TRUE(empty.empty());
}

TEST(ChunkedVectorTest, MoveHandsTheChunksOverAndClearStartsOver) {
  using V = ChunkedVector<int, 4, 16, 60>;
  V source;
  ASSERT_TRUE(source.reserve(10));
  fillAndCheck(source, 30);
  V target(std::move(source));
  EXPECT_TRUE(source.empty());  // NOLINT(bugprone-use-after-move): the moved-from state is defined
  EXPECT_EQ(source.maxSize(), 60u);
  EXPECT_EQ(target.maxSize(), 60u - 4u + 10u);
  fillAndCheck(target, 30);

  V assigned;
  fillAndCheck(assigned, 5);
  assigned = std::move(target);
  EXPECT_TRUE(target.empty());  // NOLINT(bugprone-use-after-move)
  fillAndCheck(assigned, 30);

  assigned.clear();
  EXPECT_TRUE(assigned.empty());
  EXPECT_EQ(assigned.maxSize(), 60u);
  fillAndCheck(assigned, 60);
}

TEST(ChunkedVectorTest, ReaderInstantiationsCoverAWholeSection) {
  // Section::PageLut and ChapterHtmlSlimParser::ParagraphLut: 10000 is Page::MAX_PAGES_PER_SECTION.
  using PageLut = ChunkedVector<int, 16, 256, 10000>;
  using ParagraphLut = ChunkedVector<std::pair<uint32_t, uint32_t>, 16, 128, 10000>;
  static_assert(PageLut::minMaxSize() >= 10000);
  static_assert(ParagraphLut::minMaxSize() >= 10000);
  PageLut lut;
  ASSERT_TRUE(lut.reserve(52));
  fillAndCheck(lut, 10000);
  ParagraphLut paragraphs;
  for (uint32_t i = 0; i < 10000; ++i) {
    ASSERT_TRUE(paragraphs.push_back({i, ~i})) << "at " << i;
  }
  for (uint32_t i = 0; i < 10000; ++i) {
    EXPECT_EQ(paragraphs[i].first, i);
    EXPECT_EQ(paragraphs[i].second, ~i);
  }
}

TEST(ChunkedVectorTest, MovesNonTrivialValues) {
  ChunkedVector<std::pair<std::string, uint16_t>, 8, 32, 256> anchors;
  for (int i = 0; i < 200; ++i) {
    ASSERT_TRUE(anchors.push_back({"anchor-with-a-long-enough-id-" + std::to_string(i), static_cast<uint16_t>(i)}));
  }
  for (int i = 0; i < 200; ++i) {
    EXPECT_EQ(anchors[i].first, "anchor-with-a-long-enough-id-" + std::to_string(i));
    EXPECT_EQ(anchors[i].second, i);
  }
  anchors.pop_back();
  EXPECT_EQ(anchors.back().first, "anchor-with-a-long-enough-id-198");
}
