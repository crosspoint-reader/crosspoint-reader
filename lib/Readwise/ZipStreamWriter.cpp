#include "ZipStreamWriter.h"

#include <Logging.h>

#include <cstring>

namespace {
void writeLe16(uint8_t out[2], uint16_t v) {
  out[0] = static_cast<uint8_t>(v & 0xFF);
  out[1] = static_cast<uint8_t>(v >> 8);
}

void writeLe32(uint8_t out[4], uint32_t v) {
  out[0] = static_cast<uint8_t>(v & 0xFF);
  out[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
  out[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
  out[3] = static_cast<uint8_t>(v >> 24);
}

// Offsets inside the 30-byte local file header.
constexpr size_t LHDR_CRC_OFFSET = 14;

bool wrote(const size_t requested, const size_t written) { return requested == written; }
}  // namespace

uint32_t ZipStreamWriter::crc32Update(const uint32_t crc, const uint8_t* data, const size_t len) {
  // Table-less bitwise CRC-32 (polynomial 0xEDB88320): ~1KB less DRAM than a
  // lookup table, and article-sized payloads make the extra cycles irrelevant.
  uint32_t c = crc ^ 0xFFFFFFFFu;
  for (size_t i = 0; i < len; i++) {
    c ^= data[i];
    for (int k = 0; k < 8; k++) {
      c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
  }
  return c ^ 0xFFFFFFFFu;
}

bool ZipStreamWriter::begin(HalFile& file) {
  file_ = &file;
  entryCount_ = 0;
  active_ = -1;
  return true;
}

bool ZipStreamWriter::beginEntry(const char* name) {
  if (!file_ || !file_->isOpen() || active_ >= 0 || entryCount_ >= MAX_ENTRIES) return false;

  EntryInfo& e = entries_[entryCount_];
  const size_t nameLen = strlen(name);
  if (nameLen == 0 || nameLen > MAX_NAME_LEN) return false;
  memcpy(e.name, name, nameLen);
  e.name[nameLen] = '\0';
  e.nameLen = static_cast<uint16_t>(nameLen);
  e.crc = 0;
  e.size = 0;

  uint8_t hdr[30];
  writeLe32(hdr, 0x04034B50u);   // local file header signature
  writeLe16(hdr + 4, 20);        // version needed
  writeLe16(hdr + 6, 0);         // flags
  writeLe16(hdr + 8, 0);         // method: STORE
  writeLe16(hdr + 10, 0);        // mod time
  writeLe16(hdr + 12, 0x21);     // mod date (1980-01-01)
  writeLe32(hdr + 14, 0);        // crc placeholder
  writeLe32(hdr + 18, 0);        // compressed size placeholder
  writeLe32(hdr + 22, 0);        // uncompressed size placeholder
  writeLe16(hdr + 26, e.nameLen);
  writeLe16(hdr + 28, 0);  // extra field length

  const size_t pos = file_->position();
  if (!wrote(sizeof(hdr), file_->write(hdr, sizeof(hdr))) ||
      !wrote(nameLen, file_->write(reinterpret_cast<const uint8_t*>(e.name), nameLen))) {
    LOG_ERR("RWISE", "ZIP header write failed");
    return false;
  }
  e.offset = pos;
  active_ = static_cast<int8_t>(entryCount_);
  return true;
}

bool ZipStreamWriter::write(const uint8_t* data, const size_t len) {
  if (!file_ || active_ < 0 || len == 0) return len == 0;
  EntryInfo& e = entries_[active_];
  if (!wrote(len, file_->write(data, len))) {
    LOG_ERR("RWISE", "ZIP data write failed at %u bytes", (unsigned)e.size);
    return false;
  }
  e.crc = crc32Update(e.crc, data, len);
  e.size += static_cast<uint32_t>(len);
  return true;
}

bool ZipStreamWriter::finishEntry() {
  if (!file_ || active_ < 0) return false;
  EntryInfo& e = entries_[active_];

  uint8_t fields[10];
  writeLe32(fields, e.crc);
  writeLe32(fields + 4, e.size);  // compressed
  writeLe32(fields + 8, e.size);  // uncompressed

  const size_t endPos = file_->position();
  if (!file_->seek(e.offset + LHDR_CRC_OFFSET) || !wrote(sizeof(fields), file_->write(fields, sizeof(fields))) ||
      !file_->seek(endPos)) {
    LOG_ERR("RWISE", "ZIP header patch failed");
    return false;
  }

  entryCount_++;
  active_ = -1;
  return true;
}

bool ZipStreamWriter::finish() {
  if (!file_) return false;

  const size_t cdStart = file_->position();
  for (uint8_t i = 0; i < entryCount_; i++) {
    const EntryInfo& e = entries_[i];
    uint8_t rec[46];
    writeLe32(rec, 0x02014B50u);       // central directory signature
    writeLe16(rec + 4, 20);            // version made by
    writeLe16(rec + 6, 20);            // version needed
    writeLe16(rec + 8, 0);             // flags
    writeLe16(rec + 10, 0);            // method: STORE
    writeLe16(rec + 12, 0);            // mod time
    writeLe16(rec + 14, 0x21);         // mod date
    writeLe32(rec + 16, e.crc);
    writeLe32(rec + 20, e.size);
    writeLe32(rec + 24, e.size);
    writeLe16(rec + 28, e.nameLen);
    writeLe16(rec + 30, 0);            // extra len
    writeLe16(rec + 32, 0);            // comment len
    writeLe16(rec + 34, 0);            // disk number start
    writeLe16(rec + 36, 0);            // internal attrs
    writeLe32(rec + 38, 0);            // external attrs
    writeLe32(rec + 42, e.offset);
    if (!wrote(sizeof(rec), file_->write(rec, sizeof(rec))) ||
        !wrote(e.nameLen, file_->write(reinterpret_cast<const uint8_t*>(e.name), e.nameLen))) {
      LOG_ERR("RWISE", "central dir write failed");
      return false;
    }
  }
  const size_t cdEnd = file_->position();

  uint8_t eocd[22];
  writeLe32(eocd, 0x06054B50u);      // EOCD signature
  writeLe16(eocd + 4, 0);            // disk number
  writeLe16(eocd + 6, 0);            // central dir disk
  writeLe16(eocd + 8, entryCount_);  // entries on this disk
  writeLe16(eocd + 10, entryCount_);
  writeLe32(eocd + 12, cdEnd - cdStart);  // central dir size
  writeLe32(eocd + 16, cdStart);
  writeLe16(eocd + 20, 0);           // comment len

  if (!wrote(sizeof(eocd), file_->write(eocd, sizeof(eocd)))) {
    LOG_ERR("RWISE", "EOCD write failed");
    return false;
  }
  file_->flush();
  return true;
}
