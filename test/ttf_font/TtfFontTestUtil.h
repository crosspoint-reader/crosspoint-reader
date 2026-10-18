#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

#include "TtfFamily.h"

namespace ttf_test {

/// A font file read through ttf::FileSource, as the SD card serves it, with an I/O error on demand. Every read's
/// offset is kept, so a test can see which way the reads went.
struct MemoryFile {
  std::vector<uint8_t> bytes;
  bool failReads = false;
  uint32_t reads = 0;
  uint32_t stamp = 0x57A3;  ///< what the card says of the file: FileSource::stamp
  std::vector<uint32_t> offsets;

  ttf::FileSource source() { return {static_cast<uint32_t>(bytes.size()), &MemoryFile::read, this, stamp}; }

  static bool read(void* ctx, const uint32_t offset, void* dst, const uint32_t len) {
    auto* file = static_cast<MemoryFile*>(ctx);
    ++file->reads;
    file->offsets.push_back(offset);
    if (file->failReads || offset > file->bytes.size() || len > file->bytes.size() - offset) return false;
    std::memcpy(dst, file->bytes.data() + offset, len);
    return true;
  }
};

}  // namespace ttf_test
