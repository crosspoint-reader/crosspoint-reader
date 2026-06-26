#include "src/simulator/SimulatorHeap.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

class SimulatorHeapTest : public ::testing::Test {
 protected:
  void TearDown() override { SimulatorHeap::shutdownForTests(); }
};

TEST_F(SimulatorHeapTest, FreshArenaReportsConstantTotalAndInitialFreeHeadroom) {
  ASSERT_TRUE(SimulatorHeap::resetForTests(4096));

  const std::size_t total = SimulatorHeap::totalBytes();
  const std::size_t free = SimulatorHeap::freeBytes();

  EXPECT_EQ(total, 4096U);
  EXPECT_GT(free, 0U);
  EXPECT_LT(free, total);
  EXPECT_EQ(SimulatorHeap::minFreeBytes(), free);
  EXPECT_LE(SimulatorHeap::largestFreeBlockBytes(), free);
}

TEST_F(SimulatorHeapTest, AllocateAndFreeRestoresFreeBytes) {
  ASSERT_TRUE(SimulatorHeap::resetForTests(4096));
  const std::size_t baselineFree = SimulatorHeap::freeBytes();

  void* ptr = SimulatorHeap::allocateForTests(128);
  ASSERT_NE(ptr, nullptr);
  EXPECT_LT(SimulatorHeap::freeBytes(), baselineFree);
  EXPECT_LT(SimulatorHeap::minFreeBytes(), baselineFree);

  SimulatorHeap::freeForTests(ptr);
  EXPECT_EQ(SimulatorHeap::freeBytes(), baselineFree);
  EXPECT_EQ(SimulatorHeap::largestFreeBlockBytes(), baselineFree);
}

TEST_F(SimulatorHeapTest, SplittingAndCoalescingAffectsLargestFreeBlock) {
  ASSERT_TRUE(SimulatorHeap::resetForTests(4096));
  const std::size_t baselineFree = SimulatorHeap::freeBytes();
  const std::size_t baselineMaxAlloc = SimulatorHeap::largestFreeBlockBytes();

  void* a = SimulatorHeap::allocateForTests(256);
  void* b = SimulatorHeap::allocateForTests(256);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);

  SimulatorHeap::freeForTests(a);
  const std::size_t fragmentedMaxAlloc = SimulatorHeap::largestFreeBlockBytes();
  EXPECT_LT(fragmentedMaxAlloc, baselineMaxAlloc);

  SimulatorHeap::freeForTests(b);
  EXPECT_EQ(SimulatorHeap::largestFreeBlockBytes(), baselineMaxAlloc);
  EXPECT_EQ(SimulatorHeap::freeBytes(), baselineFree);
}

TEST_F(SimulatorHeapTest, CoalescingMergesAdjacentFreeBlocksOnBothSides) {
  ASSERT_TRUE(SimulatorHeap::resetForTests(4096));
  const std::size_t baselineFree = SimulatorHeap::freeBytes();
  const std::size_t baselineMaxAlloc = SimulatorHeap::largestFreeBlockBytes();

  void* a = SimulatorHeap::allocateForTests(512);
  void* b = SimulatorHeap::allocateForTests(512);
  void* c = SimulatorHeap::allocateForTests(512);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  ASSERT_NE(c, nullptr);

  SimulatorHeap::freeForTests(b);
  EXPECT_LT(SimulatorHeap::largestFreeBlockBytes(), baselineMaxAlloc);

  SimulatorHeap::freeForTests(a);
  void* mergedFront = SimulatorHeap::allocateForTests(1024);
  EXPECT_NE(mergedFront, nullptr);
  SimulatorHeap::freeForTests(mergedFront);

  SimulatorHeap::freeForTests(c);
  EXPECT_EQ(SimulatorHeap::freeBytes(), baselineFree);
  EXPECT_EQ(SimulatorHeap::largestFreeBlockBytes(), baselineMaxAlloc);
}

TEST_F(SimulatorHeapTest, ReallocShrinkAndGrowPreservesContent) {
  ASSERT_TRUE(SimulatorHeap::resetForTests(8192));

  auto* ptr = static_cast<std::uint8_t*>(SimulatorHeap::allocateForTests(512));
  ASSERT_NE(ptr, nullptr);
  std::memset(ptr, 0xAB, 512);

  const std::size_t freeAfterAlloc = SimulatorHeap::freeBytes();
  auto* shrunk = static_cast<std::uint8_t*>(SimulatorHeap::reallocForTests(ptr, 128));
  ASSERT_EQ(shrunk, ptr);
  EXPECT_GT(SimulatorHeap::freeBytes(), freeAfterAlloc);
  for (std::size_t i = 0; i < 128; ++i) {
    EXPECT_EQ(shrunk[i], 0xAB);
  }

  auto* grown = static_cast<std::uint8_t*>(SimulatorHeap::reallocForTests(shrunk, 1024));
  ASSERT_NE(grown, nullptr);
  for (std::size_t i = 0; i < 128; ++i) {
    EXPECT_EQ(grown[i], 0xAB);
  }
}

TEST_F(SimulatorHeapTest, FragmentationCanBlockLargeAllocationEvenWhenFreeBytesRemain) {
  ASSERT_TRUE(SimulatorHeap::resetForTests(4096));

  void* a = SimulatorHeap::allocateForTests(880);
  void* b = SimulatorHeap::allocateForTests(880);
  void* c = SimulatorHeap::allocateForTests(880);
  void* d = SimulatorHeap::allocateForTests(880);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  ASSERT_NE(c, nullptr);
  ASSERT_NE(d, nullptr);

  SimulatorHeap::freeForTests(a);
  SimulatorHeap::freeForTests(c);

  EXPECT_GT(SimulatorHeap::freeBytes(), 1024U);
  EXPECT_LT(SimulatorHeap::largestFreeBlockBytes(), 1024U);
  EXPECT_EQ(SimulatorHeap::allocateForTests(1024), nullptr);

  SimulatorHeap::freeForTests(b);
  void* merged = SimulatorHeap::allocateForTests(1024);
  EXPECT_NE(merged, nullptr);
  SimulatorHeap::freeForTests(merged);
  SimulatorHeap::freeForTests(d);
}

TEST_F(SimulatorHeapTest, CallocZeroesAndTotalRemainsConstant) {
  ASSERT_TRUE(SimulatorHeap::resetForTests(4096));
  const std::size_t total = SimulatorHeap::totalBytes();

  auto* ptr = static_cast<std::uint32_t*>(SimulatorHeap::callocForTests(16, sizeof(std::uint32_t)));
  ASSERT_NE(ptr, nullptr);
  for (std::size_t i = 0; i < 16; ++i) {
    EXPECT_EQ(ptr[i], 0U);
  }

  EXPECT_EQ(SimulatorHeap::totalBytes(), total);
  EXPECT_LE(SimulatorHeap::largestFreeBlockBytes(), SimulatorHeap::freeBytes());

  SimulatorHeap::freeForTests(ptr);
  EXPECT_EQ(SimulatorHeap::totalBytes(), total);
}

}  // namespace
