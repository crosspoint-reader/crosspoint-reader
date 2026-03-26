#include "XrefTable.h"

#include "PdfObject.h"
#include "StreamDecoder.h"

#include <Logging.h>

#include <cctype>
#include <cstring>
#include <string_view>
#include <utility>

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

// Depth-indexed section updates (no heap; avoids large stack frames in merge).
struct SecPair {
  uint32_t id = 0;
  uint32_t off = 0;
};
static SecPair g_secUpdates[PDF_MAX_XREF_CHAIN_DEPTH][PDF_MAX_XREF_UPDATES_PER_SECTION];
static unsigned g_secCount[PDF_MAX_XREF_CHAIN_DEPTH];

static uint8_t g_pdfLargeWork[PDF_LARGE_WORK_BYTES];

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

bool parseClassicXrefSection(FsFile& file, size_t fileSize, uint32_t xrefOff, unsigned depth, uint32_t& rootOut,
                             uint32_t& prevXref, uint32_t& declaredSize) {
  rootOut = 0;
  prevXref = 0;
  declaredSize = 0;
  if (depth >= PDF_MAX_XREF_CHAIN_DEPTH) {
    LOG_ERR("PDF", "xref: depth overflow");
    return false;
  }
  g_secCount[depth] = 0;

  if (xrefOff >= fileSize || !file.seek(xrefOff)) {
    return false;
  }

  char lineBuf[128];
  size_t lineLen = 0;
  readLineFromFile(file, lineBuf, sizeof(lineBuf), lineLen);
  if (std::strncmp(lineBuf, "xref", 4) != 0) {
    return false;
  }

  PdfFixedString<16384> trailer;

  for (;;) {
    readLineFromFile(file, lineBuf, sizeof(lineBuf), lineLen);
    if (lineLen == 0) {
      if (file.position() >= fileSize) {
        LOG_ERR("PDF", "xref: unexpected EOF before trailer");
        return false;
      }
      continue;
    }
    if (lineLen >= 7 && std::strncmp(lineBuf, "trailer", 7) == 0 &&
        (lineBuf[7] == '\0' || lineBuf[7] == ' ' || lineBuf[7] == '\t')) {
      const char* rest = lineBuf + 7;
      skipWs(rest);
      trailer.clear();
      if (!trailer.append(rest, std::strlen(rest))) {
        return false;
      }
      break;
    }
    char* endp = nullptr;
    const unsigned long firstObj = strtoul(lineBuf, &endp, 10);
    skipWs(endp);
    const unsigned long count = strtoul(endp, &endp, 10);
    if (count > 100000 || firstObj + count > 2000000) {
      LOG_ERR("PDF", "xref: bad subsection");
      return false;
    }
    if (count == 0) {
      continue;
    }

    char row[20];
    for (unsigned long i = 0; i < count; ++i) {
      const int rd = file.read(reinterpret_cast<uint8_t*>(row), 20);
      if (rd != 20) {
        LOG_ERR("PDF", "xref: short read entries");
        return false;
      }
      if (row[10] != ' ' || row[16] != ' ') {
        LOG_ERR("PDF", "xref: bad entry row");
        return false;
      }
      char offStr[11];
      std::memcpy(offStr, row, 10);
      offStr[10] = '\0';
      const char flag = row[17];
      const uint32_t idx = static_cast<uint32_t>(firstObj + i);
      const uint32_t offVal = (flag == 'n') ? static_cast<uint32_t>(strtoul(offStr, nullptr, 10)) : 0;
      if (g_secCount[depth] >= PDF_MAX_XREF_UPDATES_PER_SECTION) {
        LOG_ERR("PDF", "xref: too many xref updates in section");
        return false;
      }
      g_secUpdates[depth][g_secCount[depth]++] = {idx, offVal};
    }
  }

  {
    constexpr size_t kChunk = 256;
    char chBuf[kChunk];
    for (int safety = 0; safety < 200 && trailer.size() < 16384 - kChunk; ++safety) {
      if (trailer.view().find("startxref") != std::string_view::npos) {
        break;
      }
      const int rd = file.read(reinterpret_cast<uint8_t*>(chBuf), kChunk);
      if (rd <= 0) break;
      if (!trailer.append(chBuf, static_cast<size_t>(rd))) break;
      const size_t sx = trailer.view().find("startxref");
      if (sx != std::string_view::npos) {
        trailer.resize(sx);
        break;
      }
    }
  }

  {
    const char* s = trailer.c_str();
    const char* rootKey = strstr(s, "/Root");
    if (rootKey) {
      rootKey += 5;
      skipWs(rootKey);
      char rb[32];
      size_t ri = 0;
      while (*rootKey >= '0' && *rootKey <= '9' && ri + 1 < sizeof(rb)) {
        rb[ri++] = *rootKey++;
      }
      rb[ri] = '\0';
      if (ri > 0) {
        rootOut = static_cast<uint32_t>(strtoul(rb, nullptr, 10));
      }
    }
    const char* prevKey = strstr(s, "/Prev");
    if (prevKey) {
      prevKey += 5;
      skipWs(prevKey);
      char pb[32];
      size_t pi = 0;
      while (*prevKey >= '0' && *prevKey <= '9' && pi + 1 < sizeof(pb)) {
        pb[pi++] = *prevKey++;
      }
      pb[pi] = '\0';
      if (pi > 0) {
        prevXref = static_cast<uint32_t>(strtoul(pb, nullptr, 10));
      }
    }
    const char* sizeKey = strstr(s, "/Size");
    if (sizeKey) {
      sizeKey += 5;
      skipWs(sizeKey);
      char sb[32];
      size_t si = 0;
      while (*sizeKey >= '0' && *sizeKey <= '9' && si + 1 < sizeof(sb)) {
        sb[si++] = *sizeKey++;
      }
      sb[si] = '\0';
      if (si > 0) {
        const uint32_t sz = static_cast<uint32_t>(strtoul(sb, nullptr, 10));
        if (sz > 0 && sz < 2000000) {
          declaredSize = sz;
        }
      }
    }
  }

  return true;
}

bool mergeXrefChainRecursive(FsFile& file, size_t fileSize, uint32_t xrefOff, XrefTable& xref, uint32_t& rootOut,
                             unsigned chainDepth = 0) {
  if (chainDepth > PDF_MAX_XREF_CHAIN_DEPTH) {
    LOG_ERR("PDF", "xref: /Prev chain too deep");
    return false;
  }
  uint32_t secRoot = 0;
  uint32_t prevXref = 0;
  uint32_t declaredSize = 0;
  if (!parseClassicXrefSection(file, fileSize, xrefOff, chainDepth, secRoot, prevXref, declaredSize)) {
    return false;
  }

  if (prevXref != 0 && prevXref < fileSize) {
    if (!mergeXrefChainRecursive(file, fileSize, prevXref, xref, rootOut, chainDepth + 1)) {
      return false;
    }
  }

  for (unsigned i = 0; i < g_secCount[chainDepth]; ++i) {
    const SecPair& u = g_secUpdates[chainDepth][i];
    if (!xref.setOffset(u.id, u.off)) {
      return false;
    }
  }
  if (declaredSize > 0 && declaredSize <= PDF_MAX_OBJECTS) {
    xref.ensureOffsetCount(declaredSize);
  }
  if (secRoot != 0) {
    rootOut = secRoot;
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

  int indexStart = 0;
  int indexCount = size;
  PdfFixedString<PDF_DICT_VALUE_MAX> idxStr;
  PdfFixedVector<int, 32> idxv;
  if (PdfObject::getDictValue("/Index", dict.view(), idxStr) && !idxStr.empty() && parseBracketInts(idxStr.view(), idxv) &&
      idxv.size() >= 2) {
    indexStart = idxv[0];
    indexCount = idxv[1];
  }
  if (indexCount <= 0 || indexStart < 0 ||
      static_cast<size_t>(indexStart) + static_cast<size_t>(indexCount) > PDF_MAX_OBJECTS) {
    LOG_ERR("PDF", "xref: bad /Index");
    return false;
  }

  const size_t got = StreamDecoder::flateDecode(file, so, sl, g_pdfLargeWork, PDF_LARGE_WORK_BYTES);
  if (got == 0) {
    LOG_ERR("PDF", "xref: xref stream decode failed");
    return false;
  }

  std::memset(offsets_, 0, sizeof(offsets_));
  offsetCount_ = static_cast<uint32_t>(size);

  uint8_t objStmContainers[PDF_MAX_OBJECTS];
  std::memset(objStmContainers, 0, sizeof(objStmContainers));

  const size_t nRows = static_cast<size_t>(indexCount);
  if (got < nRows * recBytes) {
    LOG_ERR("PDF", "xref: xref stream too short");
    return false;
  }

  for (size_t i = 0; i < nRows; ++i) {
    const uint8_t* row = g_pdfLargeWork + i * recBytes;
    const uint32_t objId = static_cast<uint32_t>(indexStart + static_cast<int>(i));
    if (objId >= PDF_MAX_OBJECTS) {
      LOG_ERR("PDF", "xref: object id out of range");
      return false;
    }
    uint32_t ft = 0;
    if (w0 >= 1) {
      ft = row[0];
    }
    const uint8_t* f1b = row + w0;
    if (ft == 1 && w1 > 0) {
      uint32_t fileOff = 0;
      if (w1 == 1) {
        fileOff = f1b[0];
      } else if (w1 == 2) {
        fileOff = readBe16(f1b);
      } else {
        fileOff = readBe24(f1b);
      }
      offsets_[objId] = fileOff;
    } else if (ft == 2 && w1 > 0) {
      uint32_t stmObj = 0;
      if (w1 == 1) {
        stmObj = f1b[0];
      } else if (w1 == 2) {
        stmObj = readBe16(f1b);
      } else {
        stmObj = readBe24(f1b);
      }
      if (stmObj < PDF_MAX_OBJECTS) {
        objStmContainers[stmObj] = 1;
      }
    } else if (ft == 0) {
      offsets_[objId] = 0;
    }
  }

  for (uint32_t sid = 0; sid < PDF_MAX_OBJECTS; ++sid) {
    if (objStmContainers[sid]) {
      loadObjStream(file, sid);
    }
  }

  bool any = false;
  for (uint32_t i = 0; i < offsetCount_; ++i) {
    if (offsets_[i] != 0) {
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
    LOG_ERR("PDF", "xref: no objects after xref stream");
    return false;
  }
  if (rootObjId_ == 0) {
    LOG_ERR("PDF", "xref: missing /Root");
    return false;
  }
  return true;
}

void XrefTable::loadObjStream(FsFile& file, uint32_t stmObjId) {
  if (stmObjId < PDF_MAX_OBJECTS && loadedObjStm_[stmObjId]) {
    return;
  }
  const uint32_t off = stmObjId < offsetCount_ ? offsets_[stmObjId] : 0;
  if (off == 0) {
    return;
  }
  PdfFixedString<PDF_OBJECT_BODY_MAX> dict;
  uint32_t so = 0;
  uint32_t sl = 0;
  if (!PdfObject::readAt(file, off, dict, &so, &sl, this)) {
    return;
  }
  PdfFixedString<PDF_DICT_VALUE_MAX> st;
  if (!PdfObject::getDictValue("/Type", dict.view(), st)) {
    return;
  }
  while (st.size() > 0 && (st[0] == ' ' || st[0] == '\t' || st[0] == '\r' || st[0] == '\n')) {
    st.erase_prefix(1);
  }
  if (st.view() != "/ObjStm") {
    return;
  }
  const int nObj = PdfObject::getDictInt("/N", dict.view(), 0);
  const int first = PdfObject::getDictInt("/First", dict.view(), 0);
  if (nObj <= 0 || nObj > 4096 || first < 0) {
    return;
  }

  const size_t rawLen = StreamDecoder::flateDecode(file, so, sl, g_pdfLargeWork, PDF_LARGE_WORK_BYTES);
  if (rawLen == 0) {
    return;
  }

  if (static_cast<size_t>(first) > rawLen) {
    return;
  }
  std::string_view raw(reinterpret_cast<const char*>(g_pdfLargeWork), rawLen);
  const std::string_view header = raw.substr(0, static_cast<size_t>(first));

  struct Pair {
    uint32_t oid = 0;
    uint32_t rel = 0;
  };
  Pair pairs[512];
  size_t pairCount = 0;
  {
    const char* p = header.data();
    const char* hend = header.data() + header.size();
    while (p < hend && pairCount < 512) {
      while (p < hend && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
      if (p >= hend) break;
      char* e = nullptr;
      const unsigned long oid = std::strtoul(p, &e, 10);
      if (e == p) break;
      p = e;
      while (p < hend && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
      const unsigned long rel = std::strtoul(p, &e, 10);
      if (e == p) break;
      p = e;
      pairs[pairCount].oid = static_cast<uint32_t>(oid);
      pairs[pairCount].rel = static_cast<uint32_t>(rel);
      ++pairCount;
    }
  }
  if (pairCount < static_cast<size_t>(nObj)) {
    return;
  }

  if (stmObjId < PDF_MAX_OBJECTS) {
    loadedObjStm_[stmObjId] = 1;
  }

  const size_t base = static_cast<size_t>(first);
  for (int i = 0; i < nObj && i < static_cast<int>(pairCount); ++i) {
    const uint32_t objNum = pairs[static_cast<size_t>(i)].oid;
    const uint32_t rel = pairs[static_cast<size_t>(i)].rel;
    const size_t start = base + static_cast<size_t>(rel);
    if (start > raw.size()) {
      continue;
    }
    size_t end = raw.size();
    if (i + 1 < static_cast<int>(pairCount)) {
      const uint32_t relNext = pairs[static_cast<size_t>(i + 1)].rel;
      const size_t nextStart = base + static_cast<size_t>(relNext);
      if (nextStart > start && nextStart <= raw.size()) {
        end = nextStart;
      }
    }
    std::string_view slice = raw.substr(start, end - start);
    while (slice.size() > 0 && (slice.front() == ' ' || slice.front() == '\t' || slice.front() == '\r' ||
                                slice.front() == '\n')) {
      slice.remove_prefix(1);
    }
    while (slice.size() > 0 && (slice.back() == ' ' || slice.back() == '\t' || slice.back() == '\r' ||
                                slice.back() == '\n')) {
      slice.remove_suffix(1);
    }
    if (slice.empty()) {
      continue;
    }
    PdfFixedString<PDF_INLINE_DICT_MAX> d;
    PdfByteBuffer stm;
    if (!splitObjStmObjectSlice(slice, d, stm)) {
      continue;
    }
    insertInlineObject(objNum, d, stm.ptr(), stm.len);
  }
}

bool XrefTable::setOffset(uint32_t objId, uint32_t off) {
  if (objId >= PDF_MAX_OBJECTS) {
    LOG_ERR("PDF", "xref: object id overflow");
    return false;
  }
  offsets_[objId] = off;
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
  LOG_ERR("PDF", "xref: inline object pool full");
  return false;
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
  for (auto& e : inline_) {
    e = InlineEntry{};
  }
  std::memset(loadedObjStm_, 0, sizeof(loadedObjStm_));

  const size_t fileSize = file.fileSize();
  if (fileSize < 32) {
    LOG_ERR("PDF", "xref: file too small");
    return false;
  }

  const size_t tailSize = std::min<size_t>(1024, fileSize);
  const size_t tailStart = fileSize - tailSize;
  uint8_t tailBuf[1024];
  if (!file.seek(tailStart) || file.read(tailBuf, tailSize) != static_cast<int>(tailSize)) {
    LOG_ERR("PDF", "xref: read tail failed");
    return false;
  }

  const size_t sx = findStartXref(tailBuf, tailSize);
  if (sx == SIZE_MAX) {
    LOG_ERR("PDF", "xref: startxref not found");
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
    LOG_ERR("PDF", "xref: bad startxref offset");
    return false;
  }
  const uint32_t xrefOffset = static_cast<uint32_t>(strtoul(numBuf, nullptr, 10));

  if (xrefOffset >= fileSize) {
    LOG_ERR("PDF", "xref: startxref out of range");
    return false;
  }

  if (!file.seek(xrefOffset)) {
    LOG_ERR("PDF", "xref: seek failed");
    return false;
  }

  char lineBuf[128];
  size_t lineLen = 0;
  readLineFromFile(file, lineBuf, sizeof(lineBuf), lineLen);
  if (std::strncmp(lineBuf, "xref", 4) != 0) {
    return parseXrefStream(file, fileSize, xrefOffset);
  }

  rootObjId_ = 0;
  if (!mergeXrefChainRecursive(file, fileSize, xrefOffset, *this, rootObjId_)) {
    LOG_ERR("PDF", "xref: merge failed");
    return false;
  }

  bool any = false;
  for (uint32_t i = 0; i < offsetCount_; ++i) {
    if (offsets_[i] != 0) {
      any = true;
      break;
    }
  }
  if (!any) {
    LOG_ERR("PDF", "xref: no objects");
    return false;
  }
  if (rootObjId_ == 0) {
    LOG_ERR("PDF", "xref: missing /Root");
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
    return false;
  }
  return PdfObject::readAt(file, off, dictBody, nullptr, nullptr, this);
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
  return offsets_[objId];
}

uint32_t XrefTable::objectCount() const { return offsetCount_; }

uint32_t XrefTable::rootObjId() const { return rootObjId_; }
