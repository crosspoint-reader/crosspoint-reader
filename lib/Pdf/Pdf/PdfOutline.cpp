#include "PdfOutline.h"

#include <Logging.h>

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string_view>

#include "PageTree.h"
#include "PdfObject.h"

namespace {

static constexpr uint16_t kWin1252ToU[128] = {
    0x20AC, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x017D, 0x2018,
    0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x017E, 0x0178, 0x00A0,
    0x00A1, 0x00A2, 0x00A3, 0x00A4, 0x00A5, 0x00A6, 0x00A7, 0x00A8, 0x00A9, 0x00AA, 0x00AB, 0x00AC, 0x00AD, 0x00AE,
    0x00AF, 0x00B0, 0x00B1, 0x00B2, 0x00B3, 0x00B4, 0x00B5, 0x00B6, 0x00B7, 0x00B8, 0x00B9, 0x00BA, 0x00BB, 0x00BC,
    0x00BD, 0x00BE, 0x00BF, 0x00C0, 0x00C1, 0x00C2, 0x00C3, 0x00C4, 0x00C5, 0x00C6, 0x00C7, 0x00C8, 0x00C9, 0x00CA,
    0x00CB, 0x00CC, 0x00CD, 0x00CE, 0x00CF, 0x00D0, 0x00D1, 0x00D2, 0x00D3, 0x00D4, 0x00D5, 0x00D6, 0x00D7, 0x00D8,
    0x00D9, 0x00DA, 0x00DB, 0x00DC, 0x00DD, 0x00DE, 0x00DF, 0x00E0, 0x00E1, 0x00E2, 0x00E3, 0x00E4, 0x00E5, 0x00E6,
    0x00E7, 0x00E8, 0x00E9, 0x00EA, 0x00EB, 0x00EC, 0x00ED, 0x00EE, 0x00EF, 0x00F0, 0x00F1, 0x00F2, 0x00F3, 0x00F4,
    0x00F5, 0x00F6, 0x00F7, 0x00F8, 0x00F9, 0x00FA, 0x00FB, 0x00FC, 0x00FD, 0x00FE, 0x00FF};

constexpr size_t kTraversalCap = 64;

void appendUtf8(PdfFixedString<PDF_MAX_OUTLINE_TITLE_BYTES>& out, uint32_t cp) {
  if (cp < 0x80) {
    out.append(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.append(static_cast<char>(0xC0 | (cp >> 6)));
    out.append(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.append(static_cast<char>(0xE0 | (cp >> 12)));
    out.append(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.append(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

void pdfBytesToUtf8(const uint8_t* data, size_t len, PdfFixedString<PDF_MAX_OUTLINE_TITLE_BYTES>& out) {
  out.clear();
  if (len >= 2 && data[0] == 0xFE && data[1] == 0xFF) {
    for (size_t i = 2; i + 1 < len; i += 2) {
      const uint16_t cu = static_cast<uint16_t>((data[i] << 8) | data[i + 1]);
      appendUtf8(out, cu);
    }
    return;
  }
  for (size_t i = 0; i < len; ++i) {
    const unsigned char b = data[i];
    if (b < 0x80) {
      out.append(static_cast<char>(b));
    } else {
      appendUtf8(out, kWin1252ToU[b - 0x80]);
    }
  }
}

void trimVal(PdfFixedString<PDF_DICT_VALUE_MAX>& s) {
  while (s.size() > 0 && (s[0] == ' ' || s[0] == '\t' || s[0] == '\r' || s[0] == '\n')) {
    s.erase_prefix(1);
  }
  while (s.size() > 0) {
    const char c = s[s.size() - 1];
    if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
    s.resize(s.size() - 1);
  }
}

bool decodeTitleValue(PdfFixedString<PDF_DICT_VALUE_MAX> raw, PdfFixedString<PDF_MAX_OUTLINE_TITLE_BYTES>& out) {
  out.clear();
  trimVal(raw);
  if (raw.empty()) return true;
  if (raw[0] == '(') {
    PdfFixedString<PDF_DICT_VALUE_MAX> inner;
    const char* p = raw.c_str() + 1;
    int depth = 1;
    while (*p && depth > 0) {
      if (*p == '\\') {
        ++p;
        if (*p == 'n')
          inner.append('\n');
        else if (*p == 'r')
          inner.append('\r');
        else if (*p == 't')
          inner.append('\t');
        else if (*p)
          inner.append(*p);
        if (*p) ++p;
        continue;
      }
      if (*p == '(') {
        ++depth;
        inner.append(*p++);
        continue;
      }
      if (*p == ')') {
        --depth;
        if (depth == 0) {
          ++p;
          break;
        }
        inner.append(*p++);
        continue;
      }
      inner.append(*p++);
    }
    pdfBytesToUtf8(reinterpret_cast<const uint8_t*>(inner.data()), inner.size(), out);
    return true;
  }
  if (raw[0] == '<' && (raw.size() < 2 || raw[1] != '<')) {
    PdfFixedString<PDF_DICT_VALUE_MAX> bytes;
    size_t i = 1;
    uint8_t acc = 0;
    bool have = false;
    while (i < raw.size()) {
      if (raw[i] == '>') break;
      if (std::isspace(static_cast<unsigned char>(raw[i]))) {
        ++i;
        continue;
      }
      int v = -1;
      char c = raw[i];
      if (c >= '0' && c <= '9')
        v = c - '0';
      else if (c >= 'A' && c <= 'F')
        v = c - 'A' + 10;
      else if (c >= 'a' && c <= 'f')
        v = c - 'a' + 10;
      if (v < 0) break;
      if (!have) {
        acc = static_cast<uint8_t>(v << 4);
        have = true;
      } else {
        acc |= static_cast<uint8_t>(v);
        bytes.append(static_cast<char>(acc));
        have = false;
      }
      ++i;
    }
    pdfBytesToUtf8(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), out);
    return true;
  }
  return out.assign(raw.view());
}

void normalizePdfNameToken(PdfFixedString<PDF_DICT_VALUE_MAX>& s) {
  if (!s.empty() && s[0] == '/') {
    s.erase_prefix(1);
  }
}

bool decodePdfStringToken(std::string_view raw, PdfFixedString<PDF_DICT_VALUE_MAX>& out) {
  while (!raw.empty() && (raw.front() == ' ' || raw.front() == '\t' || raw.front() == '\r' || raw.front() == '\n')) {
    raw.remove_prefix(1);
  }
  while (!raw.empty() && (raw.back() == ' ' || raw.back() == '\t' || raw.back() == '\r' || raw.back() == '\n')) {
    raw.remove_suffix(1);
  }
  out.clear();
  if (raw.empty()) return false;
  if (raw.front() == '(') {
    const char* p = raw.data() + 1;
    const char* end = raw.data() + raw.size();
    int depth = 1;
    while (p < end && depth > 0) {
      if (*p == '\\' && p + 1 < end) {
        ++p;
        if (*p == 'n')
          out.append('\n');
        else if (*p == 'r')
          out.append('\r');
        else if (*p == 't')
          out.append('\t');
        else if (*p == 'b')
          out.append('\b');
        else if (*p == 'f')
          out.append('\f');
        else if (*p >= '0' && *p <= '7') {
          int value = *p - '0';
          int digits = 1;
          while (digits < 3 && p + 1 < end && p[1] >= '0' && p[1] <= '7') {
            ++p;
            value = (value << 3) + (*p - '0');
            ++digits;
          }
          out.append(static_cast<char>(value));
        } else if (*p) {
          out.append(*p);
        }
        if (*p) ++p;
        continue;
      }
      if (*p == '(') {
        ++depth;
        out.append(*p++);
        continue;
      }
      if (*p == ')') {
        --depth;
        if (depth == 0) {
          break;
        }
        out.append(*p++);
        continue;
      }
      out.append(*p++);
    }
    return !out.empty();
  }
  if (raw.front() == '<' && raw.size() > 1 && raw[1] != '<') {
    uint8_t acc = 0;
    bool have = false;
    for (size_t i = 1; i < raw.size(); ++i) {
      const char c = raw[i];
      if (c == '>') break;
      if (std::isspace(static_cast<unsigned char>(c))) continue;
      int v = -1;
      if (c >= '0' && c <= '9')
        v = c - '0';
      else if (c >= 'A' && c <= 'F')
        v = c - 'A' + 10;
      else if (c >= 'a' && c <= 'f')
        v = c - 'a' + 10;
      if (v < 0) return false;
      if (!have) {
        acc = static_cast<uint8_t>(v << 4);
        have = true;
      } else {
        acc |= static_cast<uint8_t>(v);
        out.append(static_cast<char>(acc));
        have = false;
      }
    }
    return !out.empty();
  }
  return out.assign(raw);
}

bool readStringToken(const char*& p, const char* end, PdfFixedString<PDF_DICT_VALUE_MAX>& out) {
  out.clear();
  while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == '[' || *p == ']')) ++p;
  if (p >= end || *p == ']') return false;
  if (*p == '(') {
    const char* start = p;
    int depth = 0;
    do {
      if (*p == '\\' && p + 1 < end) {
        p += 2;
        continue;
      }
      if (*p == '(')
        ++depth;
      else if (*p == ')')
        --depth;
      ++p;
    } while (p < end && depth > 0);
    return decodePdfStringToken(std::string_view(start, static_cast<size_t>(p - start)), out);
  }
  if (*p == '<' && p + 1 < end && p[1] != '<') {
    const char* start = p;
    ++p;
    while (p < end && *p != '>') ++p;
    if (p < end) ++p;
    return decodePdfStringToken(std::string_view(start, static_cast<size_t>(p - start)), out);
  }
  const char* start = p;
  while (p < end && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' && *p != '[' && *p != ']') ++p;
  return out.assign(std::string_view(start, static_cast<size_t>(p - start)));
}

bool readObjectRef(const char*& p, const char* end, uint32_t& outObjId) {
  while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
  if (p >= end || *p == ']') return false;
  char* e = nullptr;
  const unsigned long objId = std::strtoul(p, &e, 10);
  if (e == p) return false;
  p = e;
  while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
  std::strtoul(p, &e, 10);
  p = e;
  while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
  if (p < end && (*p == 'R' || *p == 'r')) {
    ++p;
    outObjId = static_cast<uint32_t>(objId);
    return true;
  }
  return false;
}

bool readArrayToken(const char*& p, const char* end, PdfFixedString<PDF_DICT_VALUE_MAX>& out) {
  out.clear();
  if (p >= end || *p != '[') return false;
  int depth = 1;
  const char* start = p;
  ++p;
  while (p < end && depth > 0) {
    if (*p == '\\' && p + 1 < end) {
      p += 2;
      continue;
    }
    if (*p == '(') {
      ++p;
      int strDepth = 1;
      while (p < end && strDepth > 0) {
        if (*p == '\\' && p + 1 < end) {
          p += 2;
          continue;
        }
        if (*p == '(') {
          ++strDepth;
        } else if (*p == ')') {
          --strDepth;
        }
        ++p;
      }
      continue;
    }
    if (*p == '<' && p + 1 < end && p[1] != '<') {
      ++p;
      while (p < end && *p != '>') ++p;
      if (p < end) ++p;
      continue;
    }
    if (*p == '[') {
      ++depth;
      ++p;
      continue;
    }
    if (*p == ']') {
      --depth;
      ++p;
      continue;
    }
    ++p;
  }
  return out.assign(std::string_view(start, static_cast<size_t>(p - start)));
}

bool parseDestArrayToPage(const PageTree& pageTree, std::string_view dest, uint32_t& outPage) {
  outPage = UINT32_MAX;
  if (dest.empty()) return false;
  const size_t lb = dest.find('[');
  if (lb == std::string_view::npos) return false;
  const char* p = dest.data() + lb + 1;
  const char* end = dest.data() + dest.size();
  while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == '[')) ++p;
  if (p >= end || *p == ']') return false;
  char* e = nullptr;
  const unsigned long id = std::strtoul(p, &e, 10);
  if (e == p) return false;
  const uint32_t pageIndex = pageTree.pageIndexForObjectId(static_cast<uint32_t>(id));
  if (pageIndex == UINT32_MAX) return false;
  outPage = pageIndex;
  return true;
}

bool findNamedDestinationObjectId(FsFile& file, const XrefTable& xref, uint32_t namesObjId, std::string_view target,
                                  uint32_t& outDestObjId, PdfFixedString<PDF_DICT_VALUE_MAX>& outDirectDest,
                                  bool& outHasDirectDest) {
  outDestObjId = 0;
  outHasDirectDest = false;
  outDirectDest.clear();
  if (namesObjId == 0) {
    return false;
  }

  uint32_t destTreeRoot = namesObjId;
  static PdfFixedString<PDF_OBJECT_BODY_MAX> rootBody;
  if (xref.readDictForObject(file, namesObjId, rootBody)) {
    const uint32_t destsObjId = PdfObject::getDictRef("/Dests", rootBody.view());
    if (destsObjId != 0) {
      destTreeRoot = destsObjId;
    }
  }

  static PdfFixedVector<uint32_t, kTraversalCap> stack;
  stack.clear();
  if (!stack.push_back(destTreeRoot)) {
    return false;
  }

  while (!stack.empty()) {
    const uint32_t nodeId = stack.back();
    stack.pop_back();

    static PdfFixedString<PDF_OBJECT_BODY_MAX> body;
    if (!xref.readDictForObject(file, nodeId, body)) {
      continue;
    }

    static PdfFixedString<PDF_DICT_VALUE_MAX> kidsVal;
    if (PdfObject::getDictValue("/Kids", body.view(), kidsVal)) {
      const char* p = kidsVal.c_str();
      const char* end = kidsVal.c_str() + kidsVal.size();
      while (p < end) {
        uint32_t childId = 0;
        if (!readObjectRef(p, end, childId)) {
          ++p;
          continue;
        }
        if (!stack.push_back(childId)) {
          return false;
        }
      }
      continue;
    }

    static PdfFixedString<PDF_DICT_VALUE_MAX> namesVal;
    if (!PdfObject::getDictValue("/Names", body.view(), namesVal)) {
      continue;
    }

    const char* p = namesVal.c_str();
    const char* end = namesVal.c_str() + namesVal.size();
    while (p < end) {
      PdfFixedString<PDF_DICT_VALUE_MAX> key;
      if (!readStringToken(p, end, key)) {
        break;
      }
      normalizePdfNameToken(key);

      while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
      if (p < end && *p == '[') {
        PdfFixedString<PDF_DICT_VALUE_MAX> destValue;
        if (!readArrayToken(p, end, destValue)) {
          break;
        }
        if (key.view() == target) {
          outDirectDest = std::move(destValue);
          outHasDirectDest = true;
          return true;
        }
        continue;
      }

      uint32_t refId = 0;
      if (!readObjectRef(p, end, refId)) {
        break;
      }
      if (key.view() == target) {
        outDestObjId = refId;
        return true;
      }
    }
  }

  return false;
}

bool resolveNamedDestinationToPage(FsFile& file, const XrefTable& xref, const PageTree& pageTree, uint32_t namesObjId,
                                   std::string_view dest, uint32_t& outPage) {
  outPage = UINT32_MAX;
  PdfFixedString<PDF_DICT_VALUE_MAX> token;
  if (!decodePdfStringToken(dest, token)) {
    return false;
  }
  normalizePdfNameToken(token);

  if (!token.empty() && token[0] == '[') {
    return parseDestArrayToPage(pageTree, token.view(), outPage);
  }

  uint32_t destObjId = 0;
  PdfFixedString<PDF_DICT_VALUE_MAX> directDest;
  bool hasDirectDest = false;
  if (findNamedDestinationObjectId(file, xref, namesObjId, token.view(), destObjId, directDest, hasDirectDest)) {
    if (hasDirectDest) {
      return parseDestArrayToPage(pageTree, directDest.view(), outPage);
    }
    static PdfFixedString<PDF_OBJECT_BODY_MAX> destBody;
    if (!xref.readDictForObject(file, destObjId, destBody)) {
      return false;
    }
    return parseDestArrayToPage(pageTree, destBody.view(), outPage);
  }

  return parseDestArrayToPage(pageTree, dest, outPage);
}

bool resolveActionDestination(FsFile& file, const XrefTable& xref, std::string_view actionValue,
                              PdfFixedString<PDF_DICT_VALUE_MAX>& outDest) {
  outDest.clear();
  trimVal(outDest);  // no-op on empty, keeps the helper consistent with other parsers.
  PdfFixedString<PDF_DICT_VALUE_MAX> trimmed;
  if (!trimmed.assign(actionValue)) {
    return false;
  }
  trimVal(trimmed);
  if (trimmed.empty()) {
    return false;
  }
  if (trimmed.size() >= 2 && trimmed[0] == '<' && trimmed[1] == '<') {
    return PdfObject::getDictValue("/D", trimmed.view(), outDest);
  }

  const char* p = trimmed.c_str();
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
  char* e = nullptr;
  const unsigned long objId = std::strtoul(p, &e, 10);
  if (e != p) {
    while (*e == ' ' || *e == '\t' || *e == '\r' || *e == '\n') ++e;
    const char* afterGen = e;
    std::strtoul(afterGen, &e, 10);
    while (*e == ' ' || *e == '\t' || *e == '\r' || *e == '\n') ++e;
    if (*e == 'R' || *e == 'r') {
      static PdfFixedString<PDF_OBJECT_BODY_MAX> actionBody;
      if (!xref.readDictForObject(file, static_cast<uint32_t>(objId), actionBody)) {
        return false;
      }
      return PdfObject::getDictValue("/D", actionBody.view(), outDest);
    }
  }

  return PdfObject::getDictValue("/D", trimmed.view(), outDest);
}

bool walkOutlineItem(FsFile& file, const XrefTable& xref, const PageTree& pageTree, uint32_t namesObjId,
                     uint32_t firstItemId, PdfFixedVector<PdfOutlineEntry, PDF_MAX_OUTLINE_ENTRIES>& out, size_t& count,
                     const size_t maxEntries) {
  static PdfFixedVector<uint32_t, PDF_MAX_OUTLINE_ENTRIES * 2> stack;
  stack.clear();
  if (firstItemId != 0 && !stack.push_back(firstItemId)) {
    return false;
  }

  while (!stack.empty() && count < maxEntries) {
    const uint32_t itemId = stack.back();
    stack.pop_back();

    static PdfFixedString<PDF_OBJECT_BODY_MAX> body;
    if (!xref.readDictForObject(file, itemId, body)) {
      continue;
    }

    PdfOutlineEntry entry;
    static PdfFixedString<PDF_DICT_VALUE_MAX> titleRaw;
    if (PdfObject::getDictValue("/Title", body.view(), titleRaw)) {
      decodeTitleValue(std::move(titleRaw), entry.title);
    }

    static PdfFixedString<PDF_DICT_VALUE_MAX> dest;
    if (!PdfObject::getDictValue("/Dest", body.view(), dest)) {
      dest.clear();
    }
    trimVal(dest);
    if (dest.empty()) {
      static PdfFixedString<PDF_DICT_VALUE_MAX> action;
      if (PdfObject::getDictValue("/A", body.view(), action) && !action.empty()) {
        if (!resolveActionDestination(file, xref, action.view(), dest)) {
          dest.clear();
        }
        trimVal(dest);
      }
    }

    if (!resolveNamedDestinationToPage(file, xref, pageTree, namesObjId, dest.view(), entry.pageNum)) {
      continue;
    }

    if (!entry.title.empty()) {
      if (out.push_back(std::move(entry))) {
        ++count;
      } else {
        return false;
      }
    }

    const uint32_t nextSibling = PdfObject::getDictRef("/Next", body.view());
    if (nextSibling != 0 && !stack.push_back(nextSibling)) {
      return false;
    }

    const uint32_t firstChild = PdfObject::getDictRef("/First", body.view());
    if (firstChild != 0 && !stack.push_back(firstChild)) {
      return false;
    }
  }

  return true;
}

}  // namespace

bool PdfOutlineParser::parse(FsFile& file, const XrefTable& xref, const PageTree& pageTree, uint32_t outlinesObjId,
                             uint32_t namesObjId,
                             PdfFixedVector<PdfOutlineEntry, PDF_MAX_OUTLINE_ENTRIES>& outEntries) {
  outEntries.clear();
  if (outlinesObjId == 0) {
    return true;
  }

  static PdfFixedString<PDF_OBJECT_BODY_MAX> body;
  if (!xref.readDictForObject(file, outlinesObjId, body)) {
    return true;
  }

  const uint32_t first = PdfObject::getDictRef("/First", body.view());
  if (first == 0) {
    return true;
  }

  size_t count = 0;
  constexpr size_t kMax = PDF_MAX_OUTLINE_ENTRIES;
  if (!walkOutlineItem(file, xref, pageTree, namesObjId, first, outEntries, count, kMax)) {
    LOG_ERR("PDF", "outline: traversal stopped early");
  }
  return true;
}
