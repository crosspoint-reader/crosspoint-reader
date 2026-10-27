#include "StoredZipWriter.h"

#include <Logging.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {

constexpr uint16_t kDosDate = ((2024 - 1980) << 9) | (1 << 5) | 1;  // 2024-01-01
constexpr uint16_t kDosTime = 0;

void putLe16(uint8_t* p, uint16_t v) {
  p[0] = v & 0xFF;
  p[1] = v >> 8;
}

void putLe32(uint8_t* p, uint32_t v) {
  p[0] = v & 0xFF;
  p[1] = (v >> 8) & 0xFF;
  p[2] = (v >> 16) & 0xFF;
  p[3] = (v >> 24) & 0xFF;
}

}  // namespace

bool StoredZipWriter::open(const std::string& path) {
  if (!Storage.openFileForWrite("FB2", path, file_)) {
    LOG_ERR("FB2", "zip open failed: %s", path.c_str());
    return false;
  }
  entries_.clear();
  bytesWritten_ = 0;
  entryOpen_ = false;
  started_ = true;
  ok_ = true;
  return true;
}

bool StoredZipWriter::writeAll(const void* data, size_t len) {
  if (!ok_) return false;
  if (len == 0) return true;
  if (file_.write(data, len) != len) {
    ok_ = false;
    LOG_ERR("FB2", "zip write failed (%u bytes)", (unsigned)len);
    return false;
  }
  bytesWritten_ += len;
  return true;
}

bool StoredZipWriter::writeStr(const char* s) { return writeAll(s, strlen(s)); }

bool StoredZipWriter::writeFmt(const char* fmt, ...) {
  char buf[512];
  va_list args;
  va_start(args, fmt);
  const int n = vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  if (n < 0) return false;
  const size_t len = (n >= (int)sizeof(buf)) ? sizeof(buf) - 1 : (size_t)n;
  return writeAll(buf, len);
}

uint32_t StoredZipWriter::crc32Update(uint32_t crc, const uint8_t* data, size_t len) {
  // Reflected CRC-32 (polynomial 0xEDB88320), no table: entries are written
  // once, so the throughput/size trade of a 1 KB table is not worth the RAM.
  crc = ~crc;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int k = 0; k < 8; k++) {
      crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
  }
  return ~crc;
}

bool StoredZipWriter::writeLocalHeader(uint32_t crc, uint32_t size) {
  const uint16_t nameLen = (uint16_t)strlen(entries_.back().name);
  uint8_t header[30];
  putLe32(header + 0, 0x04034B50);   // local file header signature
  putLe16(header + 4, 20);           // version needed (2.0)
  putLe16(header + 6, 0);            // flags
  putLe16(header + 8, 0);            // method: stored
  putLe16(header + 10, kDosTime);
  putLe16(header + 12, kDosDate);
  putLe32(header + 14, crc);
  putLe32(header + 18, size);        // compressed size
  putLe32(header + 22, size);        // uncompressed size
  putLe16(header + 26, nameLen);
  putLe16(header + 28, 0);           // extra field length
  if (!writeAll(header, sizeof(header))) return false;
  return writeAll(entries_.back().name, nameLen);
}

bool StoredZipWriter::startEntry(const char* name) {
  if (!ok_ || entryOpen_) return false;
  if (entries_.size() >= kMaxEntries) {
    LOG_ERR("FB2", "zip entry limit reached");
    ok_ = false;
    return false;
  }
  if (strlen(name) > kMaxNameLength) {
    LOG_ERR("FB2", "zip entry name too long: %s", name);
    ok_ = false;
    return false;
  }
  entries_.emplace_back();
  CentralEntry& e = entries_.back();
  strncpy(e.name, name, sizeof(e.name) - 1);
  e.offset = (uint32_t)bytesWritten_;
  entrySize_ = 0;
  entryCrc_ = 0;
  entryOpen_ = true;
  // Placeholder header; rewritten by finishEntry() with real crc/size.
  return writeLocalHeader(0, 0);
}

bool StoredZipWriter::write(const void* data, size_t len) {
  if (!ok_ || !entryOpen_) return false;
  entryCrc_ = crc32Update(entryCrc_, static_cast<const uint8_t*>(data), len);
  entrySize_ += (uint32_t)len;
  return writeAll(data, len);
}

bool StoredZipWriter::finishEntry() {
  if (!ok_ || !entryOpen_) return false;
  CentralEntry& e = entries_.back();
  e.crc = entryCrc_;
  e.size = entrySize_;
  const uint64_t endPos = bytesWritten_;
  if (!file_.seekSet(e.offset)) {
    ok_ = false;
    LOG_ERR("FB2", "zip seek failed while patching header");
    return false;
  }
  // bytesWritten_ is restored below; writeLocalHeader adds to it, so save the
  // true cursor first and reposition afterwards.
  const uint64_t savedBytes = bytesWritten_;
  bytesWritten_ = e.offset;
  if (!writeLocalHeader(e.crc, e.size)) return false;
  bytesWritten_ = savedBytes;
  if (!file_.seekSet(endPos)) {
    ok_ = false;
    return false;
  }
  entryOpen_ = false;
  return true;
}

bool StoredZipWriter::finish() {
  if (!ok_ || entryOpen_) return false;
  const uint64_t cdOffset = bytesWritten_;
  for (const CentralEntry& e : entries_) {
    uint8_t header[46];
    const uint16_t nameLen = (uint16_t)strlen(e.name);
    putLe32(header + 0, 0x02014B50);  // central directory signature
    putLe16(header + 4, 20);          // version made by
    putLe16(header + 6, 20);          // version needed
    putLe16(header + 8, 0);           // flags
    putLe16(header + 10, 0);          // method: stored
    putLe16(header + 12, kDosTime);
    putLe16(header + 14, kDosDate);
    putLe32(header + 16, e.crc);
    putLe32(header + 20, e.size);
    putLe32(header + 24, e.size);
    putLe16(header + 28, nameLen);
    putLe16(header + 30, 0);          // extra length
    putLe16(header + 32, 0);          // comment length
    putLe16(header + 34, 0);          // disk number
    putLe16(header + 36, 0);          // internal attributes
    putLe32(header + 38, 0);          // external attributes
    putLe32(header + 42, e.offset);
    if (!writeAll(header, sizeof(header))) return false;
    if (!writeAll(e.name, nameLen)) return false;
  }
  const uint64_t cdSize = bytesWritten_ - cdOffset;
  const uint16_t count = (uint16_t)entries_.size();
  uint8_t eocd[22];
  putLe32(eocd + 0, 0x06054B50);
  putLe16(eocd + 4, 0);      // this disk
  putLe16(eocd + 6, 0);      // cd start disk
  putLe16(eocd + 8, count);  // entries this disk
  putLe16(eocd + 10, count);
  putLe32(eocd + 12, (uint32_t)cdSize);
  putLe32(eocd + 16, (uint32_t)cdOffset);
  putLe16(eocd + 20, 0);     // comment length
  if (!writeAll(eocd, sizeof(eocd))) return false;
  file_.flush();
  file_.close();
  ok_ = false;  // finished: no further writes
  return true;
}

void StoredZipWriter::abort() {
  entryOpen_ = false;
  ok_ = false;
  file_.close();
  entries_.clear();
}
