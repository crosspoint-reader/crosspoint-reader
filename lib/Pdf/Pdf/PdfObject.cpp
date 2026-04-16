#include "PdfObject.h"

#include <cctype>
#include <cstdlib>
#include <cstring>

#include "PdfLog.h"
#include "XrefTable.h"

namespace {

constexpr size_t kPdfObjectReadChunkBytes = 64;

size_t findStreamKeywordSv(std::string_view s, size_t start) {
  static const char pat[] = "stream";
  constexpr size_t plen = 6;
  if (start >= s.size()) {
    return std::string_view::npos;
  }
  size_t pos = start;
  while (pos + plen <= s.size()) {
    if (std::memcmp(s.data() + pos, pat, plen) == 0) {
      const char prev = pos == 0 ? '\0' : s[pos - 1];
      const bool prevOk = (prev == ' ' || prev == '\t' || prev == '\r' || prev == '\n' || prev == '\0' || prev == '/' ||
                           prev == '<' || prev == '>' || prev == '[' || prev == ']' || prev == '(' || prev == ')');
      const size_t afterPos = pos + plen;
      const bool nextOk = (afterPos >= s.size()) || (s[afterPos] == ' ' || s[afterPos] == '\t' || s[afterPos] == '\r' ||
                                                     s[afterPos] == '\n' || s[afterPos] == '\0');
      if (prevOk && nextOk) {
        return pos;
      }
    }
    ++pos;
  }
  return std::string_view::npos;
}

size_t findEndobjKeywordSv(std::string_view s, size_t start) {
  size_t p = 0;
  if (start >= s.size()) {
    return std::string_view::npos;
  }
  p = start;
  while (p < s.size()) {
    const size_t f = s.find("endobj", p);
    if (f == std::string_view::npos) return std::string_view::npos;
    if (f > 0 && (std::isalnum(static_cast<unsigned char>(s[f - 1])) || s[f - 1] == '/')) {
      p = f + 6;
      continue;
    }
    const size_t after = f + 6;
    if (after < s.size()) {
      const char c = s[after];
      if (c != '\r' && c != '\n' && c != ' ' && c != '\t' && c != '<' && c != '\0') {
        p = f + 6;
        continue;
      }
    }
    return f;
  }
  return std::string_view::npos;
}

uint32_t recoverStreamLengthFromNearbyEndstream(FsFile& file, uint32_t streamOffset, uint32_t declaredLen) {
  constexpr size_t kSlack = 32;
  constexpr size_t kMarkerLen = 9;  // "endstream"
  constexpr size_t kScanChunk = 128;
  constexpr size_t kCarry = kMarkerLen + 2;
  if (declaredLen == 0) {
    return 0;
  }
  const uint64_t start = (declaredLen > kSlack) ? (declaredLen - kSlack) : 0;
  const uint32_t startAbs = streamOffset + static_cast<uint32_t>(start);
  const uint64_t windowLen = kSlack + declaredLen - start + kSlack + kMarkerLen + 2;
  if (windowLen == 0 || windowLen > 2048) {
    return declaredLen;
  }
  if (!file.seek(startAbs)) {
    return declaredLen;
  }

  static const char marker[kMarkerLen + 1] = "endstream";
  char window[kScanChunk + kCarry];
  size_t carryLen = 0;
  size_t consumed = 0;

  uint32_t bestLen = declaredLen;
  uint32_t bestDistance = UINT32_MAX;
  while (consumed < windowLen) {
    const size_t want = std::min(kScanChunk, static_cast<size_t>(windowLen - consumed));
    const int n = file.read(reinterpret_cast<uint8_t*>(window + carryLen), want);
    if (n <= 0) {
      break;
    }
    const size_t available = carryLen + static_cast<size_t>(n);

    for (size_t i = 0; i + kMarkerLen <= available; ++i) {
      bool isMarker = true;
      for (size_t j = 0; j < kMarkerLen; ++j) {
        if (window[i + j] != marker[j]) {
          isMarker = false;
          break;
        }
      }
      if (!isMarker) {
        continue;
      }
      const size_t markerAbs = start + consumed - carryLen + i;
      const size_t next = i + kMarkerLen;
      if (markerAbs < (declaredLen > kSlack ? declaredLen - kSlack : 0) ||
          markerAbs > static_cast<size_t>(declaredLen + kSlack)) {
        continue;
      }
      if (i > 0 && (window[i - 1] != ' ' && window[i - 1] != '\r' && window[i - 1] != '\n' && window[i - 1] != '\t')) {
        continue;
      }
      if (next >= available ||
          (window[next] != ' ' && window[next] != '\r' && window[next] != '\n' && window[next] != '\t')) {
        continue;
      }
      const uint32_t candidate = static_cast<uint32_t>(markerAbs);
      const uint32_t distance = candidate > declaredLen ? candidate - declaredLen : declaredLen - candidate;
      if (distance < bestDistance) {
        bestDistance = distance;
        bestLen = candidate;
      }
    }

    carryLen = std::min(available, kCarry);
    std::memmove(window, window + available - carryLen, carryLen);
    consumed += static_cast<size_t>(n);
  }
  return bestLen;
}

bool locateObjKeywordFs(PdfFixedString<PDF_OBJECT_BODY_MAX>& acc, size_t& outEraseTo, size_t startFrom) {
  if (startFrom >= acc.size()) return false;
  if (startFrom + 3 > acc.size()) {
    return false;
  }
  for (size_t i = startFrom; i + 3 <= acc.size(); ++i) {
    if (std::memcmp(acc.data() + i, "obj", 3) != 0) continue;
    if (i >= 3 && acc[i - 1] == 'b' && acc[i - 2] == 'd' && acc[i - 3] == 'n') continue;
    if (i > 0) {
      const char prev = acc[i - 1];
      if (std::isalnum(static_cast<unsigned char>(prev)) || prev == '/') continue;
    }
    const size_t after = i + 3;
    if (after >= acc.size()) continue;
    {
      const char c = acc[after];
      if (c != ' ' && c != '\t' && c != '\r' && c != '\n' && c != '<') continue;
    }
    outEraseTo = after;
    return true;
  }
  return false;
}

uint32_t resolveIndirectStreamLength(FsFile& file, const XrefTable* xref, uint32_t lenObjId, uint32_t gen) {
  if (!xref || lenObjId == 0) {
    return 0;
  }
  const uint32_t lenObjOff = xref->getOffset(lenObjId);
  if (lenObjOff == 0) {
    return 0;
  }
  PdfFixedString<PDF_OBJECT_BODY_MAX> lenBody;
  if (!PdfObject::readAt(file, lenObjOff, lenBody, nullptr, nullptr, nullptr)) {
    return 0;
  }
  const char* p = lenBody.c_str();
  char* end = nullptr;
  const auto isWs = [](const char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };

  while (isWs(*p)) ++p;
  const unsigned long first = std::strtoul(p, &end, 10);
  if (end == p) return 0;

  const char* q = end;
  while (isWs(*q)) ++q;
  if (*q == '\0') {
    return static_cast<uint32_t>(first);
  }
  if (std::isdigit(static_cast<unsigned char>(*q))) {
    char* e2 = nullptr;
    const unsigned long second = std::strtoul(q, &e2, 10);
    if (e2 == q) return 0;
    const char* afterSecond = e2;
    while (isWs(*afterSecond)) ++afterSecond;
    if (afterSecond[0] != 'o' || afterSecond[1] != 'b' || afterSecond[2] != 'j') {
      return 0;
    }
    if (first != static_cast<unsigned long>(lenObjId) || second != static_cast<unsigned long>(gen)) {
      return 0;
    }
    const char* value = afterSecond + 3;
    while (isWs(*value)) ++value;
    if (*value == '\0') {
      return 0;
    }
    const unsigned long len = std::strtoul(value, &end, 10);
    return end == value ? 0 : static_cast<uint32_t>(len);
  }
  if (q[0] != 'o' || q[1] != 'b' || q[2] != 'j') {
    // Fallback: treat as raw numeric payload.
    return static_cast<uint32_t>(first);
  }
  return 0;
}

uint32_t resolveStreamLength(FsFile& file, const XrefTable* xref, std::string_view dictPart) {
  PdfFixedString<PDF_DICT_VALUE_MAX> v;
  if (!PdfObject::getDictValue("/Length", dictPart, v)) return 0;
  while (v.size() > 0 && (v[0] == ' ' || v[0] == '\t' || v[0] == '\r' || v[0] == '\n')) {
    v.erase_prefix(1);
  }
  while (v.size() > 0 &&
         (v[v.size() - 1] == ' ' || v[v.size() - 1] == '\t' || v[v.size() - 1] == '\r' || v[v.size() - 1] == '\n')) {
    v.resize(v.size() - 1);
  }
  if (v.empty()) return 0;
  const char* p = v.c_str();
  char* e1 = nullptr;
  const unsigned long num = std::strtoul(p, &e1, 10);
  if (e1 == p) return 0;
  while (*e1 == ' ' || *e1 == '\t' || *e1 == '\r' || *e1 == '\n') ++e1;
  if (*e1 == '\0') {
    return static_cast<uint32_t>(num);
  }
  char* e2 = nullptr;
  const unsigned long gen = std::strtoul(e1, &e2, 10);
  (void)gen;
  if (e2 == e1) {
    return static_cast<uint32_t>(num);
  }
  while (*e2 == ' ' || *e2 == '\t') ++e2;
  if (*e2 != 'R' && *e2 != 'r') {
    return static_cast<uint32_t>(num);
  }
  if (!xref) {
    return 0;
  }
  const uint32_t lenObjId = static_cast<uint32_t>(num);
  return resolveIndirectStreamLength(file, xref, lenObjId, static_cast<uint32_t>(gen));
}

}  // namespace

bool PdfObject::readAt(FsFile& file, uint32_t offset, PdfFixedString<PDF_OBJECT_BODY_MAX>& bodyStr,
                       uint32_t* streamOffset, uint32_t* streamLength, const XrefTable* xrefForIndirectLength) {
  bodyStr.clear();
  if (streamOffset) *streamOffset = 0;
  if (streamLength) *streamLength = 0;

  if (!file.seek(offset)) {
    return false;
  }

  uint8_t chunk[kPdfObjectReadChunkBytes];

  constexpr size_t kMaxAcc = PDF_OBJECT_BODY_MAX;

  bool strippedHeader = false;
  size_t objSearchFrom = 0;
  size_t streamSearchFrom = 0;
  size_t endObjSearchFrom = 0;
  while (bodyStr.size() < kMaxAcc) {
    const size_t remaining = kMaxAcc - bodyStr.size() - 1;
    if (remaining == 0) {
      pdfLogErrU32U32("PdfObject::readAt overflow offset/bodySize=", offset, static_cast<uint32_t>(bodyStr.size()));
      pdfLogErr("PdfObject::readAt object body overflow");
      return false;
    }
    const size_t toRead = std::min(remaining, sizeof(chunk));
    const int n = file.read(chunk, toRead);
    if (n <= 0) break;
    if (!bodyStr.append(reinterpret_cast<const char*>(chunk), static_cast<size_t>(n))) {
      pdfLogErrU32U32("PdfObject::readAt append overflow offset/bodySize=", offset,
                      static_cast<uint32_t>(bodyStr.size()));
      pdfLogErr("PdfObject::readAt object body overflow");
      return false;
    }

    if (!strippedHeader) {
      size_t eraseTo = 0;
      const size_t objStart = (objSearchFrom > 3 ? objSearchFrom - 3 : 0);
      if (locateObjKeywordFs(bodyStr, eraseTo, objStart)) {
        bodyStr.erase_prefix(eraseTo);
        while (bodyStr.size() > 0 &&
               (bodyStr[0] == ' ' || bodyStr[0] == '\t' || bodyStr[0] == '\r' || bodyStr[0] == '\n')) {
          bodyStr.erase_prefix(1);
        }
        strippedHeader = true;
        streamSearchFrom = 0;
        endObjSearchFrom = 0;
      } else {
        if (bodyStr.size() > 3) {
          objSearchFrom = bodyStr.size() - 3;
        } else {
          objSearchFrom = bodyStr.size();
        }
      }
    }

    if (!strippedHeader) {
      continue;
    }

    if (streamOffset != nullptr) {
      const std::string_view acc = bodyStr.view();
      const size_t streamStart = (streamSearchFrom > 7 ? streamSearchFrom - 7 : 0);
      const size_t sp = findStreamKeywordSv(acc, streamStart);
      if (sp != std::string_view::npos) {
        size_t dictEnd = sp;
        while (dictEnd > 0 && (bodyStr[dictEnd - 1] == ' ' || bodyStr[dictEnd - 1] == '\t' ||
                               bodyStr[dictEnd - 1] == '\r' || bodyStr[dictEnd - 1] == '\n')) {
          --dictEnd;
        }
        bodyStr.resize(dictEnd);
        size_t dataIdx = sp + 6;
        while (dataIdx < acc.size() && (acc[dataIdx] == '\r' || acc[dataIdx] == '\n')) {
          ++dataIdx;
        }
        const size_t curPos = file.position();
        if (curPos < acc.size()) {
          return false;
        }
        const uint32_t acc0File = static_cast<uint32_t>(curPos - acc.size());
        *streamOffset = acc0File + static_cast<uint32_t>(dataIdx);
        *streamLength = resolveStreamLength(file, xrefForIndirectLength, bodyStr.view());
        if (*streamLength > 0) {
          const uint32_t repaired = recoverStreamLengthFromNearbyEndstream(file, *streamOffset, *streamLength);
          *streamLength = repaired;
        }
        return bodyStr.size() > 0 && *streamLength > 0;
      }
      streamSearchFrom = acc.size();
    }

    const std::string_view acc = bodyStr.view();
    const size_t endScanFrom = (endObjSearchFrom > 6 ? endObjSearchFrom - 6 : 0);
    const size_t ep = streamOffset == nullptr ? findEndobjKeywordSv(acc, endScanFrom) : std::string_view::npos;
    if (ep != std::string_view::npos) {
      size_t end = ep;
      while (end > 0 && (bodyStr[end - 1] == ' ' || bodyStr[end - 1] == '\t' || bodyStr[end - 1] == '\r' ||
                         bodyStr[end - 1] == '\n')) {
        --end;
      }
      bodyStr.resize(end);
      return bodyStr.size() > 0;
    }
    endObjSearchFrom = acc.size();
  }

  if (strippedHeader && streamOffset == nullptr) {
    const size_t endScanFrom = (endObjSearchFrom > 6 ? endObjSearchFrom - 6 : 0);
    const size_t ep = findEndobjKeywordSv(bodyStr.view(), endScanFrom);
    if (ep != std::string_view::npos) {
      size_t end = ep;
      while (end > 0 && (bodyStr[end - 1] == ' ' || bodyStr[end - 1] == '\t' || bodyStr[end - 1] == '\r' ||
                         bodyStr[end - 1] == '\n')) {
        --end;
      }
      bodyStr.resize(end);
      return bodyStr.size() > 0;
    }
  }

  return false;
}

bool PdfObject::getDictValue(const char* key, std::string_view dict, PdfFixedString<PDF_DICT_VALUE_MAX>& out) {
  out.clear();
  const size_t keyLen = std::strlen(key);
  if (keyLen == 0) return false;

  size_t pos = 0;
  while (pos < dict.size()) {
    pos = dict.find(key, pos);
    if (pos == std::string_view::npos) break;
    if (pos > 0 && dict[pos - 1] == '/') {
      pos += keyLen;
      continue;
    }
    const size_t afterKey = pos + keyLen;
    if (afterKey < dict.size()) {
      const unsigned char c = static_cast<unsigned char>(dict[afterKey]);
      if (!std::isspace(c) && c != '/' && c != '<' && c != '[' && c != '(' && c != '>' && c != ']') {
        pos += keyLen;
        continue;
      }
    }
    size_t v = pos + keyLen;
    while (v < dict.size() && (dict[v] == ' ' || dict[v] == '\t' || dict[v] == '\r' || dict[v] == '\n')) {
      ++v;
    }
    if (v >= dict.size()) return false;

    const size_t start = v;
    int depth = 0;
    while (v < dict.size()) {
      if (v + 1 < dict.size() && dict[v] == '<' && dict[v + 1] == '<') {
        depth += 2;
        v += 2;
        continue;
      }
      if (v + 1 < dict.size() && dict[v] == '>' && dict[v + 1] == '>') {
        depth -= 2;
        v += 2;
        if (depth <= 0) {
          break;
        }
        continue;
      }
      if (depth == 0) {
        if (dict[v] == '/') {
          if (v > start) {
            break;
          }
          ++v;
          while (v < dict.size()) {
            const unsigned char c = static_cast<unsigned char>(dict[v]);
            if (std::isspace(c) || c == '/' || c == '(' || c == '[' || c == '<' || c == '>' || c == ']' || c == '{' ||
                c == '}') {
              break;
            }
            ++v;
          }
          return out.assign(dict.data() + start, v - start);
        }
        if (dict[v] == '>' && v + 1 < dict.size() && dict[v + 1] == '>') {
          break;
        }
      }
      ++v;
    }
    return out.assign(dict.data() + start, v - start);
  }
  return false;
}

int PdfObject::getDictInt(const char* key, std::string_view dict, int defaultVal) {
  PdfFixedString<PDF_DICT_VALUE_MAX> v;
  if (!getDictValue(key, dict, v) || v.empty()) return defaultVal;
  const char* p = v.c_str();
  while (*p == ' ' || *p == '\t') ++p;
  if (!*p) return defaultVal;
  char* end = nullptr;
  const long n = std::strtol(p, &end, 10);
  if (end == p) return defaultVal;
  return static_cast<int>(n);
}

uint32_t PdfObject::getDictRef(const char* key, std::string_view dict) {
  PdfFixedString<PDF_DICT_VALUE_MAX> v;
  if (!getDictValue(key, dict, v) || v.empty()) return 0;
  const char* p = v.c_str();
  while (*p == ' ' || *p == '\t') ++p;
  char* end = nullptr;
  const unsigned long id = std::strtoul(p, &end, 10);
  if (end == p) return 0;
  return static_cast<uint32_t>(id);
}
