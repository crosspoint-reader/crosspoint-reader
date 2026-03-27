#include "XrefTable.h"

#include "PdfObject.h"
#include "StreamDecoder.h"
#include "PdfLog.h"

#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <cctype>
#include <cstring>
#include <string_view>
#include <utility>
#include <vector>

namespace {

bool matchBytes(const uint8_t* p, size_t n, const char* lit, size_t litLen) {
  if (n < litLen) return false;
  return std::memcmp(p, lit, litLen) == 0;
}

size_t findStartXref(const uint8_t* buf, size_t len) {
  static const char k[] = "startxref";
  constexpr size_t kLen = 9;
  if (len < kLen) return SIZE_MAX;
  for (size_t i = 0; i + kLen <= len; ++i) {
    if (matchBytes(buf + i, len - i, k, kLen)) {
      return i;
    }
  }
  return SIZE_MAX;
}

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\0'; }

void skipWs(const char*& p) {
  while (*p && isSpace(*p)) ++p;
}

void skipWs(char*& p) {
  while (*p && isSpace(*p)) ++p;
}

bool readLineFromFile(FsFile& file, char* out, size_t outCap, size_t& outLen) {
  outLen = 0;
  while (outLen + 1 < outCap) {
    int c = file.read();
    if (c < 0) break;
    if (c == '\r') {
      int c2 = file.read();
      if (c2 >= 0 && c2 != '\n') {
        file.seekCur(-1);
      }
      break;
    }
    if (c == '\n') break;
    out[outLen++] = static_cast<char>(c);
  }
  out[outLen] = '\0';
  return true;
}

uint32_t readBe24(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 16) | (static_cast<uint32_t>(p[1]) << 8) | static_cast<uint32_t>(p[2]);
}

uint32_t readBe16(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 8) | static_cast<uint32_t>(p[1]);
}

static constexpr size_t kPackedOffsetBytes = 3;

template <size_t N>
bool appendUnsigned(PdfFixedString<N>& s, uint32_t value) {
  char buf[16];
  size_t len = 0;
  do {
    if (len >= sizeof(buf)) {
      return false;
    }
    buf[len++] = static_cast<char>('0' + (value % 10U));
    value /= 10U;
  } while (value != 0U);
  while (len > 0) {
    if (!s.append(&buf[len - 1], 1)) {
      return false;
    }
    --len;
  }
  return true;
}

uint32_t loadPackedOffset(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16);
}

void storePackedOffset(uint8_t* p, uint32_t off) {
  p[0] = static_cast<uint8_t>(off & 0xFFU);
  p[1] = static_cast<uint8_t>((off >> 8) & 0xFFU);
  p[2] = static_cast<uint8_t>((off >> 16) & 0xFFU);
}

bool parseBracketInts(std::string_view src, PdfFixedVector<int, 32>& out) {
  out.clear();
  const size_t lb = src.find('[');
  if (lb == std::string_view::npos) return false;
  const size_t rb = src.find(']', lb + 1);
  if (rb == std::string_view::npos) return false;
  const char* p = src.data() + lb + 1;
  const char* end = src.data() + rb;
  while (p < end) {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
    if (p >= end) break;
    char* e = nullptr;
    const long n = std::strtol(p, &e, 10);
    if (e == p) return false;
    if (!out.push_back(static_cast<int>(n))) return false;
    p = e;
  }
  return !out.empty();
}

size_t findStreamKeywordSlice(std::string_view s) {
  static const char pat[] = "\nstream";
  constexpr size_t plen = 7;
  size_t pos = 0;
  while (pos + plen <= s.size()) {
    if (std::memcmp(s.data() + pos, pat, plen) == 0) {
      return pos + 1;
    }
    if (pos + 8 <= s.size() && s[pos] == '\r' && s[pos + 1] == '\n' &&
        std::memcmp(s.data() + pos + 2, "stream", 6) == 0) {
      return pos + 2;
    }
    ++pos;
  }
  return std::string_view::npos;
}

void trimWsBothFs(PdfFixedString<PDF_INLINE_DICT_MAX>& s) {
  while (s.size() > 0 && (s[0] == ' ' || s[0] == '\t' || s[0] == '\r' || s[0] == '\n')) {
    s.erase_prefix(1);
  }
  while (s.size() > 0) {
    const char c = s[s.size() - 1];
    if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
    s.resize(s.size() - 1);
  }
}

bool splitObjStmObjectSlice(std::string_view slice, PdfFixedString<PDF_INLINE_DICT_MAX>& dictOut, PdfByteBuffer& streamOut) {
  streamOut.clear();
  const size_t sp = findStreamKeywordSlice(slice);
  if (sp == std::string_view::npos) {
    if (!dictOut.assign(slice.data(), slice.size())) return false;
    trimWsBothFs(dictOut);
    return !dictOut.empty();
  }
  if (!dictOut.assign(slice.data(), sp)) return false;
  while (dictOut.size() > 0 && (dictOut[dictOut.size() - 1] == ' ' || dictOut[dictOut.size() - 1] == '\t' ||
                                dictOut[dictOut.size() - 1] == '\r' || dictOut[dictOut.size() - 1] == '\n')) {
    dictOut.resize(dictOut.size() - 1);
  }
  size_t dataIdx = sp + 6;
  while (dataIdx < slice.size() && (slice[dataIdx] == '\r' || slice[dataIdx] == '\n')) {
    ++dataIdx;
  }
  const size_t ep = slice.rfind("endstream");
  if (ep == std::string_view::npos || ep < dataIdx) {
    return false;
  }
  const size_t stmLen = ep - dataIdx;
  if (!streamOut.resize(stmLen)) return false;
  std::memcpy(streamOut.ptr(), slice.data() + dataIdx, stmLen);
  return !dictOut.empty();
}

struct ObjStmDecodeAccum {
  std::vector<uint8_t> bytes;
};

[[maybe_unused]] bool appendObjStmBytes(void* ctx, const uint8_t* data, size_t len) {
  auto* out = static_cast<ObjStmDecodeAccum*>(ctx);
  if (!out) return false;
  out->bytes.insert(out->bytes.end(), data, data + len);
  return true;
}

[[maybe_unused]] bool parseObjStmPairToken(const uint8_t* buf, size_t len, size_t limit, size_t& pos, uint32_t& out) {
  while (pos < limit && pos < len && (buf[pos] == ' ' || buf[pos] == '\t' || buf[pos] == '\r' || buf[pos] == '\n')) {
    ++pos;
  }
  if (pos >= limit || pos >= len) {
    return false;
  }
  if (buf[pos] < '0' || buf[pos] > '9') {
    return false;
  }
  uint32_t val = 0;
  while (pos < limit && pos < len) {
    const uint8_t c = buf[pos];
    if (c < '0' || c > '9') {
      break;
    }
    const uint32_t next = val * 10U + static_cast<uint32_t>(c - '0');
    if (next < val) {
      return false;
    }
    val = next;
    ++pos;
  }
  out = val;
  return true;
}

bool scanObjStmHeaderFromBytes(const uint8_t* bytes, size_t len, size_t first, int nObj, uint32_t targetObjId,
                               uint32_t& targetRel, uint32_t& nextRel) {
  if (!bytes || len <= first || nObj <= 0) {
    return false;
  }
  targetRel = 0;
  nextRel = UINT32_MAX;
  bool targetFound = false;
  size_t pos = 0;
  for (int i = 0; i < nObj; ++i) {
    uint32_t objId = 0;
    uint32_t rel = 0;
    if (!parseObjStmPairToken(bytes, len, first, pos, objId) || !parseObjStmPairToken(bytes, len, first, pos, rel)) {
      return false;
    }
    if (objId == targetObjId) {
      targetFound = true;
      targetRel = rel;
    } else if (targetFound && rel > targetRel && rel < nextRel) {
      nextRel = rel;
    }
  }
  return targetFound;
}

[[maybe_unused]] bool readObjStmUnsignedToken(FsFile& file, size_t limit, uint32_t& out) {
  out = 0;
  bool haveDigit = false;
  while (file.position() < limit) {
    const int ch = file.read();
    if (ch < 0) {
      return false;
    }
    if (ch >= '0' && ch <= '9') {
      haveDigit = true;
      out = static_cast<uint32_t>(out * 10U + static_cast<uint32_t>(ch - '0'));
      continue;
    }
    if (haveDigit) {
      return true;
    }
  }
  return haveDigit;
}

[[maybe_unused]] bool scanObjStmHeaderForTarget(FsFile& file, size_t first, int nObj, uint32_t targetObjId,
                                                uint32_t& targetRel) {
  if (!file.seek(0)) {
    return false;
  }
  for (int i = 0; i < nObj; ++i) {
    uint32_t oid = 0;
    uint32_t rel = 0;
    if (!readObjStmUnsignedToken(file, first, oid) || !readObjStmUnsignedToken(file, first, rel)) {
      return false;
    }
    if (oid == targetObjId) {
      targetRel = rel;
      return true;
    }
  }
  return false;
}

[[maybe_unused]] bool scanObjStmHeaderForNext(FsFile& file, size_t first, int nObj, uint32_t targetRel,
                                              uint32_t& nextRel) {
  if (!file.seek(0)) {
    return false;
  }
  nextRel = 0;
  for (int i = 0; i < nObj; ++i) {
    [[maybe_unused]] uint32_t oid = 0;
    uint32_t rel = 0;
    if (!readObjStmUnsignedToken(file, first, oid) || !readObjStmUnsignedToken(file, first, rel)) {
      return false;
    }
    if (rel > targetRel && (nextRel == 0 || rel < nextRel)) {
      nextRel = rel;
    }
  }
  return true;
}

struct StreamToFileCtx {
  FsFile* file = nullptr;
};

[[maybe_unused]] bool writeChunkToFile(void* ctx, const uint8_t* data, size_t len) {
  auto* s = static_cast<StreamToFileCtx*>(ctx);
  return s && s->file && s->file->write(data, len) == len;
}

struct ClassicXrefMeta {
  uint32_t root = 0;
  uint32_t prev = 0;
  uint32_t declaredSize = 0;
};

bool captureTrailingNumber(std::string_view src, const char* key, uint32_t& out) {
  const size_t pos = src.find(key);
  if (pos == std::string_view::npos) {
    return false;
  }

  const size_t keyEnd = pos + std::strlen(key);
  size_t v = keyEnd;
  while (v < src.size() && (src[v] == ' ' || src[v] == '\t' || src[v] == '\r' || src[v] == '\n')) {
    ++v;
  }
  if (v >= src.size()) {
    return false;
  }

  const char* p = src.data() + v;
  char* end = nullptr;
  const unsigned long n = std::strtoul(p, &end, 10);
  if (end == p) {
    return false;
  }
  if (end >= src.data() + src.size()) {
    return false;
  }
  const char term = *end;
  if (!(term == ' ' || term == '\t' || term == '\r' || term == '\n' || term == '/' || term == '>' || term == '<' ||
        term == '[' || term == ']' || term == '(' || term == ')')) {
    return false;
  }

  out = static_cast<uint32_t>(n);
  return true;
}

bool scanClassicTrailerMeta(FsFile& file, size_t fileSize, ClassicXrefMeta& meta) {
  constexpr size_t kChunk = 256;
  constexpr size_t kWindow = 1024;
  PdfFixedString<kWindow> trailer;
  char chunk[kChunk];

  for (int safety = 0; safety < 256 && file.position() < fileSize; ++safety) {
    const size_t remaining = fileSize - file.position();
    const size_t want = std::min(remaining, kChunk);
    const int rd = file.read(reinterpret_cast<uint8_t*>(chunk), want);
    if (rd <= 0) {
      break;
    }
    if (!trailer.append(chunk, static_cast<size_t>(rd))) {
      return false;
    }

    const std::string_view view = trailer.view();
    if (meta.root == 0) {
      captureTrailingNumber(view, "/Root", meta.root);
    }
    if (meta.prev == 0) {
      captureTrailingNumber(view, "/Prev", meta.prev);
    }
    if (meta.declaredSize == 0) {
      uint32_t sizeVal = 0;
      if (captureTrailingNumber(view, "/Size", sizeVal) && sizeVal > 0 && sizeVal < 2000000) {
        meta.declaredSize = sizeVal;
      }
    }

    if (view.find("startxref") != std::string_view::npos) {
      return true;
    }

    if (trailer.size() > kWindow / 2) {
      const size_t keep = kWindow / 2;
      trailer.erase_prefix(trailer.size() - keep);
    }
  }

  return meta.root != 0 || meta.prev != 0 || meta.declaredSize != 0;
}

bool readClassicXrefMeta(FsFile& file, size_t fileSize, uint32_t xrefOff, ClassicXrefMeta& meta) {
  meta = {};
  if (xrefOff >= fileSize || !file.seek(xrefOff)) {
    return false;
  }

  char lineBuf[128];
  size_t lineLen = 0;
  readLineFromFile(file, lineBuf, sizeof(lineBuf), lineLen);
  if (std::strncmp(lineBuf, "xref", 4) != 0) {
    return false;
  }

  for (;;) {
    readLineFromFile(file, lineBuf, sizeof(lineBuf), lineLen);
      if (lineLen == 0) {
        if (file.position() >= fileSize) {
        pdfLogErr("xref: unexpected EOF before trailer");
        return false;
      }
      continue;
    }
    if (lineLen >= 7 && std::strncmp(lineBuf, "trailer", 7) == 0 &&
        (lineBuf[7] == '\0' || lineBuf[7] == ' ' || lineBuf[7] == '\t')) {
      return scanClassicTrailerMeta(file, fileSize, meta);
    }

    char* endp = nullptr;
    const unsigned long firstObj = strtoul(lineBuf, &endp, 10);
    skipWs(endp);
    const unsigned long count = strtoul(endp, &endp, 10);
    if (count > 100000 || firstObj + count > 2000000) {
      pdfLogErr("xref: bad subsection");
      return false;
    }
    if (count == 0) continue;
    constexpr size_t kRowLen = 20;
    if (!file.seek(file.position() + static_cast<size_t>(count) * kRowLen)) {
      return false;
    }
  }
}

bool applyClassicXrefSection(FsFile& file, size_t fileSize, uint32_t xrefOff, XrefTable& xref, ClassicXrefMeta& meta) {
  meta = {};
  if (xrefOff >= fileSize || !file.seek(xrefOff)) {
    return false;
  }

  char lineBuf[128];
  size_t lineLen = 0;
  readLineFromFile(file, lineBuf, sizeof(lineBuf), lineLen);
  if (std::strncmp(lineBuf, "xref", 4) != 0) {
    return false;
  }

  for (;;) {
    readLineFromFile(file, lineBuf, sizeof(lineBuf), lineLen);
    if (lineLen == 0) {
      if (file.position() >= fileSize) {
        pdfLogErr("xref: unexpected EOF before trailer");
        return false;
      }
      continue;
    }
    if (lineLen >= 7 && std::strncmp(lineBuf, "trailer", 7) == 0 &&
        (lineBuf[7] == '\0' || lineBuf[7] == ' ' || lineBuf[7] == '\t')) {
      return scanClassicTrailerMeta(file, fileSize, meta);
    }

    char* endp = nullptr;
    const unsigned long firstObj = strtoul(lineBuf, &endp, 10);
    skipWs(endp);
    const unsigned long count = strtoul(endp, &endp, 10);
    if (count > 100000 || firstObj + count > 2000000) {
      pdfLogErr("xref: bad subsection");
      return false;
    }
    if (count == 0) continue;
    constexpr size_t kRowLen = 20;
    for (unsigned long i = 0; i < count; ++i) {
      char row[kRowLen];
      const int rd = file.read(reinterpret_cast<uint8_t*>(row), kRowLen);
      if (rd != static_cast<int>(kRowLen)) {
        pdfLogErr("xref: short read entries");
        return false;
      }
      if (row[10] != ' ' || row[16] != ' ') {
        pdfLogErr("xref: bad entry row");
        return false;
      }
      char offStr[11];
      std::memcpy(offStr, row, 10);
      offStr[10] = '\0';
      const char flag = row[17];
      const uint32_t idx = static_cast<uint32_t>(firstObj + i);
      const uint32_t offVal = (flag == 'n') ? static_cast<uint32_t>(strtoul(offStr, nullptr, 10)) : 0;
      if (!xref.setOffset(idx, offVal)) {
        return false;
      }
    }
  }
}

bool mergeClassicXrefChain(FsFile& file, size_t fileSize, uint32_t xrefOff, XrefTable& xref, uint32_t& rootOut) {
  PdfFixedVector<uint32_t, PDF_MAX_XREF_CHAIN_DEPTH + 1> chain;
  uint32_t cur = xrefOff;
  for (size_t depth = 0; depth < PDF_MAX_XREF_CHAIN_DEPTH + 1 && cur != 0; ++depth) {
    ClassicXrefMeta meta{};
    if (!readClassicXrefMeta(file, fileSize, cur, meta)) {
      return false;
    }
    if (!chain.push_back(cur)) {
      pdfLogErr("xref: /Prev chain too deep");
      return false;
    }
    if (meta.prev == 0 || meta.prev >= fileSize) {
      break;
    }
    cur = meta.prev;
  }
  if (chain.empty()) {
    return false;
  }

  for (int i = static_cast<int>(chain.size()) - 1; i >= 0; --i) {
    ClassicXrefMeta meta{};
    if (!applyClassicXrefSection(file, fileSize, chain[static_cast<size_t>(i)], xref, meta)) {
      return false;
    }
    if (meta.declaredSize > 0 && meta.declaredSize <= PDF_MAX_OBJECTS) {
      xref.ensureOffsetCount(meta.declaredSize);
    }
    if (meta.root != 0) {
      rootOut = meta.root;
    }
  }
  return true;
}

}  // namespace

namespace {

struct XrefStreamDecodeState {
  XrefTable* xref = nullptr;
  uint32_t rangeStart = 0;
  uint32_t rangeLeft = 0;
  uint16_t rangeCount = 0;
  uint16_t currentRange = 0;
  uint32_t rangeStarts[16]{};
  uint32_t rangeCounts[16]{};
  size_t w0 = 0;
  size_t w1 = 0;
  size_t w2 = 0;
  size_t recBytes = 0;
  uint8_t rowBuf[32]{};
  size_t rowPos = 0;
  uint8_t objStmBits[(PDF_MAX_OBJECTS + 7) / 8]{};
};

void setBit(uint8_t* bits, uint32_t id) {
  bits[id >> 3U] |= static_cast<uint8_t>(1U << (id & 7U));
}

bool takeXrefStreamChunk(void* ctx, const uint8_t* data, size_t len) {
  auto* s = static_cast<XrefStreamDecodeState*>(ctx);
  while (len > 0) {
    const size_t need = s->recBytes - s->rowPos;
    const size_t take = std::min(need, len);
    std::memcpy(s->rowBuf + s->rowPos, data, take);
    s->rowPos += take;
    data += take;
    len -= take;
    if (s->rowPos < s->recBytes) {
      continue;
    }
    if (s->rangeLeft == 0) {
      if (s->currentRange + 1 >= s->rangeCount) {
        return false;
      }
      ++s->currentRange;
      s->rangeStart = s->rangeStarts[s->currentRange];
      s->rangeLeft = s->rangeCounts[s->currentRange];
    }
    if (s->rangeLeft == 0) {
      return false;
    }
    const uint32_t objId = s->rangeStart;
    if (objId >= PDF_MAX_OBJECTS) {
      return false;
    }
    uint32_t ft = 0;
    if (s->w0 >= 1) {
      ft = s->rowBuf[0];
    }
    const uint8_t* f1b = s->rowBuf + s->w0;
    if (ft == 1 && s->w1 > 0) {
      uint32_t fileOff = 0;
      if (s->w1 == 1) {
        fileOff = f1b[0];
      } else if (s->w1 == 2) {
        fileOff = readBe16(f1b);
      } else {
        fileOff = readBe24(f1b);
      }
      if (!s->xref->setOffset(objId, fileOff)) {
        return false;
      }
    } else if (ft == 2 && s->w1 > 0) {
      uint32_t stmObj = 0;
      if (s->w1 == 1) {
        stmObj = f1b[0];
      } else if (s->w1 == 2) {
        stmObj = readBe16(f1b);
      } else {
        stmObj = readBe24(f1b);
      }
      if (stmObj < PDF_MAX_OBJECTS) {
        setBit(s->objStmBits, stmObj);
      }
    } else if (ft == 0) {
      if (!s->xref->setOffset(objId, 0)) {
        return false;
      }
    }
    s->rowPos = 0;
    ++s->rangeStart;
    --s->rangeLeft;
    if (s->currentRange + 1 >= s->rangeCount && s->rangeLeft == 0) {
      break;
    }
  }
  return true;
}

}  // namespace

bool XrefTable::parseXrefStream(FsFile& file, size_t /*fileSize*/, uint32_t xrefObjOffset) {
  PdfFixedString<PDF_OBJECT_BODY_MAX> dict;
  uint32_t so = 0;
  uint32_t sl = 0;
  if (!PdfObject::readAt(file, xrefObjOffset, dict, &so, &sl, nullptr)) {
    LOG_ERR("PDF", "xref: cannot read xref stream object");
    return false;
  }
  PdfFixedString<PDF_DICT_VALUE_MAX> t;
  if (!PdfObject::getDictValue("/Type", dict.view(), t)) {
    LOG_ERR("PDF", "xref: missing /Type");
    return false;
  }
  while (t.size() > 0 && (t[0] == ' ' || t[0] == '\t' || t[0] == '\r' || t[0] == '\n')) {
    t.erase_prefix(1);
  }
  if (t.view() != "/XRef") {
    LOG_ERR("PDF", "xref: expected /Type /XRef");
    return false;
  }

  rootObjId_ = PdfObject::getDictRef("/Root", dict.view());
  const int size = PdfObject::getDictInt("/Size", dict.view(), 0);
  if (size <= 0 || size > static_cast<int>(PDF_MAX_OBJECTS)) {
    LOG_ERR("PDF", "xref: bad /Size in xref stream");
    return false;
  }

  PdfFixedVector<int, 32> wv;
  PdfFixedString<PDF_DICT_VALUE_MAX> wStr;
  if (!PdfObject::getDictValue("/W", dict.view(), wStr) || !parseBracketInts(wStr.view(), wv) || wv.size() < 3 ||
      wv[0] < 0 || wv[1] < 0 || wv[2] < 0) {
    LOG_ERR("PDF", "xref: bad /W in xref stream");
    return false;
  }
  const size_t w0 = static_cast<size_t>(wv[0]);
  const size_t w1 = static_cast<size_t>(wv[1]);
  const size_t w2 = static_cast<size_t>(wv[2]);
  const size_t recBytes = w0 + w1 + w2;
  if (recBytes == 0 || recBytes > 32) {
    LOG_ERR("PDF", "xref: invalid /W entry sizes");
    return false;
  }

  PdfFixedString<PDF_DICT_VALUE_MAX> idxStr;
  PdfFixedVector<int, 32> idxv;
  bool hasIndex = false;
  if (PdfObject::getDictValue("/Index", dict.view(), idxStr) && !idxStr.empty() &&
      parseBracketInts(idxStr.view(), idxv) && idxv.size() >= 2) {
    hasIndex = true;
    if ((idxv.size() & 1) != 0) {
      LOG_ERR("PDF", "xref: bad /Index pair count");
      return false;
    }
  }

  std::memset(offsets_, 0, sizeof(offsets_));
  offsetCount_ = static_cast<uint32_t>(size);

  XrefStreamDecodeState st{};
  st.xref = this;
  st.rangeCount = 0;
  st.currentRange = 0;
  st.rangeStart = 0;
  st.rangeLeft = 0;

  auto clearRangesAndFail = [&]() -> bool {
    LOG_ERR("PDF", "xref: bad /Index");
    return false;
  };

  if (hasIndex) {
    constexpr uint16_t kMaxRanges = 16;
    for (size_t i = 0; i + 1 < idxv.size(); i += 2) {
      const int start = idxv[i];
      const int count = idxv[i + 1];
      if (start < 0 || count <= 0) {
        return clearRangesAndFail();
      }
      const uint32_t uStart = static_cast<uint32_t>(start);
      const uint32_t uCount = static_cast<uint32_t>(count);
      if (uStart >= PDF_MAX_OBJECTS || uCount > PDF_MAX_OBJECTS - uStart) {
        return clearRangesAndFail();
      }
      if (st.rangeCount >= kMaxRanges) {
        return clearRangesAndFail();
      }
      st.rangeStarts[st.rangeCount] = uStart;
      st.rangeCounts[st.rangeCount] = uCount;
      ++st.rangeCount;
    }
  } else {
    st.rangeStarts[0] = 0;
    st.rangeCounts[0] = static_cast<uint32_t>(size);
    st.rangeCount = 1;
  }

  size_t totalRangeCount = 0;
  for (uint16_t i = 0; i < st.rangeCount; ++i) {
    totalRangeCount += st.rangeCounts[i];
    if (totalRangeCount > static_cast<size_t>(size)) {
      return clearRangesAndFail();
    }
  }
  if (st.rangeCount == 0) {
    return clearRangesAndFail();
  }

  st.rangeStart = st.rangeStarts[0];
  st.rangeLeft = st.rangeCounts[0];
  st.w0 = w0;
  st.w1 = w1;
  st.w2 = w2;
  st.recBytes = recBytes;

  if (!StreamDecoder::flateDecodeChunks(file, so, sl, takeXrefStreamChunk, &st)) {
    pdfLogErr("xref: xref stream decode failed");
    return false;
  }

  std::memcpy(objStmContainers_, st.objStmBits, sizeof(objStmContainers_));

  bool any = false;
  for (uint32_t i = 0; i < offsetCount_; ++i) {
    if (getOffset(i) != 0) {
      any = true;
      break;
    }
  }
  bool anyInline = false;
  for (size_t i = 0; i < PDF_MAX_INLINE_OBJECTS; ++i) {
    if (inline_[i].used) {
      anyInline = true;
      break;
    }
  }
  if (!any && !anyInline) {
    pdfLogErr("xref: no objects after xref stream");
    return false;
  }
  if (rootObjId_ == 0) {
    pdfLogErr("xref: missing /Root");
    return false;
  }
  return true;
}

bool XrefTable::loadObjStreamForTarget(FsFile& file, uint32_t stmObjId, uint32_t targetObjId) {
  if (findInline(targetObjId) != nullptr) {
    return true;
  }
  const uint32_t off = getOffset(stmObjId);
  if (off == 0) {
    return false;
  }
  PdfFixedString<PDF_OBJECT_BODY_MAX> dict;
  uint32_t so = 0;
  uint32_t sl = 0;
  if (!PdfObject::readAt(file, off, dict, &so, &sl, this)) {
    return false;
  }
  PdfFixedString<PDF_DICT_VALUE_MAX> st;
  if (!PdfObject::getDictValue("/Type", dict.view(), st)) {
    return false;
  }
  while (st.size() > 0 && (st[0] == ' ' || st[0] == '\t' || st[0] == '\r' || st[0] == '\n')) {
    st.erase_prefix(1);
  }
  if (st.view() != "/ObjStm") {
    return false;
  }
  const int nObj = PdfObject::getDictInt("/N", dict.view(), 0);
  const int first = PdfObject::getDictInt("/First", dict.view(), 0);
  if (nObj <= 0 || nObj > 4096 || first < 0) {
    return false;
  }

#ifndef HAL_STORAGE_STUB
  auto& storage = Storage;
  if (!storage.ensureDirectoryExists("/.crosspoint/pdf")) {
    return false;
  }
  constexpr size_t kTempPrefixLen = sizeof("/.crosspoint/pdf/objstm_") - 1;
  PdfFixedString<96> tempPath;
  if (!tempPath.assign("/.crosspoint/pdf/objstm_", kTempPrefixLen) || !appendUnsigned(tempPath, stmObjId) ||
      !tempPath.append(".tmp", 4)) {
    return false;
  }
  FsFile spill;
  if (!storage.openFileForWrite("PDF", tempPath.c_str(), spill)) {
    return false;
  }
  StreamToFileCtx ctx{&spill};
  const bool decoded = StreamDecoder::flateDecodeChunks(file, so, sl, writeChunkToFile, &ctx);
  spill.flush();
  spill.close();
  if (!decoded) {
    storage.remove(tempPath.c_str());
    return false;
  }
  if (!storage.openFileForRead("PDF", tempPath.c_str(), spill)) {
    storage.remove(tempPath.c_str());
    return false;
  }

  const size_t spillSize = spill.fileSize();
  if (static_cast<size_t>(first) > spillSize) {
    spill.close();
    storage.remove(tempPath.c_str());
    return false;
  }

  uint32_t targetRel = 0;
  if (!scanObjStmHeaderForTarget(spill, static_cast<size_t>(first), nObj, targetObjId, targetRel)) {
    spill.close();
    storage.remove(tempPath.c_str());
    return false;
  }

  uint32_t nextRel = 0;
  if (!scanObjStmHeaderForNext(spill, static_cast<size_t>(first), nObj, targetRel, nextRel)) {
    spill.close();
    storage.remove(tempPath.c_str());
    return false;
  }

  const size_t start = static_cast<size_t>(first) + static_cast<size_t>(targetRel);
  if (start > spillSize) {
    spill.close();
    storage.remove(tempPath.c_str());
    return false;
  }

  size_t end = spillSize;
  if (nextRel > targetRel) {
    const size_t nextStart = static_cast<size_t>(first) + static_cast<size_t>(nextRel);
    if (nextStart > start && nextStart <= spillSize) {
      end = nextStart;
    }
  }

  constexpr size_t kChunk = 256;
  char buf[kChunk];
  if (!spill.seek(start)) {
    spill.close();
    storage.remove(tempPath.c_str());
    return false;
  }
  std::string slice;
  slice.reserve(end - start);
  size_t left = end - start;
  while (left > 0) {
    const size_t want = std::min(left, kChunk);
    const int rd = spill.read(reinterpret_cast<uint8_t*>(buf), want);
    if (rd <= 0) {
      slice.clear();
      break;
    }
    slice.append(buf, static_cast<size_t>(rd));
    left -= static_cast<size_t>(rd);
  }
  spill.close();
  storage.remove(tempPath.c_str());
  if (slice.empty()) {
    return false;
  }
  while (!slice.empty() && (slice.front() == ' ' || slice.front() == '\t' || slice.front() == '\r' ||
                            slice.front() == '\n')) {
    slice.erase(slice.begin());
  }
  while (!slice.empty() && (slice.back() == ' ' || slice.back() == '\t' || slice.back() == '\r' ||
                            slice.back() == '\n')) {
    slice.pop_back();
  }
  if (slice.empty()) {
    return false;
  }
  PdfFixedString<PDF_INLINE_DICT_MAX> d;
  PdfByteBuffer stm;
  if (!splitObjStmObjectSlice(slice, d, stm)) {
    return false;
  }
  return insertInlineObject(targetObjId, d, stm.ptr(), stm.len);
#else
  ObjStmDecodeAccum stream{};
  if (!StreamDecoder::flateDecodeChunks(file, so, sl, appendObjStmBytes, &stream)) {
    return false;
  }
  if (stream.bytes.empty() || stream.bytes.size() < static_cast<size_t>(first)) {
    return false;
  }

  uint32_t targetRel = 0;
  uint32_t nextRel = 0;
  if (!scanObjStmHeaderFromBytes(stream.bytes.data(), stream.bytes.size(), static_cast<size_t>(first), nObj, targetObjId,
                                targetRel, nextRel)) {
    return false;
  }
  const size_t base = static_cast<size_t>(first);
  const size_t start = base + static_cast<size_t>(targetRel);
  const size_t end = nextRel == UINT32_MAX ? SIZE_MAX : base + static_cast<size_t>(nextRel);
  if (end != SIZE_MAX && end < start) {
    return false;
  }
  if (nextRel != UINT32_MAX && end > stream.bytes.size()) {
    return false;
  }

  const size_t clipEnd = (end == SIZE_MAX) ? stream.bytes.size() : std::min(end, stream.bytes.size());
  if (start >= clipEnd) {
    return false;
  }
  std::string_view slice(reinterpret_cast<const char*>(stream.bytes.data() + start), clipEnd - start);
  std::string_view trimmed = slice;
  while (!trimmed.empty() &&
         (trimmed.front() == ' ' || trimmed.front() == '\t' || trimmed.front() == '\r' || trimmed.front() == '\n')) {
    trimmed.remove_prefix(1);
  }
  while (!trimmed.empty() &&
         (trimmed.back() == ' ' || trimmed.back() == '\t' || trimmed.back() == '\r' || trimmed.back() == '\n')) {
    trimmed.remove_suffix(1);
  }
  if (trimmed.empty()) {
    return false;
  }
  PdfFixedString<PDF_INLINE_DICT_MAX> d;
  PdfByteBuffer stm;
  if (!splitObjStmObjectSlice(trimmed, d, stm)) {
    return false;
  }
  return insertInlineObject(targetObjId, d, stm.ptr(), stm.len);
#endif
}

bool XrefTable::setOffset(uint32_t objId, uint32_t off) {
  if (objId >= PDF_MAX_OBJECTS) {
    pdfLogErr("xref: object id overflow");
    return false;
  }
  const size_t base = static_cast<size_t>(objId) * kPackedOffsetBytes;
  storePackedOffset(offsets_ + base, off);
  if (objId + 1 > offsetCount_) {
    offsetCount_ = objId + 1;
  }
  return true;
}

void XrefTable::ensureOffsetCount(uint32_t n) {
  if (n > PDF_MAX_OBJECTS) return;
  if (n > offsetCount_) offsetCount_ = n;
}

bool XrefTable::insertInlineObject(uint32_t objNum, const PdfFixedString<PDF_INLINE_DICT_MAX>& d, const uint8_t* stm,
                                   size_t stmLen) {
  for (size_t i = 0; i < PDF_MAX_INLINE_OBJECTS; ++i) {
    if (inline_[i].used && inline_[i].objId == objNum) {
      inline_[i] = InlineEntry{};
      inline_[i].used = true;
      inline_[i].objId = objNum;
      inline_[i].dictLen = static_cast<uint16_t>(d.size());
      std::memcpy(inline_[i].dict, d.data(), d.size());
      if (stmLen <= PDF_INLINE_STREAM_MAX) {
        inline_[i].streamLen = static_cast<uint16_t>(stmLen);
        if (stmLen > 0 && stm) std::memcpy(inline_[i].stream, stm, stmLen);
      }
      return true;
    }
  }
  for (size_t i = 0; i < PDF_MAX_INLINE_OBJECTS; ++i) {
    if (!inline_[i].used) {
      if (d.size() > PDF_INLINE_DICT_MAX) return false;
      inline_[i].used = true;
      inline_[i].objId = objNum;
      inline_[i].dictLen = static_cast<uint16_t>(d.size());
      std::memcpy(inline_[i].dict, d.data(), d.size());
      if (stmLen > PDF_INLINE_STREAM_MAX) {
        inline_[i].streamLen = 0;
      } else {
        inline_[i].streamLen = static_cast<uint16_t>(stmLen);
        if (stmLen > 0 && stm) std::memcpy(inline_[i].stream, stm, stmLen);
      }
      return true;
    }
  }

  if (d.size() > PDF_INLINE_DICT_MAX) return false;
  const size_t slot = inlineVictim_ % PDF_MAX_INLINE_OBJECTS;
  inlineVictim_ = static_cast<uint16_t>((inlineVictim_ + 1) % PDF_MAX_INLINE_OBJECTS);
  inline_[slot] = InlineEntry{};
  inline_[slot].used = true;
  inline_[slot].objId = objNum;
  inline_[slot].dictLen = static_cast<uint16_t>(d.size());
  std::memcpy(inline_[slot].dict, d.data(), d.size());
  if (stmLen <= PDF_INLINE_STREAM_MAX) {
    inline_[slot].streamLen = static_cast<uint16_t>(stmLen);
    if (stmLen > 0 && stm) std::memcpy(inline_[slot].stream, stm, stmLen);
  }
  return true;
}

const XrefTable::InlineEntry* XrefTable::findInline(uint32_t objId) const {
  for (size_t i = 0; i < PDF_MAX_INLINE_OBJECTS; ++i) {
    if (inline_[i].used && inline_[i].objId == objId) {
      return &inline_[i];
    }
  }
  return nullptr;
}

bool XrefTable::parse(FsFile& file) {
  std::memset(offsets_, 0, sizeof(offsets_));
  offsetCount_ = 0;
  rootObjId_ = 0;
  inlineVictim_ = 0;
  for (auto& e : inline_) {
    e = InlineEntry{};
  }
  std::memset(objStmContainers_, 0, sizeof(objStmContainers_));

  const size_t fileSize = file.fileSize();
  if (fileSize < 32) {
    pdfLogErr("xref: file too small");
    return false;
  }

  const size_t tailSize = std::min<size_t>(1024, fileSize);
  const size_t tailStart = fileSize - tailSize;
  uint8_t tailBuf[1024];
  if (!file.seek(tailStart) || file.read(tailBuf, tailSize) != static_cast<int>(tailSize)) {
    pdfLogErr("xref: read tail failed");
    return false;
  }

  const size_t sx = findStartXref(tailBuf, tailSize);
  if (sx == SIZE_MAX) {
    pdfLogErr("xref: startxref not found");
    return false;
  }

  const char* p = reinterpret_cast<const char*>(tailBuf + sx + 9);
  skipWs(p);
  char numBuf[32];
  size_t nb = 0;
  while (*p >= '0' && *p <= '9' && nb + 1 < sizeof(numBuf)) {
    numBuf[nb++] = *p++;
  }
  numBuf[nb] = '\0';
  if (nb == 0) {
    pdfLogErr("xref: bad startxref offset");
    return false;
  }
  const uint32_t xrefOffset = static_cast<uint32_t>(strtoul(numBuf, nullptr, 10));

  if (xrefOffset >= fileSize) {
    pdfLogErr("xref: startxref out of range");
    return false;
  }

  if (!file.seek(xrefOffset)) {
    pdfLogErr("xref: seek failed");
    return false;
  }

  char lineBuf[128];
  size_t lineLen = 0;
  readLineFromFile(file, lineBuf, sizeof(lineBuf), lineLen);
  if (std::strncmp(lineBuf, "xref", 4) != 0) {
    return parseXrefStream(file, fileSize, xrefOffset);
  }

  rootObjId_ = 0;
  if (!mergeClassicXrefChain(file, fileSize, xrefOffset, *this, rootObjId_)) {
    pdfLogErr("xref: merge failed");
    return false;
  }

  bool any = false;
  for (uint32_t i = 0; i < offsetCount_; ++i) {
    if (getOffset(i) != 0) {
      any = true;
      break;
    }
  }
  if (!any) {
    pdfLogErr("xref: no objects");
    return false;
  }
  if (rootObjId_ == 0) {
    pdfLogErr("xref: missing /Root");
    return false;
  }
  return true;
}

bool XrefTable::readDictForObject(FsFile& file, uint32_t objId, PdfFixedString<PDF_OBJECT_BODY_MAX>& dictBody) const {
  dictBody.clear();
  const InlineEntry* inl = findInline(objId);
  if (inl) {
    return dictBody.assign(inl->dict, inl->dictLen);
  }
  const uint32_t off = getOffset(objId);
  if (off == 0) {
    for (uint32_t sid = 0; sid < PDF_MAX_OBJECTS; ++sid) {
      if ((objStmContainers_[sid >> 3U] & static_cast<uint8_t>(1U << (sid & 7U))) == 0) {
        continue;
      }
      if (const_cast<XrefTable*>(this)->loadObjStreamForTarget(file, sid, objId)) {
        const InlineEntry* loaded = findInline(objId);
        if (loaded) {
          return dictBody.assign(loaded->dict, loaded->dictLen);
        }
      }
    }
    return false;
  }
  return PdfObject::readAt(file, off, dictBody, nullptr, nullptr, this);
}

bool XrefTable::readStreamMetaForObject(FsFile& file, uint32_t objId, PdfFixedString<PDF_OBJECT_BODY_MAX>& dictOut,
                                        uint32_t& streamOffset, uint32_t& streamLength, bool& flateDecode) const {
  dictOut.clear();
  streamOffset = 0;
  streamLength = 0;
  flateDecode = false;

  const uint32_t off = getOffset(objId);
  if (off == 0) {
    return false;
  }
  if (!PdfObject::readAt(file, off, dictOut, &streamOffset, &streamLength, this)) {
    return false;
  }
  const std::string_view dv(dictOut.data(), dictOut.size());
  flateDecode = dv.find("/FlateDecode") != std::string_view::npos || dv.find("/Fl ") != std::string_view::npos;
  return streamOffset != 0 && streamLength > 0;
}

bool XrefTable::readStreamForObject(FsFile& file, uint32_t objId, PdfFixedString<PDF_OBJECT_BODY_MAX>& dictOut,
                                    PdfByteBuffer& streamPayload, bool& flateDecode) const {
  dictOut.clear();
  streamPayload.clear();
  flateDecode = false;

  const InlineEntry* inl = findInline(objId);
  if (inl) {
    if (!dictOut.assign(inl->dict, inl->dictLen)) {
      return false;
    }
    if (inl->streamLen == 0) {
      return false;
    }
    if (!streamPayload.resize(inl->streamLen)) {
      return false;
    }
    std::memcpy(streamPayload.ptr(), inl->stream, inl->streamLen);
    const std::string_view dv(dictOut.data(), dictOut.size());
    flateDecode = dv.find("/FlateDecode") != std::string_view::npos || dv.find("/Fl ") != std::string_view::npos;
    return !dictOut.empty();
  }

  const uint32_t off = getOffset(objId);
  if (off == 0) {
    return false;
  }
  uint32_t so = 0;
  uint32_t sl = 0;
  if (!PdfObject::readAt(file, off, dictOut, &so, &sl, this)) {
    return false;
  }
  const std::string_view dv(dictOut.data(), dictOut.size());
  flateDecode = dv.find("/FlateDecode") != std::string_view::npos || dv.find("/Fl ") != std::string_view::npos;
  if (sl == 0) {
    return false;
  }
  if (!file.seek(so)) {
    return false;
  }
  if (!streamPayload.resize(sl)) {
    return false;
  }
  if (file.read(streamPayload.ptr(), sl) != static_cast<int>(sl)) {
    streamPayload.clear();
    return false;
  }
  return true;
}

uint32_t XrefTable::getOffset(uint32_t objId) const {
  if (objId >= offsetCount_) return 0;
  const size_t base = static_cast<size_t>(objId) * kPackedOffsetBytes;
  return loadPackedOffset(offsets_ + base);
}

uint32_t XrefTable::objectCount() const { return offsetCount_; }

uint32_t XrefTable::rootObjId() const { return rootObjId_; }
