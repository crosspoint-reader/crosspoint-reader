#include "MobiParser.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cstring>

namespace {
// All PDB/MOBI integers are big-endian.
uint16_t be16(const uint8_t* p) { return ((uint16_t)p[0] << 8) | p[1]; }
uint32_t be32(const uint8_t* p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

constexpr uint16_t COMPRESSION_NONE = 1;
constexpr uint16_t COMPRESSION_PALMDOC = 2;
constexpr uint16_t COMPRESSION_HUFF = 17480;  // 0x4448 'DH'

// Sanity caps for file-controlled sizes (records are 4-8 KB in real books;
// EXTH blocks hold a few dozen entries).
constexpr uint32_t MAX_RECORD_BYTES = 1u << 20;
constexpr uint32_t MAX_EXTH_ENTRIES = 1024;

// Trailing-entry size: backward varint over the record's last 4 bytes
// (mobi_unpack getSizeOfTrailingDataEntry). Returned size includes itself.
uint32_t sizeOfTrailingEntry(const uint8_t* data, uint32_t len) {
  uint32_t num = 0;
  const uint32_t start = len > 4 ? len - 4 : 0;
  for (uint32_t i = start; i < len; i++) {
    if (data[i] & 0x80) num = 0;
    num = (num << 7) | (data[i] & 0x7F);
  }
  return num;
}
}  // namespace

MobiParser::MobiParser(std::string p) : path(std::move(p)) {}
MobiParser::~MobiParser() = default;

const char* MobiParser::errorString() const {
  switch (err) {
    case Error::None: return "ok";
    case Error::OpenFailed: return "cannot open file";
    case Error::NotPdb: return "not a MOBI file";
    case Error::Encrypted: return "book is DRM-protected";
    case Error::Kf8Only: return "AZW3/KF8-only book (re-export as MOBI or EPUB)";
    case Error::BadHeader: return "corrupt MOBI header";
    case Error::Decompress: return "decompression failed";
    case Error::Oom: return "out of memory";
  }
  return "unknown";
}

bool MobiParser::load() {
  file = std::make_unique<HalFile>();
  if (!Storage.openFileForRead("MOBI", path.c_str(), *file)) {
    err = Error::OpenFailed;
    return false;
  }
  if (!readRecordOffsets()) return false;
  if (!parseRecord0()) return false;
  if (compression == COMPRESSION_HUFF && !parseHuffCdic()) return false;
  return true;
}

bool MobiParser::readRecordOffsets() {
  uint8_t hdr[78];
  if (!file->seek(0) || file->read(hdr, sizeof(hdr)) != (int)sizeof(hdr)) {
    err = Error::NotPdb;
    return false;
  }
  if (memcmp(hdr + 60, "BOOK", 4) != 0 || memcmp(hdr + 64, "MOBI", 4) != 0) {
    err = Error::NotPdb;
    return false;
  }
  numRecords = be16(hdr + 76);
  if (numRecords < 2) {
    LOG_ERR("MOBI", "bad header: numRecords=%u", numRecords);
    err = Error::BadHeader;
    return false;
  }
  recordOffsets.clear();
  recordOffsets.reserve(numRecords + 1);
  // Record list: 8 bytes per record, offset in the first 4.
  std::vector<uint8_t> list(numRecords * 8);
  if (file->read(list.data(), list.size()) != (int)list.size()) {
    LOG_ERR("MOBI", "bad header: record list read failed (%u records)", numRecords);
    err = Error::BadHeader;
    return false;
  }
  for (uint16_t i = 0; i < numRecords; i++) recordOffsets.push_back(be32(&list[i * 8]));
  recordOffsets.push_back((uint32_t)file->fileSize());
  // Offsets must be non-decreasing and inside the file. Equality is legal:
  // PDB permits zero-length records (padding/EOF markers, common in large
  // builds like Bibles) — only strictly decreasing offsets are corrupt.
  for (uint16_t i = 0; i < numRecords; i++) {
    if (recordOffsets[i] > recordOffsets[i + 1]) {
      LOG_ERR("MOBI", "bad header: record %u offset %lu > next %lu", i, (unsigned long)recordOffsets[i],
              (unsigned long)recordOffsets[i + 1]);
      err = Error::BadHeader;
      return false;
    }
  }
  return true;
}

bool MobiParser::readRecord(uint16_t index, std::vector<uint8_t>& out) {
  if (index >= numRecords) return false;
  const uint32_t len = recordOffsets[index + 1] - recordOffsets[index];
  // Record lengths come straight from the file. A corrupt table can claim
  // hundreds of MB; resize() would abort() (no exceptions) long before the
  // allocator returns. MOBI records are 4-8 KB in practice.
  if (len > MAX_RECORD_BYTES) {
    LOG_ERR("MOBI", "record %u absurd length %lu", index, (unsigned long)len);
    err = Error::BadHeader;
    return false;
  }
  out.clear();
  out.resize(len);
  if (out.size() != len) {
    err = Error::Oom;
    return false;
  }
  if (!file->seek(recordOffsets[index])) return false;
  return file->read(out.data(), len) == (int)len;
}

bool MobiParser::parseRecord0() {
  std::vector<uint8_t> rec;
  if (!readRecord(0, rec) || rec.size() < 16) {
    LOG_ERR("MOBI", "bad header: record0 unreadable (%u bytes)", (unsigned)rec.size());
    err = Error::BadHeader;
    return false;
  }
  compression = be16(&rec[0]);
  textLength = be32(&rec[4]);
  textRecordCount = be16(&rec[8]);
  recordSize = be16(&rec[10]);
  const uint16_t encryption = be16(&rec[12]);
  if (encryption != 0) {
    err = Error::Encrypted;
    return false;
  }
  if (compression != COMPRESSION_NONE && compression != COMPRESSION_PALMDOC && compression != COMPRESSION_HUFF) {
    LOG_ERR("MOBI", "bad header: unknown compression %u", compression);
    err = Error::BadHeader;
    return false;
  }

  if (rec.size() < 16 + 232 || memcmp(&rec[16], "MOBI", 4) != 0) {
    // PalmDOC-only files have no MOBI header; treat as minimal book.
    bookTitle = path;
    return true;
  }
  const uint32_t mobiLen = be32(&rec[20]);
  const uint32_t mobiVersion = be32(&rec[36]);  // "file version" @0x14 from MOBI start
  textEncoding = be32(&rec[28]);

  if (mobiVersion >= 8) {
    err = Error::Kf8Only;
    return false;
  }

  const uint32_t fullNameOffset = be32(&rec[84]);
  const uint32_t fullNameLength = be32(&rec[88]);
  // Subtract, never add: offset+length is uint32 arithmetic and wraps, which
  // would let a crafted header pass the check and read before the buffer.
  if (fullNameLength > 0 && fullNameLength < 512 && fullNameOffset <= rec.size() &&
      fullNameLength <= rec.size() - fullNameOffset) {
    bookTitle.assign((const char*)&rec[fullNameOffset], fullNameLength);
  }

  const uint32_t firstImage = be32(&rec[108]);
  if (firstImage < numRecords) firstImageRecord = (uint16_t)firstImage;
  huffRecord = be32(&rec[112]);
  huffCount = be32(&rec[116]);
  const uint32_t exthFlags = be32(&rec[128]);

  // First/last content record: 0xC0/0xC2 from MOBI start = 208/210 absolute.
  if (rec.size() >= 212) lastContentRecord = be16(&rec[210]);

  extraDataFlags = 0;
  if (mobiLen >= 0xE4 && mobiVersion >= 5 && rec.size() >= 0xF4) {
    extraDataFlags = be16(&rec[0xF2]);
  }

  // EXTH: follows the MOBI header.
  if (exthFlags & 0x40) {
    const uint32_t exthStart = 16 + mobiLen;
    if (mobiLen <= rec.size() && exthStart <= rec.size() && rec.size() - exthStart >= 12 &&
        memcmp(&rec[exthStart], "EXTH", 4) == 0) {
      uint32_t count = be32(&rec[exthStart + 8]);
      // Real EXTH blocks hold a few dozen entries; a wrapped/garbage count
      // would otherwise spin for billions of iterations.
      if (count > MAX_EXTH_ENTRIES) count = MAX_EXTH_ENTRIES;
      uint32_t pos = exthStart + 12;
      for (uint32_t i = 0; i < count && pos <= rec.size() && rec.size() - pos >= 8; i++) {
        const uint32_t type = be32(&rec[pos]);
        const uint32_t len = be32(&rec[pos + 4]);
        // Subtraction form again (pos+len wraps); len >= 8 keeps pos advancing.
        if (len < 8 || len > rec.size() - pos) break;
        const uint8_t* data = &rec[pos + 8];
        const uint32_t dataLen = len - 8;
        if (type == 100 && bookAuthor.empty() && dataLen < 256) {
          bookAuthor.assign((const char*)data, dataLen);
        } else if (type == 503 && dataLen > 0 && dataLen < 512) {
          bookTitle.assign((const char*)data, dataLen);
        } else if (type == 201 && dataLen >= 4 && len >= 12) {
          const uint32_t off = be32(data);
          if (off != 0xFFFFFFFF) coverOffset = (int32_t)off + 1;  // to 1-based recindex
        }
        pos += len;
      }
    }
  }

  if (bookTitle.empty()) bookTitle = "Untitled";
  return true;
}

bool MobiParser::parseHuffCdic() {
  if (huffRecord == 0 || huffCount < 2 || huffRecord + huffCount > numRecords) {
    LOG_ERR("MOBI", "bad header: huff rec=%lu count=%lu of %u", (unsigned long)huffRecord, (unsigned long)huffCount, numRecords);
    err = Error::BadHeader;
    return false;
  }
  huff = std::make_unique<HuffCdic>();

  std::vector<uint8_t> rec;
  if (!readRecord((uint16_t)huffRecord, rec) || rec.size() < 24 || memcmp(rec.data(), "HUFF", 4) != 0) {
    LOG_ERR("MOBI", "bad header: HUFF record invalid");
    err = Error::BadHeader;
    return false;
  }
  const uint32_t off1 = be32(&rec[8]);
  const uint32_t off2 = be32(&rec[12]);
  // Subtraction form: off + size is uint32 and wraps on crafted offsets.
  if (rec.size() < 256 * 4 || rec.size() < 64 * 4 || off1 > rec.size() - 256 * 4 ||
      off2 > rec.size() - 64 * 4) {
    LOG_ERR("MOBI", "bad header: HUFF offsets %lu/%lu size %u", (unsigned long)off1, (unsigned long)off2, (unsigned)rec.size());
    err = Error::BadHeader;
    return false;
  }
  for (int i = 0; i < 256; i++) huff->table1[i] = be32(&rec[off1 + i * 4]);
  // dict2: 32 (mincode, maxcode) pairs for code lengths 1..32.
  huff->mincode[0] = 0;
  huff->maxcode[0] = 0xFFFFFFFF;
  for (int codelen = 1; codelen <= 32; codelen++) {
    const uint32_t minc = be32(&rec[off2 + (codelen - 1) * 8]);
    const uint32_t maxc = be32(&rec[off2 + (codelen - 1) * 8 + 4]);
    huff->mincode[codelen] = (codelen < 32) ? (minc << (32 - codelen)) : minc;
    // ((maxc + 1) << (32 - codelen)) - 1, without UB at codelen == 32
    huff->maxcode[codelen] = (codelen < 32) ? ((((uint64_t)maxc + 1) << (32 - codelen)) - 1) : maxc;
  }

  // CDIC phrase dictionaries.
  for (uint32_t i = 1; i < huffCount; i++) {
    std::vector<uint8_t> cdic;
    if (!readRecord((uint16_t)(huffRecord + i), cdic) || cdic.size() < 16 || memcmp(cdic.data(), "CDIC", 4) != 0) {
      LOG_ERR("MOBI", "bad header: CDIC %lu invalid", (unsigned long)i);
      err = Error::BadHeader;
      return false;
    }
    if (huff->codeLength == 0) huff->codeLength = be32(&cdic[12]);
    huff->dicts.push_back(std::move(cdic));
  }
  if (huff->codeLength == 0 || huff->codeLength > 16) {
    LOG_ERR("MOBI", "bad header: CDIC codeLength %lu", (unsigned long)huff->codeLength);
    err = Error::BadHeader;
    return false;
  }
  huff->ready = true;
  return true;
}

uint32_t MobiParser::trimTrailingEntries(const uint8_t* rec, uint32_t len) const {
  uint32_t flags = extraDataFlags;
  const bool multibyte = flags & 1;
  flags >>= 1;
  while (flags) {
    if (flags & 1) {
      const uint32_t num = sizeOfTrailingEntry(rec, len);
      if (num == 0 || num >= len) break;
      len -= num;
    }
    flags >>= 1;
  }
  if (multibyte && len > 0) {
    const uint32_t num = (rec[len - 1] & 3) + 1;
    len = (num < len) ? len - num : 0;
  }
  return len;
}

uint32_t MobiParser::decompressPalmDoc(const uint8_t* src, uint32_t srcLen, uint8_t* dst, uint32_t dstCap) const {
  uint32_t i = 0, o = 0;
  while (i < srcLen && o < dstCap) {
    const uint8_t c = src[i++];
    if (c == 0) {
      dst[o++] = 0;
    } else if (c <= 8) {
      for (uint8_t k = 0; k < c && i < srcLen && o < dstCap; k++) dst[o++] = src[i++];
    } else if (c <= 0x7F) {
      dst[o++] = c;
    } else if (c <= 0xBF) {
      if (i >= srcLen) break;
      const uint16_t pair = ((uint16_t)c << 8) | src[i++];
      const uint16_t dist = (pair >> 3) & 0x7FF;
      uint16_t count = (pair & 7) + 3;
      if (dist == 0 || dist > o) break;
      while (count-- && o < dstCap) {
        dst[o] = dst[o - dist];
        o++;
      }
    } else {
      dst[o++] = ' ';
      if (o < dstCap) dst[o++] = c ^ 0x80;
    }
  }
  return o;
}

uint32_t MobiParser::decompressHuffCdic(const uint8_t* src, uint32_t srcLen, uint8_t* dst, uint32_t dstCap) const {
  if (!huff || !huff->ready) return 0;

  // Recursive phrase expansion, depth-limited (dictionary phrases may
  // themselves be compressed sequences flagged non-terminal).
  struct Expander {
    const MobiParser* self;
    uint8_t* dst;
    uint32_t cap;
    uint32_t out = 0;
    bool fail = false;

    void emitPhrase(uint32_t r, int depth) {
      const HuffCdic& h = *self->huff;
      const uint32_t perDict = 1u << h.codeLength;
      const uint32_t dictIdx = r / perDict;
      const uint32_t local = r % perDict;
      if (dictIdx >= h.dicts.size()) {
        fail = true;
        return;
      }
      const std::vector<uint8_t>& cdic = h.dicts[dictIdx];
      const uint32_t idxOff = 16 + local * 2;
      if (idxOff + 2 > cdic.size()) {
        fail = true;
        return;
      }
      const uint32_t phraseOff = 16 + be16(&cdic[idxOff]);
      if (phraseOff + 2 > cdic.size()) {
        fail = true;
        return;
      }
      const uint16_t blen = be16(&cdic[phraseOff]);
      const uint32_t plen = blen & 0x7FFF;
      const bool expanded = blen & 0x8000;
      if (phraseOff + 2 + plen > cdic.size()) {
        fail = true;
        return;
      }
      const uint8_t* p = &cdic[phraseOff + 2];
      if (expanded) {
        for (uint32_t k = 0; k < plen && out < cap; k++) dst[out++] = p[k];
      } else {
        if (depth > 8) {
          fail = true;
          return;
        }
        decode(p, plen, depth + 1);
      }
    }

    void decode(const uint8_t* data, uint32_t len, int depth) {
      const HuffCdic& h = *self->huff;
      int64_t bitsleft = (int64_t)len * 8;
      uint32_t pos = 0;
      uint64_t x = 0;
      // Prime a 64-bit window (zero-padded past the end).
      auto load64 = [&](uint32_t bytePos) {
        uint64_t v = 0;
        for (int k = 0; k < 8; k++) {
          v <<= 8;
          if (bytePos + k < len) v |= data[bytePos + k];
        }
        return v;
      };
      x = load64(0);
      int n = 32;
      while (!fail && out < cap) {
        if (n <= 0) {
          pos += 4;
          x = load64(pos);
          n += 32;
        }
        const uint32_t code = (uint32_t)((x >> n) & 0xFFFFFFFFull);
        const uint32_t v = h.table1[code >> 24];
        uint32_t codelen = v & 0x1F;
        const bool term = v & 0x80;
        uint32_t maxcode;
        if (term) {
          maxcode = ((((uint64_t)(v >> 8)) + 1) << (32 - codelen)) - 1;
        } else {
          while (codelen < 32 && code < h.mincode[codelen]) codelen++;
          maxcode = h.maxcode[codelen];
        }
        if (codelen == 0 || codelen > 32) {
          fail = true;
          return;
        }
        n -= (int)codelen;
        bitsleft -= codelen;
        if (bitsleft < 0) return;  // normal end of stream
        const uint32_t r = (uint32_t)(((uint64_t)maxcode - code) >> (32 - codelen));
        emitPhrase(r, depth);
      }
    }
  };

  Expander e{this, dst, dstCap};
  e.decode(src, srcLen, 0);
  return e.fail ? 0 : e.out;
}

uint32_t MobiParser::readRawText(uint8_t* dst, uint32_t dstLen) {
  if (!file) return 0;
  uint32_t written = 0;
  std::vector<uint8_t> rec;
  for (uint16_t i = 1; i <= textRecordCount && written < dstLen; i++) {
    if (!readRecord(i, rec)) {
      err = Error::Decompress;
      return 0;
    }
    const uint32_t payload = trimTrailingEntries(rec.data(), (uint32_t)rec.size());
    uint32_t n = 0;
    switch (compression) {
      case COMPRESSION_NONE:
        n = payload < dstLen - written ? payload : dstLen - written;
        memcpy(dst + written, rec.data(), n);
        break;
      case COMPRESSION_PALMDOC:
        n = decompressPalmDoc(rec.data(), payload, dst + written, dstLen - written);
        break;
      case COMPRESSION_HUFF:
        n = decompressHuffCdic(rec.data(), payload, dst + written, dstLen - written);
        if (n == 0 && payload > 0) {
          err = Error::Decompress;
          return 0;
        }
        break;
    }
    written += n;
  }
  return written;
}

uint16_t MobiParser::imageCount() const {
  if (firstImageRecord == 0 || firstImageRecord >= numRecords) return 0;
  const uint16_t last = (lastContentRecord && lastContentRecord < numRecords) ? lastContentRecord : (numRecords - 1);
  return last >= firstImageRecord ? (last - firstImageRecord + 1) : 0;
}

std::vector<uint8_t> MobiParser::readImage(uint16_t recindex) {
  std::vector<uint8_t> out;
  if (recindex == 0 || firstImageRecord == 0) return out;
  const uint32_t abs = (uint32_t)firstImageRecord + recindex - 1;
  if (abs >= numRecords) return out;
  if (!readRecord((uint16_t)abs, out)) out.clear();
  // Only pass through real raster formats (records here also hold FLIS/FCIS
  // and other metadata records that are not images).
  if (out.size() >= 4) {
    const bool jpeg = out[0] == 0xFF && out[1] == 0xD8;
    const bool png = out[0] == 0x89 && out[1] == 'P';
    const bool gif = out[0] == 'G' && out[1] == 'I' && out[2] == 'F';
    const bool bmp = out[0] == 'B' && out[1] == 'M';
    if (!jpeg && !png && !gif && !bmp) out.clear();
  } else {
    out.clear();
  }
  return out;
}
