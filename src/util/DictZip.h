#pragma once

#include <HalStorage.h>

#include <cstdint>
#include <string>
#include <vector>

namespace DictZip {

struct Info {
  uint32_t dataOffset = 0;
  uint32_t totalSize = 0;
  uint16_t chunkLength = 0;
  std::vector<uint32_t> chunkOffsets;
  bool valid = false;
};

bool parse(HalFile& file, Info* info);
bool validate(const char* path);
bool extractEntry(const char* path, uint32_t offset, uint32_t size, HalFile& outFile,
                  uint32_t* compressedBytesRead = nullptr, uint32_t* uncompressedBytesDecompressed = nullptr);

}  // namespace DictZip
