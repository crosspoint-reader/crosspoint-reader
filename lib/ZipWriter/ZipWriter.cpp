#include "ZipWriter.h"

#include <HalStorage.h>
#include <Logging.h>
#include <MinizConfig.h>

#include <cstring>
#include <memory>

namespace {
void le16(uint8_t* p, uint16_t v) {
  p[0] = v & 0xFF;
  p[1] = (v >> 8) & 0xFF;
}
void le32(uint8_t* p, uint32_t v) {
  p[0] = v & 0xFF;
  p[1] = (v >> 8) & 0xFF;
  p[2] = (v >> 16) & 0xFF;
  p[3] = (v >> 24) & 0xFF;
}
// Fixed DOS timestamp (2026-01-01 00:00) — SD has no reliable clock and EPUB
// consumers ignore it.
constexpr uint16_t DOS_TIME = 0;
constexpr uint16_t DOS_DATE = ((2026 - 1980) << 9) | (1 << 5) | 1;
}  // namespace

ZipWriter::ZipWriter() = default;
ZipWriter::~ZipWriter() = default;

bool ZipWriter::begin(const char* path) {
  out = std::make_unique<HalFile>();
  if (!Storage.openFileForWrite("ZIPW", path, *out)) {
    LOG_ERR("ZIPW", "open for write failed: %s", path);
    out.reset();
    return false;
  }
  offset = 0;
  entries.clear();
  return true;
}

bool ZipWriter::writeLocalHeader(const char* name, uint32_t crc, uint32_t size) {
  const size_t nameLen = strlen(name);
  uint8_t h[30];
  le32(h + 0, 0x04034b50);
  le16(h + 4, 20);        // version needed
  le16(h + 6, 0);         // flags
  le16(h + 8, 0);         // method: stored
  le16(h + 10, DOS_TIME);
  le16(h + 12, DOS_DATE);
  le32(h + 14, crc);
  le32(h + 18, size);  // compressed
  le32(h + 22, size);  // uncompressed
  le16(h + 26, (uint16_t)nameLen);
  le16(h + 28, 0);  // extra len
  if (out->write(h, sizeof(h)) != sizeof(h)) return false;
  if (out->write(name, nameLen) != nameLen) return false;
  return true;
}

bool ZipWriter::addFile(const char* name, const uint8_t* data, size_t len) {
  if (!out || streaming) return false;
  const uint32_t crc = (uint32_t)mz_crc32(MZ_CRC32_INIT, data, len);
  const uint32_t headerOffset = offset;
  if (!writeLocalHeader(name, crc, (uint32_t)len)) return false;
  if (len && out->write(data, len) != len) return false;
  offset += 30 + strlen(name) + (uint32_t)len;
  entries.push_back({std::string(name), crc, (uint32_t)len, headerOffset});
  return true;
}

bool ZipWriter::beginStreamedFile(const char* name, uint32_t totalLen) {
  if (!out || streaming) return false;
  streamHeaderOffset = offset;
  streamDeclared = totalLen;
  streamWritten = 0;
  streamCrc = (uint32_t)MZ_CRC32_INIT;
  // CRC is unknown up front: write a placeholder header now, patch after.
  if (!writeLocalHeader(name, 0, totalLen)) return false;
  offset += 30 + strlen(name);
  entries.push_back({std::string(name), 0, totalLen, streamHeaderOffset});
  streaming = true;
  return true;
}

bool ZipWriter::writeChunk(const uint8_t* data, size_t len) {
  if (!out || !streaming) return false;
  if (streamWritten + len > streamDeclared) return false;
  streamCrc = (uint32_t)mz_crc32(streamCrc, data, len);
  if (out->write(data, len) != len) return false;
  streamWritten += len;
  offset += (uint32_t)len;
  return true;
}

bool ZipWriter::endStreamedFile() {
  if (!out || !streaming) return false;
  streaming = false;
  if (streamWritten != streamDeclared) {
    LOG_ERR("ZIPW", "streamed entry size mismatch: %lu != %lu", (unsigned long)streamWritten,
            (unsigned long)streamDeclared);
    return false;
  }
  entries.back().crc = streamCrc;
  // Patch the CRC field in the local header (offset +14).
  uint8_t crcBuf[4];
  le32(crcBuf, streamCrc);
  if (!out->seek(streamHeaderOffset + 14)) return false;
  if (out->write(crcBuf, 4) != 4) return false;
  if (!out->seek(offset)) return false;
  return true;
}

bool ZipWriter::finish() {
  if (!out || streaming) return false;
  const uint32_t cdStart = offset;
  for (const auto& e : entries) {
    uint8_t h[46];
    le32(h + 0, 0x02014b50);
    le16(h + 4, 20);  // version made by
    le16(h + 6, 20);  // version needed
    le16(h + 8, 0);   // flags
    le16(h + 10, 0);  // method
    le16(h + 12, DOS_TIME);
    le16(h + 14, DOS_DATE);
    le32(h + 16, e.crc);
    le32(h + 20, e.size);
    le32(h + 24, e.size);
    le16(h + 28, (uint16_t)e.name.size());
    le16(h + 30, 0);  // extra
    le16(h + 32, 0);  // comment
    le16(h + 34, 0);  // disk
    le16(h + 36, 0);  // internal attrs
    le32(h + 38, 0);  // external attrs
    le32(h + 42, e.offset);
    if (out->write(h, sizeof(h)) != sizeof(h)) return false;
    if (out->write(e.name.c_str(), e.name.size()) != e.name.size()) return false;
    offset += 46 + (uint32_t)e.name.size();
  }
  uint8_t eocd[22];
  le32(eocd + 0, 0x06054b50);
  le16(eocd + 4, 0);
  le16(eocd + 6, 0);
  le16(eocd + 8, (uint16_t)entries.size());
  le16(eocd + 10, (uint16_t)entries.size());
  le32(eocd + 12, offset - cdStart);
  le32(eocd + 16, cdStart);
  le16(eocd + 20, 0);
  if (out->write(eocd, sizeof(eocd)) != sizeof(eocd)) return false;
  out->flush();
  out->close();
  out.reset();
  return true;
}
