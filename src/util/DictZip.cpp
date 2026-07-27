#include "DictZip.h"

#include <Arduino.h>  // ESP.getMaxAllocHeap for the pre-reserve heap guard
#include <InflateReader.h>
#include <Memory.h>

namespace DictZip {
namespace {

// Caps the chunk table at 32KB of heap (8192 * 4 bytes); at the typical ~58KB
// chunk length that still allows ~460MB of uncompressed dictionary data.
constexpr uint16_t MAX_CHUNK_COUNT = 8192;

// Slack required above the chunk table before reserving it, so a table that
// would only just fit is refused rather than left to abort inside reserve().
constexpr size_t CHUNK_TABLE_HEAP_HEADROOM_BYTES = 1024;

bool readLe16(HalFile& file, uint16_t* out) {
  uint8_t raw[2];
  if (file.read(raw, 2) != 2) return false;
  *out = static_cast<uint16_t>(raw[0] | (static_cast<uint16_t>(raw[1]) << 8));
  return true;
}

bool extractChunkSlice(HalFile& file, uint32_t compressedOffset, uint32_t compressedSize, uint32_t discardSize,
                       uint32_t extractSize, HalFile& outFile, ExtractError* outError) {
  const auto fail = [outError](ExtractError e) {
    if (outError) *outError = e;
    return false;
  };
  if (extractSize == 0) return true;
  auto compBuf = makeUniqueNoThrow<uint8_t[]>(compressedSize);
  if (!compBuf) return fail(ExtractError::LowMemory);  // couldn't get the compressed-chunk buffer

  // compressedOffset comes from the untrusted .dz chunk table, so an out-of-range
  // seek is possible — guard it rather than reading from the prior position.
  if (!file.seekSet(compressedOffset)) return fail(ExtractError::ReadError);
  if (file.read(compBuf.get(), static_cast<int>(compressedSize)) != static_cast<int>(compressedSize))
    return fail(ExtractError::ReadError);

  InflateReader reader;
  if (!reader.init(true)) return fail(ExtractError::LowMemory);  // 32KB inflate window allocation failed
  reader.setSource(compBuf.get(), compressedSize);

  auto buf = makeUniqueNoThrow<uint8_t[]>(512);
  if (!buf) return fail(ExtractError::LowMemory);

  uint32_t batch;
  while (discardSize > 0) {
    batch = discardSize < 512 ? discardSize : 512;
    if (!reader.read(buf.get(), batch)) return fail(ExtractError::Decompress);
    discardSize -= batch;
  }

  while (extractSize > 0) {
    batch = extractSize < 512 ? extractSize : 512;
    if (!reader.read(buf.get(), batch)) return fail(ExtractError::Decompress);
    if (outFile.write(buf.get(), batch) != batch) return fail(ExtractError::ReadError);
    extractSize -= batch;
  }

  return true;
}

}  // namespace

bool parse(HalFile& file, Info* info, ExtractError* outError) {
  // Classify each failure: a header/table read that comes up short is a
  // ReadError (IO / truncated file); a value that doesn't make sense as dictzip
  // is a Decompress (malformed/unsupported .dz).
  const auto fail = [outError](ExtractError e) {
    if (outError) *outError = e;
    return false;
  };
  if (outError) *outError = ExtractError::None;  // establish the out-param on every path
  if (!info) return fail(ExtractError::Decompress);
  *info = {};

  uint8_t header[10];
  if (file.read(header, sizeof(header)) != static_cast<int>(sizeof(header))) return fail(ExtractError::ReadError);
  if (header[0] != 0x1f || header[1] != 0x8b || header[2] != 8) return fail(ExtractError::Decompress);

  const uint8_t flags = header[3];
  if ((flags & 0x04) == 0) return fail(ExtractError::Decompress);  // dictzip requires FEXTRA

  uint16_t xlen = 0;
  if (!readLe16(file, &xlen)) return fail(ExtractError::ReadError);

  uint32_t extraRead = 0;
  bool foundRa = false;
  while (extraRead + 4 <= xlen) {
    uint8_t subHeader[4];
    if (file.read(subHeader, sizeof(subHeader)) != static_cast<int>(sizeof(subHeader)))
      return fail(ExtractError::ReadError);
    extraRead += 4;
    const uint16_t subLen = static_cast<uint16_t>(subHeader[2] | (static_cast<uint16_t>(subHeader[3]) << 8));
    if (extraRead + subLen > xlen) return fail(ExtractError::Decompress);

    if (subHeader[0] == 'R' && subHeader[1] == 'A') {
      // Reject a second RA table rather than break: breaking would leave
      // extraRead < xlen and trip the check below for valid files carrying other
      // FEXTRA subfields after the RA one. Re-entering would push_back past the
      // reserve() below, and that growth aborts under -fno-exceptions.
      if (foundRa) return fail(ExtractError::Decompress);
      if (subLen < 6) return fail(ExtractError::Decompress);

      uint16_t version = 0;
      uint16_t chunkLen = 0;
      uint16_t chunkCount = 0;
      if (!readLe16(file, &version) || !readLe16(file, &chunkLen) || !readLe16(file, &chunkCount))
        return fail(ExtractError::ReadError);
      extraRead += 6;
      if (version != 1 || chunkLen == 0 || chunkCount == 0 || chunkCount > MAX_CHUNK_COUNT)
        return fail(ExtractError::Decompress);
      if (subLen != static_cast<uint16_t>(6 + chunkCount * 2)) return fail(ExtractError::Decompress);

      info->chunkLength = chunkLen;
      // vector reserve() aborts under -fno-exceptions if it can't allocate;
      // refuse up front (like Dictionary::readDefinition's guard) so a fragmented
      // heap surfaces LowMemory instead of crashing. chunkCount <= MAX_CHUNK_COUNT
      // caps this at ~32KB.
      const size_t chunkTableBytes = (static_cast<size_t>(chunkCount) + 1) * sizeof(uint32_t);
      if (ESP.getMaxAllocHeap() < chunkTableBytes + CHUNK_TABLE_HEAP_HEADROOM_BYTES)
        return fail(ExtractError::LowMemory);
      info->chunkOffsets.reserve(static_cast<size_t>(chunkCount) + 1);
      info->chunkOffsets.push_back(0);
      uint32_t cumulative = 0;
      for (uint16_t i = 0; i < chunkCount; i++) {
        uint16_t compLen = 0;
        if (!readLe16(file, &compLen)) return fail(ExtractError::ReadError);
        extraRead += 2;
        cumulative += compLen;
        info->chunkOffsets.push_back(cumulative);
      }
      foundRa = true;
    } else {
      if (!file.seekSet(file.position() + subLen)) return fail(ExtractError::ReadError);
      extraRead += subLen;
    }
  }
  if (extraRead != xlen || !foundRa) return fail(ExtractError::Decompress);

  if (flags & 0x08) {  // FNAME
    int b;
    do {
      b = file.read();
      if (b < 0) return fail(ExtractError::ReadError);
    } while (b != 0);
  }
  if (flags & 0x10) {  // FCOMMENT
    int b;
    do {
      b = file.read();
      if (b < 0) return fail(ExtractError::ReadError);
    } while (b != 0);
  }
  if (flags & 0x02) {  // FHCRC
    uint8_t crc[2];
    if (file.read(crc, 2) != 2) return fail(ExtractError::ReadError);
  }

  info->dataOffset = static_cast<uint32_t>(file.position());
  const uint32_t fileSize = static_cast<uint32_t>(file.fileSize());
  if (fileSize < 4) return fail(ExtractError::Decompress);
  if (!file.seekSet(fileSize - 4)) return fail(ExtractError::ReadError);
  uint8_t isizeRaw[4];
  if (file.read(isizeRaw, 4) != 4) return fail(ExtractError::ReadError);
  info->totalSize = static_cast<uint32_t>(isizeRaw[0]) | (static_cast<uint32_t>(isizeRaw[1]) << 8) |
                    (static_cast<uint32_t>(isizeRaw[2]) << 16) | (static_cast<uint32_t>(isizeRaw[3]) << 24);
  if (info->totalSize == 0) return fail(ExtractError::Decompress);
  info->valid = true;
  return true;
}

bool extractEntry(const char* path, uint32_t offset, uint32_t size, HalFile& outFile, ExtractError* outError) {
  const auto fail = [outError](ExtractError e) {
    if (outError) *outError = e;
    return false;
  };
  if (outError) *outError = ExtractError::None;
  if (size == 0) return true;

  HalFile file;
  if (!Storage.openFileForRead("DICTZIP", path, file)) return fail(ExtractError::ReadError);

  Info info;
  // parse() reports its own cause: a header/table read failure is ReadError,
  // a malformed/unsupported .dz is Decompress.
  if (!parse(file, &info, outError)) return false;

  // Reject ranges outside the uncompressed data (offset/size come from the
  // untrusted .idx). Subtraction form avoids uint32 overflow in offset + size
  // and guarantees localOffset < chunkOutSize in the loop below.
  if (offset > info.totalSize || size > info.totalSize - offset) return fail(ExtractError::ReadError);

  const uint32_t startChunk = offset / info.chunkLength;
  const uint32_t endChunk = (offset + size - 1) / info.chunkLength;
  if (endChunk + 1 >= info.chunkOffsets.size()) return fail(ExtractError::ReadError);

  uint32_t remaining = size;
  const uint32_t lastChunk = static_cast<uint32_t>(info.chunkOffsets.size() - 2);
  for (uint32_t chunk = startChunk; chunk <= endChunk; chunk++) {
    uint32_t chunkOutSize = info.chunkLength;
    if (chunk == lastChunk) chunkOutSize = info.totalSize - chunk * info.chunkLength;
    if (chunkOutSize == 0 || chunkOutSize > info.chunkLength) chunkOutSize = info.chunkLength;

    const uint32_t localOffset = (chunk == startChunk) ? (offset % info.chunkLength) : 0;
    const uint32_t available = chunkOutSize - localOffset;
    const uint32_t take = remaining < available ? remaining : available;

    const uint32_t compOffset = info.dataOffset + info.chunkOffsets[chunk];
    const uint32_t compSize = info.chunkOffsets[chunk + 1] - info.chunkOffsets[chunk];
    if (!extractChunkSlice(file, compOffset, compSize, localOffset, take, outFile, outError)) return false;

    remaining -= take;
    if (remaining == 0) break;
  }

  // Should be exhausted given the bounds check + chunk math above; if not, the
  // chunk table is inconsistent with the requested range — treat as corrupt.
  if (remaining != 0) return fail(ExtractError::Decompress);
  return true;
}

}  // namespace DictZip
