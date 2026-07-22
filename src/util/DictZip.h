#pragma once

#include <HalStorage.h>

#include <cstdint>
#include <vector>

// Random-access reader for dictzip (.dict.dz) files: gzip with an extra "RA"
// field holding a chunk table, so any byte range can be decompressed without
// inflating the whole file. Format: https://linux.die.net/man/1/dictzip
namespace DictZip {

struct Info {
  uint32_t dataOffset = 0;             // file offset where compressed chunk data starts
  uint32_t totalSize = 0;              // uncompressed size (gzip ISIZE trailer)
  uint16_t chunkLength = 0;            // uncompressed bytes per chunk
  std::vector<uint32_t> chunkOffsets;  // cumulative compressed offsets, chunkCount+1 entries
  bool valid = false;
};

bool parse(HalFile& file, Info* info);

// Decompress the uncompressed byte range [offset, offset+size) into outFile.
bool extractEntry(const char* path, uint32_t offset, uint32_t size, HalFile& outFile);

}  // namespace DictZip
