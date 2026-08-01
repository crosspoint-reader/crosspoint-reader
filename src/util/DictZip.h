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

// Why an extraction failed, so callers can report the accurate cause.
enum class ExtractError : uint8_t {
  None,        // success
  LowMemory,   // an allocation failed — the ~32KB inflate window or a chunk
               // buffer couldn't be obtained from the fragmented heap
  ReadError,   // file open / read / write / bad-offset failure (IO or a bogus
               // .idx offset), not a compression problem
  Decompress,  // the compressed stream itself was bad (inflate failed, or the
               // .dz header/chunk table didn't parse) — corrupt/truncated .dz
};

// Parse the dictzip header/chunk table. On failure, *outError (if provided)
// reports ReadError for a truncated/IO read or Decompress for a malformed file.
bool parse(HalFile& file, Info* info, ExtractError* outError = nullptr);

// Decompress the uncompressed byte range [offset, offset+size) into outFile.
// On failure, *outError (if provided) reports the specific cause so callers can
// distinguish low memory from IO from a corrupt file.
bool extractEntry(const char* path, uint32_t offset, uint32_t size, HalFile& outFile, ExtractError* outError = nullptr);

}  // namespace DictZip
