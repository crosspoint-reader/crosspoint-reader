#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "esp_partition.h"

// Function names of the running build, read from the table that
// scripts/gen_symbol_table.py stores in the app image (format described there).
namespace SymbolTable {

struct Table {
  const esp_partition_t* partition = nullptr;
  uint32_t offset = 0;  // of the table in the partition
  uint32_t size = 0;
  uint32_t indexOffset = 0;  // from the table start
  uint32_t blockCount = 0;
  uint32_t maxBlockSize = 0;
  uint32_t firstAddress = 0;
  uint32_t endAddress = 0;

  // The inflated block of the last lookup, kept because stack words cluster.
  std::unique_ptr<uint8_t[]> block;
  uint32_t loadedBlock = UINT32_MAX;
  uint32_t blockAddress = 0;
  uint32_t blockEndAddress = 0;
  uint32_t blockSize = 0;
};

// Allocates one block buffer (maxBlockSize, about 8 KB) that lives as long as
// the table. False when the running image has no table or the heap is short.
bool open(Table& table);

struct Function {
  const char* name;  // in the table's block buffer: valid until the next lookup
  uint32_t offset;   // of the address from the start of the entry
  uint32_t size;     // up to the next entry: can include alignment padding
  // A library outside the project, stored as one range named "[library]".
  bool isLibraryRange;
};

// Finds the function or library range that holds a code address. False when none does.
bool lookup(Table& table, uint32_t address, Function& function);

}  // namespace SymbolTable
