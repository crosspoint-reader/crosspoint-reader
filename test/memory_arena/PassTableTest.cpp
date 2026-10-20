// PassTable: a lookup table sized to the memory there is (lib/Memory/PassTable.h).
#include <PassTable.h>
#include <esp_heap_caps.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <memory>

namespace {

struct Entry {
  uint64_t key;
  uint64_t value;
};

TEST(PassTable, ARegionWithRoomHoldsEveryEntryAndTheExtraAndIsReturnedEmpty) {
  auto buffer = std::make_unique<uint8_t[]>(8192);
  BuildArena region(buffer.get(), 8192);
  {
    const auto table = PassTable<Entry>::fromRegion(&region, 100, 16, 1024);
    ASSERT_EQ(table.capacity(), 100u);
    EXPECT_TRUE(table.inRegion());
    ASSERT_NE(table.extra(), nullptr);
    EXPECT_EQ(table.extraBytes(), 1024u);
    EXPECT_GE(region.used(), 100 * sizeof(Entry) + 1024);
  }
  EXPECT_EQ(region.used(), 0u);
}

TEST(PassTable, ASmallRegionHoldsWhatFitsAndLeavesWhatItWasToldToKeep) {
  auto buffer = std::make_unique<uint8_t[]>(4096);
  BuildArena region(buffer.get(), 4096);
  const auto table = PassTable<Entry>::fromRegion(&region, 1000, 16, 0, 1024);
  // (4096 - 1024 - alignment) / 16 B.
  EXPECT_GE(table.capacity(), 185u);
  EXPECT_LE(table.capacity(), 192u);
  EXPECT_GE(region.capacity() - region.used(), 1024u);
}

TEST(PassTable, ARegionWithoutRoomForTheMinimumGivesNothing) {
  auto buffer = std::make_unique<uint8_t[]>(1024);
  BuildArena region(buffer.get(), 1024);
  const auto table = PassTable<Entry>::fromRegion(&region, 1000, 128);
  EXPECT_EQ(table.capacity(), 0u);
  EXPECT_FALSE(table.inRegion());
  EXPECT_EQ(region.used(), 0u);
  // A small want is satisfied by what fits, whatever the minimum.
  EXPECT_EQ(PassTable<Entry>::fromRegion(&region, 10, 128).capacity(), 10u);
}

TEST(PassTable, TheHeapGivesItsLargestBlockLessTheReserve) {
  PassTable<Entry>::HeapSizing sizing;
  {
    const HostFreeHeap freeHeap(300 * 1024);
    const HostLargestFreeBlock largest(200 * 1024);
    EXPECT_EQ(PassTable<Entry>::fromHeap(1732, sizing).capacity(), 1732u) << "room for every entry";
  }
  {
    const HostFreeHeap freeHeap(43576);
    const HostLargestFreeBlock largest(8 * 1024);
    EXPECT_EQ(PassTable<Entry>::fromHeap(1732, sizing).capacity(), (8 * 1024 - 64) / sizeof(Entry));
  }
  {
    const HostFreeHeap freeHeap(20 * 1024);
    const HostLargestFreeBlock largest(20 * 1024);
    // 4 KB above the reserve.
    EXPECT_EQ(PassTable<Entry>::fromHeap(1732, sizing).capacity(), (4 * 1024) / sizeof(Entry));
  }
  {
    const HostFreeHeap freeHeap(12 * 1024);
    const HostLargestFreeBlock largest(6 * 1024);
    EXPECT_EQ(PassTable<Entry>::fromHeap(1732, sizing).capacity(), sizing.minEntries)
        << "the minimum, whatever the heap reports";
  }
}

TEST(PassTable, TheHeapCarriesTheExtraAndNoWantGivesNothing) {
  const auto table = PassTable<Entry>::fromHeap(50, {}, 512);
  EXPECT_EQ(table.capacity(), 50u);
  EXPECT_FALSE(table.inRegion());
  ASSERT_NE(table.extra(), nullptr);
  EXPECT_EQ(table.extraBytes(), 512u);
  EXPECT_EQ(PassTable<Entry>::fromHeap(0, {}, 512).capacity(), 0u);
}

TEST(PassTable, AMovedTableReleasesItsBlockOnce) {
  auto buffer = std::make_unique<uint8_t[]>(4096);
  BuildArena region(buffer.get(), 4096);
  {
    auto first = PassTable<Entry>::fromRegion(&region, 10, 1);
    PassTable<Entry> second = std::move(first);
    EXPECT_EQ(first.capacity(), 0u);
    EXPECT_EQ(second.capacity(), 10u);
    EXPECT_TRUE(second.inRegion());
  }
  EXPECT_EQ(region.used(), 0u);
  EXPECT_EQ(region.releaseFailures(), 0u);
}

}  // namespace
