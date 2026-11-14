#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "SymbolTable.h"
#include "esp_ota_ops.h"

// The vendored uzlib ships without its checksum sources; the firmware never
// calls uzlib_uncompress_chksum, so the linker drops the references there.
extern "C" uint32_t uzlib_adler32(const void*, unsigned int, uint32_t) { std::abort(); }
extern "C" uint32_t uzlib_crc32(const void*, unsigned int, uint32_t) { std::abort(); }

namespace {

// The "running app partition": the image under test, followed by erased flash.
std::vector<uint8_t> flash;
esp_partition_t partition{};

void loadImage(const char* name) {
  std::ifstream file(std::string(SYMBOL_TABLE_FIXTURE_DIR) + "/" + name, std::ios::binary);
  ASSERT_TRUE(file.good()) << name;
  flash.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
  flash.resize(flash.size() + 4096, 0xFF);
  partition.size = static_cast<uint32_t>(flash.size());
}

size_t tableOffset() {
  const char magic[] = {'E', 'S', 'Y', 'M'};
  const auto found = std::search(flash.begin(), flash.end(), std::begin(magic), std::end(magic));
  EXPECT_NE(found, flash.end());
  return static_cast<size_t>(found - flash.begin());
}

struct Expected {
  uint32_t address;
  const char* name;  // nullptr: no function holds the address
  uint32_t offset;
  uint32_t size;
  bool isLibraryRange;
};

// Mirrors ENTRIES in make_fixture.py: for each entry its first and last address,
// and the addresses around every hole and around the ends of the table.
constexpr Expected EXPECTED[] = {
    {0x00000000, nullptr, 0, 0, false},
    {0x3FFFFFFF, nullptr, 0, 0, false},
    {0x40000000, "[ROM]", 0, 0x60000, true},
    {0x4005FFFF, "[ROM]", 0x5FFFF, 0x60000, true},
    {0x40060000, nullptr, 0, 0, false},
    {0x41FFFFFF, nullptr, 0, 0, false},
    {0x42000000, "alpha", 0, 0x10, false},
    {0x4200000F, "alpha", 0xF, 0x10, false},
    {0x42000010, "beta::gamma", 0, 0x20, false},
    {0x4200002F, "beta::gamma", 0x1F, 0x20, false},
    {0x42000030, nullptr, 0, 0, false},
    {0x420000FF, nullptr, 0, 0, false},
    {0x42000100, "[libnet80211]", 0, 0x300, true},
    {0x420003FF, "[libnet80211]", 0x2FF, 0x300, true},
    {0x42000400, "Reader::render::{lambda}", 0, 0x20, false},
    {0x4200041F, "Reader::render::{lambda}", 0x1F, 0x20, false},
    {0x42000420, "std::vector<>::push_back", 0, 0xE0, false},
    {0x420004FF, "std::vector<>::push_back", 0xDF, 0xE0, false},
    {0x42000500, "epsilon", 0, 2, false},
    {0x42000501, "epsilon", 1, 2, false},
    {0x42000502, "zeta", 0, 0xFAFE, false},
    {0x4200FFFF, "zeta", 0xFAFD, 0xFAFE, false},
    {0x42010000, "far_away", 0, 0x40, false},
    {0x4201003F, "far_away", 0x3F, 0x40, false},
    {0x42010040, nullptr, 0, 0, false},
    {0xFFFFFFFF, nullptr, 0, 0, false},
};

void expectLookup(SymbolTable::Table& table, const Expected& expected) {
  SCOPED_TRACE(::testing::Message() << "address 0x" << std::hex << expected.address);
  SymbolTable::Function function{};
  const bool found = SymbolTable::lookup(table, expected.address, function);
  ASSERT_EQ(found, expected.name != nullptr);
  if (!found) {
    return;
  }
  EXPECT_STREQ(function.name, expected.name);
  EXPECT_EQ(function.offset, expected.offset);
  EXPECT_EQ(function.size, expected.size);
  EXPECT_EQ(function.isLibraryRange, expected.isLibraryRange);
}

}  // namespace

esp_err_t esp_partition_read(const esp_partition_t*, size_t offset, void* destination, size_t size) {
  if (offset > flash.size() || size > flash.size() - offset) {
    return ESP_FAIL;
  }
  memcpy(destination, flash.data() + offset, size);
  return ESP_OK;
}

const esp_partition_t* esp_ota_get_running_partition() { return &partition; }

TEST(SymbolTable, ResolvesEveryEntryOfAGeneratedTable) {
  loadImage("with_table.bin");
  SymbolTable::Table table;
  ASSERT_TRUE(SymbolTable::open(table));
  // The fixture must exercise block boundaries, not one block.
  ASSERT_GE(table.blockCount, 3u);

  for (const Expected& expected : EXPECTED) {
    expectLookup(table, expected);
  }
}

TEST(SymbolTable, GivesTheSameAnswersWhenBlocksAreRevisited) {
  loadImage("with_table.bin");
  SymbolTable::Table table;
  ASSERT_TRUE(SymbolTable::open(table));

  // Reverse order makes every lookup leave the block the previous one loaded.
  for (auto expected = std::rbegin(EXPECTED); expected != std::rend(EXPECTED); ++expected) {
    expectLookup(table, *expected);
  }
}

TEST(SymbolTable, ImageWithoutTableDoesNotOpen) {
  loadImage("without_table.bin");
  SymbolTable::Table table;
  EXPECT_FALSE(SymbolTable::open(table));

  SymbolTable::Function function{};
  EXPECT_FALSE(SymbolTable::lookup(table, 0x42000000, function));
}

TEST(SymbolTable, UnknownVersionDoesNotOpen) {
  loadImage("with_table.bin");
  flash[tableOffset() + 4] ^= 0x7F;  // version follows the magic
  SymbolTable::Table table;
  EXPECT_FALSE(SymbolTable::open(table));
}

TEST(SymbolTable, DamagedBlockDataIsRejected) {
  loadImage("with_table.bin");
  SymbolTable::Table table;
  ASSERT_TRUE(SymbolTable::open(table));
  // Everything after the header and the index is deflate data.
  const size_t blocks = tableOffset() + table.indexOffset + (table.blockCount + 1) * 8;
  for (size_t i = blocks; i < tableOffset() + table.size; i++) {
    flash[i] = 0xFF;
  }

  // 0xFF bytes start a block of a reserved type: every block fails to inflate.
  SymbolTable::Function function{};
  for (const Expected& expected : EXPECTED) {
    EXPECT_FALSE(SymbolTable::lookup(table, expected.address, function));
  }
}
