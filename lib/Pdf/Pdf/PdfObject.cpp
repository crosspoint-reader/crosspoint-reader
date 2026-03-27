#include "PdfObject.h"

#include "XrefTable.h"
#include "PdfLog.h"

#include <cctype>
#include <cstdlib>
#include <cstring>

namespace {

size_t findStreamKeywordSv(std::string_view s) {
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

size_t findEndobjKeywordSv(std::string_view s) {
  size_t p = 0;
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

void trimTrailingWsFs(PdfFixedString<PDF_OBJECT_BODY_MAX>& s) {
  while (s.size() > 0) {
    const char c = s[s.size() - 1];
    if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
    s.resize(s.size() - 1);
  }
}

bool locateObjKeywordFs(PdfFixedString<PDF_OBJECT_BODY_MAX>& acc, size_t& outEraseTo) {
  for (size_t i = 0; i + 3 <= acc.size(); ++i) {
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

uint32_t resolveStreamLength(FsFile& file, const XrefTable* xref, std::string_view dictPart) {
  PdfFixedString<PDF_DICT_VALUE_MAX> v;
  if (!PdfObject::getDictValue("/Length", dictPart, v)) return 0;
  while (v.size() > 0 && (v[0] == ' ' || v[0] == '\t' || v[0] == '\r' || v[0] == '\n')) {
    v.erase_prefix(1);
  }
  while (v.size() > 0 && (v[v.size() - 1] == ' ' || v[v.size() - 1] == '\t' || v[v.size() - 1] == '\r' ||
                          v[v.size() - 1] == '\n')) {
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
  const uint32_t loff = xref->getOffset(lenObjId);
  if (loff == 0) {
    return 0;
  }
  static PdfFixedString<PDF_OBJECT_BODY_MAX> lenBody;
  if (!PdfObject::readAt(file, loff, lenBody, nullptr, nullptr, nullptr)) {
    return 0;
  }
  p = lenBody.c_str();
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
  return static_cast<uint32_t>(std::strtoul(p, nullptr, 10));
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

  uint8_t chunk[512];

  constexpr size_t kMaxAcc = PDF_OBJECT_BODY_MAX;

  bool strippedHeader = false;
  while (bodyStr.size() < kMaxAcc) {
    const size_t remaining = kMaxAcc - bodyStr.size() - 1;
    if (remaining == 0) {
      pdfLogErr("PdfObject::readAt object body overflow");
      return false;
    }
    const size_t toRead = std::min(remaining, sizeof(chunk));
    const int n = file.read(chunk, toRead);
    if (n <= 0) break;
    if (!bodyStr.append(reinterpret_cast<const char*>(chunk), static_cast<size_t>(n))) {
      pdfLogErr("PdfObject::readAt object body overflow");
      return false;
    }

    if (!strippedHeader) {
      size_t eraseTo = 0;
      if (locateObjKeywordFs(bodyStr, eraseTo)) {
        bodyStr.erase_prefix(eraseTo);
        while (bodyStr.size() > 0 && (bodyStr[0] == ' ' || bodyStr[0] == '\t' || bodyStr[0] == '\r' ||
                                      bodyStr[0] == '\n')) {
          bodyStr.erase_prefix(1);
        }
        strippedHeader = true;
      }
    }

    if (!strippedHeader) {
      continue;
    }

    if (streamOffset != nullptr) {
      const std::string_view acc = bodyStr.view();
      const size_t sp = findStreamKeywordSv(acc);
      if (sp != std::string_view::npos) {
        PdfFixedString<PDF_OBJECT_BODY_MAX> dictPart;
        if (!dictPart.assign(acc.data(), sp)) {
          return false;
        }
        trimTrailingWsFs(dictPart);
        bodyStr.clear();
        if (!bodyStr.assign(dictPart.view())) {
          return false;
        }
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
        return bodyStr.size() > 0 && *streamLength > 0;
      }
    }

    const size_t ep = findEndobjKeywordSv(bodyStr.view());
    if (ep != std::string_view::npos && streamOffset == nullptr) {
      PdfFixedString<PDF_OBJECT_BODY_MAX> only;
      if (!only.assign(bodyStr.view().data(), ep)) {
        return false;
      }
      trimTrailingWsFs(only);
      bodyStr.clear();
      if (!bodyStr.assign(only.view())) {
        return false;
      }
      return bodyStr.size() > 0;
    }
  }

  if (strippedHeader && streamOffset == nullptr) {
    const size_t ep = findEndobjKeywordSv(bodyStr.view());
    if (ep != std::string_view::npos) {
      PdfFixedString<PDF_OBJECT_BODY_MAX> only;
      if (!only.assign(bodyStr.view().data(), ep)) {
        return false;
      }
      trimTrailingWsFs(only);
      bodyStr.clear();
      if (!bodyStr.assign(only.view())) {
        return false;
      }
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
            if (std::isspace(c) || c == '/' || c == '(' || c == '[' || c == '<' || c == '>' || c == ']' ||
                c == '{' || c == '}') {
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
