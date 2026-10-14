// The simulated NOR flash the glyph store's host tests run on: it has to behave like the chip.
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "SimNorFlash.h"

TEST(SimNorFlash, StartsErased) {
  SimNorFlash flash(8192);
  uint8_t bytes[4] = {};
  ASSERT_TRUE(flash.read(100, bytes, sizeof bytes));
  for (const uint8_t b : bytes) EXPECT_EQ(0xFF, b);
}

TEST(SimNorFlash, ProgramClearsBitsOnly) {
  SimNorFlash flash(8192);
  const uint8_t first = 0xF0;
  const uint8_t second = 0x3C;  // bits 2-3 would have to go from 0 back to 1
  ASSERT_TRUE(flash.program(10, &first, 1));
  ASSERT_TRUE(flash.program(10, &second, 1));
  uint8_t cell = 0;
  ASSERT_TRUE(flash.read(10, &cell, 1));
  EXPECT_EQ(0x30, cell);
  EXPECT_EQ(1u, flash.violations());
}

TEST(SimNorFlash, ReprogrammingTheSameValueIsNoViolation) {
  SimNorFlash flash(8192);
  const uint8_t value = 0xA5;
  ASSERT_TRUE(flash.program(10, &value, 1));
  ASSERT_TRUE(flash.program(10, &value, 1));
  EXPECT_EQ(0u, flash.violations());
}

TEST(SimNorFlash, EraseSetsWholeSectorsAndCountsThem) {
  SimNorFlash flash(3 * 4096);
  const uint8_t zero = 0;
  ASSERT_TRUE(flash.program(4096 + 5, &zero, 1));
  ASSERT_TRUE(flash.erase(4096, 4096));
  uint8_t cell = 0;
  ASSERT_TRUE(flash.read(4096 + 5, &cell, 1));
  EXPECT_EQ(0xFF, cell);
  EXPECT_EQ(0u, flash.sectorErases(0));
  EXPECT_EQ(1u, flash.sectorErases(1));
  EXPECT_FALSE(flash.erase(100, 4096));  // not sector-aligned
  EXPECT_FALSE(flash.erase(4096, 100));
  EXPECT_EQ(1u, flash.totalSectorErases());
}

TEST(SimNorFlash, OutOfRangeIsRefused) {
  SimNorFlash flash(8192);
  uint8_t bytes[8] = {};
  EXPECT_FALSE(flash.read(8190, bytes, sizeof bytes));
  EXPECT_FALSE(flash.program(8190, bytes, sizeof bytes));
  EXPECT_FALSE(flash.erase(8192, 4096));
  EXPECT_EQ(nullptr, flash.map(8190, 8));
}

TEST(SimNorFlash, MappingSeesLaterWrites) {
  SimNorFlash flash(8192);
  const uint8_t* view = flash.map(0, 4096);
  ASSERT_NE(nullptr, view);
  const uint8_t value = 0x42;
  ASSERT_TRUE(flash.program(3, &value, 1));
  EXPECT_EQ(0x42, view[3]);
  ASSERT_TRUE(flash.erase(0, 4096));
  EXPECT_EQ(0xFF, view[3]);
}

TEST(SimNorFlash, MappingsAreLimitedByFreePages) {
  SimNorFlash flash(4 * 65536, 2);
  const uint8_t* a = flash.map(0, 65536);
  const uint8_t* b = flash.map(65536, 65536);
  ASSERT_NE(nullptr, a);
  ASSERT_NE(nullptr, b);
  EXPECT_EQ(0u, flash.freeMapPages());
  EXPECT_EQ(nullptr, flash.map(131072, 10));
  flash.unmap(a);
  EXPECT_EQ(1u, flash.freeMapPages());
  EXPECT_EQ(1u, flash.mappings());
}

TEST(SimNorFlash, AMappingAcrossAPageBoundaryCostsTwoPages) {
  SimNorFlash flash(4 * 65536, 4);
  ASSERT_NE(nullptr, flash.map(60000, 10000));
  EXPECT_EQ(2u, flash.freeMapPages());
}

TEST(SimNorFlash, PowerCutStopsAWriteAndEverythingAfter) {
  SimNorFlash flash(8192);
  const uint8_t zeros[8] = {};
  flash.cutAfterSteps(3);
  EXPECT_FALSE(flash.program(0, zeros, sizeof zeros));
  EXPECT_TRUE(flash.dead());
  uint8_t cells[8] = {};
  ASSERT_TRUE(flash.read(0, cells, sizeof cells));
  for (int i = 0; i < 3; ++i) EXPECT_EQ(0x00, cells[i]) << i;
  for (int i = 3; i < 8; ++i) EXPECT_EQ(0xFF, cells[i]) << i;
  EXPECT_FALSE(flash.program(100, zeros, 1));
  EXPECT_FALSE(flash.erase(4096, 4096));
  flash.powerCycle();
  EXPECT_TRUE(flash.program(100, zeros, 1));
}

TEST(SimNorFlash, PartialCutLeavesSomeBitsOfTheByteInFlight) {
  SimNorFlash flash(8192);
  flash.partialCut(true);
  flash.cutAfterSteps(0);
  const uint8_t zero = 0x00;
  EXPECT_FALSE(flash.program(7, &zero, 1));
  uint8_t cell = 0;
  ASSERT_TRUE(flash.read(7, &cell, 1));
  EXPECT_EQ(0x5A, cell);
}

TEST(SimNorFlash, PowerCutDuringEraseLeavesTheSectorHalfErased) {
  SimNorFlash flash(8192);
  const std::vector<uint8_t> zeros(4096, 0);
  ASSERT_TRUE(flash.program(0, zeros.data(), 4096));
  flash.cutAfterSteps(0);
  EXPECT_FALSE(flash.erase(0, 4096));
  uint8_t first = 0;
  uint8_t last = 0xFF;
  ASSERT_TRUE(flash.read(0, &first, 1));
  ASSERT_TRUE(flash.read(4095, &last, 1));
  EXPECT_EQ(0xFF, first);
  EXPECT_EQ(0x00, last);
}

TEST(SimNorFlash, StepsCountBytesAndSectors) {
  SimNorFlash flash(3 * 4096);
  const uint8_t zeros[10] = {};
  ASSERT_TRUE(flash.program(0, zeros, sizeof zeros));
  EXPECT_EQ(10u, flash.steps());
  ASSERT_TRUE(flash.erase(4096, 8192));
  EXPECT_EQ(12u, flash.steps());
}

TEST(SimNorFlash, PowerCycleDropsMappings) {
  SimNorFlash flash(4 * 65536, 3);
  ASSERT_NE(nullptr, flash.map(0, 65536));
  flash.powerCycle();
  EXPECT_EQ(0u, flash.mappings());
  EXPECT_EQ(3u, flash.freeMapPages());
}
