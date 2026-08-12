#include "DictZip.h"

#include <Arduino.h>  // ESP.getMaxAllocHeap for the pre-reserve heap guard
#include <InflateReader.h>
#include <Memory.h>

#include <cstddef>

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

// Compressed input for one chunk, pulled from the file on demand instead of
// buffered whole. A dictzip chunk is ~58KB uncompressed (measured at 18KB
// compressed for a real dictionary), so the old whole-chunk buffer was a large
// contiguous request made while the 32KB inflate window was about to be taken
// as well — two big blocks live at once, on a heap that during a reading
// session has well under 50KB free (#2744). This holds INPUT_BUF_BYTES instead.
//
// InflateReader MUST stay the first member: the uzlib callback receives only a
// uzlib_uncomp* and recovers this struct by casting it (see InflateReader.h).
constexpr size_t INPUT_BUF_BYTES = 2048;

struct ChunkSource {
  InflateReader reader;  // must be first
  HalFile* file = nullptr;
  uint32_t remaining = 0;  // compressed bytes left in this chunk
  bool readFailed = false;
  uint8_t buf[INPUT_BUF_BYTES] = {};
};

static_assert(offsetof(ChunkSource, reader) == 0, "InflateReader must be first for the uzlib callback cast");

// Refills from the file and returns the next byte, or -1 at the chunk's end.
// Never reads past compressedSize: the following bytes belong to the next
// chunk, and stopping there reproduces exactly the hard limit the whole-chunk
// buffer used to impose.
int chunkReadCb(uzlib_uncomp* u) {
  auto* src = reinterpret_cast<ChunkSource*>(u);
  if (src->remaining == 0) return -1;

  const uint32_t want = src->remaining < INPUT_BUF_BYTES ? src->remaining : INPUT_BUF_BYTES;
  const int n = src->file->read(src->buf, static_cast<int>(want));
  if (n <= 0) {
    // Remember the cause: uzlib collapses this into a generic decode failure,
    // but an IO error is a ReadError, not a corrupt .dz.
    src->readFailed = true;
    return -1;
  }
  src->remaining -= static_cast<uint32_t>(n);
  u->source = src->buf + 1;
  u->source_limit = src->buf + n;
  return src->buf[0];
}

bool extractChunkSlice(HalFile& file, uint32_t compressedOffset, uint32_t compressedSize, uint32_t discardSize,
                       uint32_t extractSize, HalFile& outFile, ExtractError* outError) {
  const auto fail = [outError](ExtractError e) {
    if (outError) *outError = e;
    return false;
  };
  if (extractSize == 0) return true;

  // Largest block FIRST. Every allocation here is carved from the same big free
  // run, so taking the ~3KB ChunkSource before the 32KB ring left the ring's
  // block 12 bytes short on device (largest=32756 against need=32768) even on a
  // barely fragmented heap. Ordering by size removes that failure mode: the ring
  // gets the big block while it is still whole, and the small buffers fit in
  // what remains — or in one of the smaller free blocks.
  auto window = makeUniqueNoThrow<uint8_t[]>(InflateReader::RING_BYTES);
  if (!window) return fail(ExtractError::LowMemory);

  auto src = makeUniqueNoThrow<ChunkSource>();
  if (!src) return fail(ExtractError::LowMemory);
  src->file = &file;
  src->remaining = compressedSize;

  // compressedOffset comes from the untrusted .dz chunk table, so an out-of-range
  // seek is possible — guard it rather than reading from the prior position.
  if (!file.seekSet(compressedOffset)) return fail(ExtractError::ReadError);

  // `window` outlives the reader (declared above it, destroyed after), as
  // initWithRing() requires.
  if (!src->reader.initWithRing(window.get())) return fail(ExtractError::LowMemory);
  // initWithRing() leaves source/source_limit null, so the very first byte
  // already comes through the callback. Set it after, which resets state.
  src->reader.setReadCallback(&chunkReadCb);

  auto buf = makeUniqueNoThrow<uint8_t[]>(512);
  if (!buf) return fail(ExtractError::LowMemory);

  // A decode failure after the callback hit an IO error is a read failure, not
  // a corrupt stream — keep the two distinguishable for the caller.
  const auto decodeFail = [&src, &fail] {
    return fail(src->readFailed ? ExtractError::ReadError : ExtractError::Decompress);
  };

  uint32_t batch;
  while (discardSize > 0) {
    batch = discardSize < 512 ? discardSize : 512;
    if (!src->reader.read(buf.get(), batch)) return decodeFail();
    discardSize -= batch;
  }

  while (extractSize > 0) {
    batch = extractSize < 512 ? extractSize : 512;
    if (!src->reader.read(buf.get(), batch)) return decodeFail();
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
