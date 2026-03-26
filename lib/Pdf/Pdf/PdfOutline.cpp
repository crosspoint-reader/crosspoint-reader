#include "PdfOutline.h"

#include <Logging.h>

#include <cctype>
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

uint32_t destStringToPage(const PageTree& pageTree, std::string_view dest) {
  if (dest.empty()) return 0;
  const size_t lb = dest.find('[');
  if (lb == std::string_view::npos) return 0;
  const char* p = dest.data() + lb + 1;
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
  char* end = nullptr;
  const unsigned long id = std::strtoul(p, &end, 10);
  if (end == p) return 0;
  return pageTree.pageIndexForObjectId(static_cast<uint32_t>(id));
}

void walkOutlineItem(FsFile& file, const XrefTable& xref, const PageTree& pageTree, uint32_t itemId,
                     PdfFixedVector<PdfOutlineEntry, PDF_MAX_OUTLINE_ENTRIES>& out, size_t& count,
                     const size_t maxEntries) {
  while (itemId != 0 && count < maxEntries) {
    PdfFixedString<PDF_OBJECT_BODY_MAX> body;
    if (!xref.readDictForObject(file, itemId, body)) break;

    PdfOutlineEntry entry;
    PdfFixedString<PDF_DICT_VALUE_MAX> titleRaw;
    if (PdfObject::getDictValue("/Title", body.view(), titleRaw)) {
      decodeTitleValue(std::move(titleRaw), entry.title);
    }

    PdfFixedString<PDF_DICT_VALUE_MAX> dest;
    if (!PdfObject::getDictValue("/Dest", body.view(), dest)) {
      dest.clear();
    }
    trimVal(dest);
    if (dest.empty()) {
      PdfFixedString<PDF_DICT_VALUE_MAX> action;
      if (PdfObject::getDictValue("/A", body.view(), action) && !action.empty()) {
        if (!PdfObject::getDictValue("/D", action.view(), dest)) {
          dest.clear();
        }
        trimVal(dest);
      }
    }
    entry.pageNum = destStringToPage(pageTree, dest.view());

    if (!entry.title.empty()) {
      if (out.push_back(std::move(entry))) {
        ++count;
      } else {
        break;
      }
    }

    const uint32_t firstChild = PdfObject::getDictRef("/First", body.view());
    if (firstChild != 0) {
      walkOutlineItem(file, xref, pageTree, firstChild, out, count, maxEntries);
    }
    itemId = PdfObject::getDictRef("/Next", body.view());
  }
}

}  // namespace

bool PdfOutlineParser::parse(FsFile& file, const XrefTable& xref, const PageTree& pageTree, uint32_t outlinesObjId,
                             PdfFixedVector<PdfOutlineEntry, PDF_MAX_OUTLINE_ENTRIES>& outEntries) {
  outEntries.clear();
  if (outlinesObjId == 0) {
    return true;
  }

  PdfFixedString<PDF_OBJECT_BODY_MAX> body;
  if (!xref.readDictForObject(file, outlinesObjId, body)) {
    return true;
  }

  const uint32_t first = PdfObject::getDictRef("/First", body.view());
  if (first == 0) {
    return true;
  }

  size_t count = 0;
  constexpr size_t kMax = PDF_MAX_OUTLINE_ENTRIES;
  walkOutlineItem(file, xref, pageTree, first, outEntries, count, kMax);
  return true;
}
