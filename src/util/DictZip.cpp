#include "DictZip.h"

#include <InflateReader.h>
#include <Memory.h>

namespace DictZip {
namespace {

// Caps the chunk table at 32KB of heap (8192 * 4 bytes); at the typical ~58KB
// chunk length that still allows ~460MB of uncompressed dictionary data.
constexpr uint16_t MAX_CHUNK_COUNT = 8192;

bool readLe16(HalFile& file, uint16_t* out) {
  uint8_t raw[2];
  if (file.read(raw, 2) != 2) return false;
  *out = static_cast<uint16_t>(raw[0] | (static_cast<uint16_t>(raw[1]) << 8));
  return true;
}

bool extractChunkSlice(HalFile& file, uint32_t compressedOffset, uint32_t compressedSize, uint32_t discardSize,
                       uint32_t extractSize, HalFile& outFile) {
  if (extractSize == 0) return true;
  auto compBuf = makeUniqueNoThrow<uint8_t[]>(compressedSize);
  if (!compBuf) return false;

  file.seekSet(compressedOffset);
  if (file.read(compBuf.get(), static_cast<int>(compressedSize)) != static_cast<int>(compressedSize)) return false;

  InflateReader reader;
  if (!reader.init(true)) return false;
  reader.setSource(compBuf.get(), compressedSize);

  auto buf = makeUniqueNoThrow<uint8_t[]>(512);
  if (!buf) return false;

  uint32_t batch;
  while (discardSize > 0) {
    batch = discardSize < 512 ? discardSize : 512;
    if (!reader.read(buf.get(), batch)) return false;
    discardSize -= batch;
  }

  while (extractSize > 0) {
    batch = extractSize < 512 ? extractSize : 512;
    if (!reader.read(buf.get(), batch)) return false;
    if (outFile.write(buf.get(), batch) != batch) return false;
    extractSize -= batch;
  }

  return true;
}

}  // namespace

bool parse(HalFile& file, Info* info) {
  if (!info) return false;
  *info = {};

  uint8_t header[10];
  if (file.read(header, sizeof(header)) != static_cast<int>(sizeof(header))) return false;
  if (header[0] != 0x1f || header[1] != 0x8b || header[2] != 8) return false;

  const uint8_t flags = header[3];
  if ((flags & 0x04) == 0) return false;  // dictzip requires FEXTRA

  uint16_t xlen = 0;
  if (!readLe16(file, &xlen)) return false;

  uint32_t extraRead = 0;
  bool foundRa = false;
  while (extraRead + 4 <= xlen) {
    uint8_t subHeader[4];
    if (file.read(subHeader, sizeof(subHeader)) != static_cast<int>(sizeof(subHeader))) return false;
    extraRead += 4;
    const uint16_t subLen = static_cast<uint16_t>(subHeader[2] | (static_cast<uint16_t>(subHeader[3]) << 8));
    if (extraRead + subLen > xlen) return false;

    if (subHeader[0] == 'R' && subHeader[1] == 'A') {
      if (subLen < 6) return false;

      uint16_t version = 0;
      uint16_t chunkLen = 0;
      uint16_t chunkCount = 0;
      if (!readLe16(file, &version) || !readLe16(file, &chunkLen) || !readLe16(file, &chunkCount)) return false;
      extraRead += 6;
      if (version != 1 || chunkLen == 0 || chunkCount == 0 || chunkCount > MAX_CHUNK_COUNT) return false;
      if (subLen != static_cast<uint16_t>(6 + chunkCount * 2)) return false;

      info->chunkLength = chunkLen;
      info->chunkOffsets.reserve(static_cast<size_t>(chunkCount) + 1);
      info->chunkOffsets.push_back(0);
      uint32_t cumulative = 0;
      for (uint16_t i = 0; i < chunkCount; i++) {
        uint16_t compLen = 0;
        if (!readLe16(file, &compLen)) return false;
        extraRead += 2;
        cumulative += compLen;
        info->chunkOffsets.push_back(cumulative);
      }
      foundRa = true;
    } else {
      file.seekSet(file.position() + subLen);
      extraRead += subLen;
    }
  }
  if (extraRead != xlen || !foundRa) return false;

  if (flags & 0x08) {  // FNAME
    int b;
    do {
      b = file.read();
      if (b < 0) return false;
    } while (b != 0);
  }
  if (flags & 0x10) {  // FCOMMENT
    int b;
    do {
      b = file.read();
      if (b < 0) return false;
    } while (b != 0);
  }
  if (flags & 0x02) {  // FHCRC
    uint8_t crc[2];
    if (file.read(crc, 2) != 2) return false;
  }

  info->dataOffset = static_cast<uint32_t>(file.position());
  const uint32_t fileSize = static_cast<uint32_t>(file.fileSize());
  if (fileSize < 4) return false;
  file.seekSet(fileSize - 4);
  uint8_t isizeRaw[4];
  if (file.read(isizeRaw, 4) != 4) return false;
  info->totalSize = static_cast<uint32_t>(isizeRaw[0]) | (static_cast<uint32_t>(isizeRaw[1]) << 8) |
                    (static_cast<uint32_t>(isizeRaw[2]) << 16) | (static_cast<uint32_t>(isizeRaw[3]) << 24);
  if (info->totalSize == 0) return false;
  info->valid = true;
  return true;
}

bool extractEntry(const char* path, uint32_t offset, uint32_t size, HalFile& outFile) {
  if (size == 0) return true;

  HalFile file;
  if (!Storage.openFileForRead("DICTZIP", path, file)) return false;

  Info info;
  if (!parse(file, &info)) return false;

  // Reject ranges outside the uncompressed data (offset/size come from the
  // untrusted .idx). Subtraction form avoids uint32 overflow in offset + size
  // and guarantees localOffset < chunkOutSize in the loop below.
  if (offset > info.totalSize || size > info.totalSize - offset) return false;

  const uint32_t startChunk = offset / info.chunkLength;
  const uint32_t endChunk = (offset + size - 1) / info.chunkLength;
  if (endChunk + 1 >= info.chunkOffsets.size()) return false;

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
    if (!extractChunkSlice(file, compOffset, compSize, localOffset, take, outFile)) return false;

    remaining -= take;
    if (remaining == 0) break;
  }

  return remaining == 0;
}

}  // namespace DictZip
