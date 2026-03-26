#include "ContentStream.h"

#include <Logging.h>

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <unordered_map>
#include <vector>

#include "PdfLimits.h"
#include "PdfObject.h"
#include "StreamDecoder.h"

namespace {

// WinAnsiEncoding bytes 0x80–0xFF → Unicode (PDF 1.7, Adobe table)
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

void appendUtf8(std::string& out, uint32_t cp) {
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

std::string pdfBytesToUtf8(const uint8_t* data, size_t len) {
  std::string out;
  if (len >= 2 && data[0] == 0xFE && data[1] == 0xFF) {
    for (size_t i = 2; i + 1 < len; i += 2) {
      const uint16_t cu = static_cast<uint16_t>((data[i] << 8) | data[i + 1]);
      appendUtf8(out, cu);
    }
    return out;
  }
  // UTF-16BE in hex strings: pairs big-endian (common for Identity-H / CJK datasheets).
  if (len >= 2 && (len % 2) == 0) {
    bool allHiZero = true;
    for (size_t i = 0; i < len; i += 2) {
      if (data[i] != 0) {
        allHiZero = false;
        break;
      }
    }
    if (allHiZero) {
      for (size_t i = 0; i + 1 < len; i += 2) {
        const uint16_t cu = static_cast<uint16_t>((data[i] << 8) | data[i + 1]);
        if (cu != 0) {
          appendUtf8(out, cu);
        }
      }
      return out;
    }
    // Do not guess UTF-16BE when high bytes vary: hex is often glyph/CID data, not Unicode.
  }
  for (size_t i = 0; i < len; ++i) {
    const unsigned char b = data[i];
    if (b == 0) {
      continue;
    }
    if (b < 0x80) {
      out.push_back(static_cast<char>(b));
    } else {
      appendUtf8(out, kWin1252ToU[b - 0x80]);
    }
  }
  return out;
}

using CidToUtf8Map = std::unordered_map<uint16_t, std::string>;

static int hexDigitVal(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  return -1;
}

static std::string utf16BeHexToUtf8(const std::string& hex) {
  std::string out;
  if (hex.size() < 2 || (hex.size() % 2) != 0) {
    return out;
  }
  for (size_t i = 0; i + 1 < hex.size(); i += 2) {
    const int hi = hexDigitVal(hex[i]);
    const int lo = hexDigitVal(hex[i + 1]);
    if (hi < 0 || lo < 0) {
      continue;
    }
    const uint16_t cu = static_cast<uint16_t>((hi << 4) | lo);
    if (cu >= 0xD800 && cu <= 0xDFFF) {
      continue;
    }
    appendUtf8(out, cu);
  }
  return out;
}

// Parse PDF ToUnicode CMap (beginbfchar / beginbfrange); enough for common Identity-H fonts.
static bool parseToUnicodeCMap(const std::string& cmap, CidToUtf8Map& out) {
  enum class Mode { None, BfChar, BfRange };
  Mode mode = Mode::None;
  std::istringstream iss(cmap);
  std::string line;
  while (std::getline(iss, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    size_t p = 0;
    while (p < line.size() && std::isspace(static_cast<unsigned char>(line[p]))) {
      ++p;
    }
    const std::string t = line.substr(p);
    if (t.find("beginbfchar") != std::string::npos) {
      mode = Mode::BfChar;
      continue;
    }
    if (t.find("endbfchar") != std::string::npos) {
      mode = Mode::None;
      continue;
    }
    if (t.find("beginbfrange") != std::string::npos) {
      mode = Mode::BfRange;
      continue;
    }
    if (t.find("endbfrange") != std::string::npos) {
      mode = Mode::None;
      continue;
    }
    if (mode == Mode::BfChar) {
      const size_t a = t.find('<');
      if (a == std::string::npos) {
        continue;
      }
      const size_t b = t.find('>', a + 1);
      if (b == std::string::npos) {
        continue;
      }
      const size_t c = t.find('<', b + 1);
      if (c == std::string::npos) {
        continue;
      }
      const size_t d = t.find('>', c + 1);
      if (d == std::string::npos) {
        continue;
      }
      const std::string srcHex = t.substr(a + 1, b - a - 1);
      const std::string dstHex = t.substr(c + 1, d - c - 1);
      if (srcHex.size() != 4) {
        continue;
      }
      char* e0 = nullptr;
      const unsigned long cid = std::strtoul(srcHex.c_str(), &e0, 16);
      if (e0 == srcHex.c_str()) {
        continue;
      }
      out[static_cast<uint16_t>(cid)] = utf16BeHexToUtf8(dstHex);
    } else if (mode == Mode::BfRange) {
      const size_t a = t.find('<');
      if (a == std::string::npos) {
        continue;
      }
      const size_t b = t.find('>', a + 1);
      const size_t c = t.find('<', b + 1);
      const size_t d = t.find('>', c + 1);
      const size_t e = t.find('<', d + 1);
      const size_t f = t.find('>', e + 1);
      if (a == std::string::npos || b == std::string::npos || c == std::string::npos || d == std::string::npos ||
          e == std::string::npos || f == std::string::npos) {
        continue;
      }
      const std::string h1 = t.substr(a + 1, b - a - 1);
      const std::string h2 = t.substr(c + 1, d - c - 1);
      const std::string h3 = t.substr(e + 1, f - e - 1);
      if (h1.size() != 4 || h2.size() != 4 || h3.size() != 4) {
        continue;
      }
      char* e1 = nullptr;
      char* e2 = nullptr;
      char* e3 = nullptr;
      const unsigned long v1 = std::strtoul(h1.c_str(), &e1, 16);
      const unsigned long v2 = std::strtoul(h2.c_str(), &e2, 16);
      const unsigned long v3 = std::strtoul(h3.c_str(), &e3, 16);
      if (e1 == h1.c_str() || e2 == h2.c_str() || e3 == h3.c_str()) {
        continue;
      }
      if (v1 > 0xFFFFUL || v2 > 0xFFFFUL || v3 > 0xFFFFUL || v2 < v1) {
        continue;
      }
      const uint16_t srcFirst = static_cast<uint16_t>(v1);
      const uint16_t srcLast = static_cast<uint16_t>(v2);
      const uint16_t dstFirst = static_cast<uint16_t>(v3);
      for (uint32_t k = 0; k <= static_cast<uint32_t>(srcLast - srcFirst); ++k) {
        const uint16_t cid = static_cast<uint16_t>(srcFirst + k);
        const uint32_t u = static_cast<uint32_t>(dstFirst) + k;
        if (u > 0xFFFFU) {
          break;
        }
        std::string one;
        appendUtf8(one, static_cast<uint16_t>(u));
        out[cid] = std::move(one);
      }
    }
  }
  return !out.empty();
}

[[gnu::noinline]] static bool loadToUnicodeMapForFont(FsFile& file, const XrefTable& xref, uint32_t fontObjId,
                                                       CidToUtf8Map& out) {
  out.clear();
  static PdfFixedString<PDF_OBJECT_BODY_MAX> body;
  if (!xref.readDictForObject(file, fontObjId, body)) {
    return false;
  }
  const uint32_t tuId = PdfObject::getDictRef("/ToUnicode", body.view());
  if (tuId == 0) {
    return false;
  }
  static PdfFixedString<PDF_OBJECT_BODY_MAX> cmapDict;
  static PdfByteBuffer payload;
  bool flate = false;
  if (!xref.readStreamForObject(file, tuId, cmapDict, payload, flate)) {
    return false;
  }
  static PdfByteBuffer decoded;
  size_t got = 0;
  if (flate) {
    got = StreamDecoder::flateDecodeBytes(payload.ptr(), payload.len, decoded.ptr(), decoded.data.size());
  } else {
    got = std::min(payload.len, decoded.data.size());
    std::memcpy(decoded.ptr(), payload.ptr(), got);
  }
  if (got == 0) {
    return false;
  }
  const std::string cmap(reinterpret_cast<const char*>(decoded.ptr()), got);
  return parseToUnicodeCMap(cmap, out);
}

static std::string bytesToUtf8WithCidMap(const uint8_t* data, size_t len, const CidToUtf8Map* map) {
  if (!map || map->empty() || len < 2 || (len % 2) != 0) {
    return pdfBytesToUtf8(data, len);
  }
  std::string out;
  out.reserve(len);
  for (size_t i = 0; i + 1 < len; i += 2) {
    const uint16_t cid = static_cast<uint16_t>((static_cast<uint16_t>(data[i]) << 8) | data[i + 1]);
    const auto it = map->find(cid);
    if (it != map->end() && !it->second.empty()) {
      out += it->second;
    } else {
      out.push_back('?');
    }
  }
  return out;
}

void skipWsComment(char*& p, char* end) {
  while (p < end) {
    if (*p == '%') {
      while (p < end && *p != '\r' && *p != '\n') ++p;
      continue;
    }
    if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
      ++p;
      continue;
    }
    break;
  }
}

bool readPdfStringLiteral(char*& p, char* end, std::string& rawBytes) {
  rawBytes.clear();
  if (p >= end || *p != '(') return false;
  ++p;
  int depth = 1;
  while (p < end && depth > 0) {
    if (*p == '\\') {
      ++p;
      if (p >= end) break;
      char c = *p++;
      if (c >= '0' && c <= '7') {
        int oct = c - '0';
        int count = 1;
        while (count < 3 && p < end && *p >= '0' && *p <= '7') {
          oct = (oct << 3) | (*p++ - '0');
          ++count;
        }
        rawBytes.push_back(static_cast<char>(oct & 0xFF));
        continue;
      }
      if (c == 'n')
        rawBytes.push_back('\n');
      else if (c == 'r')
        rawBytes.push_back('\r');
      else if (c == 't')
        rawBytes.push_back('\t');
      else if (c == 'b')
        rawBytes.push_back('\b');
      else if (c == 'f')
        rawBytes.push_back('\f');
      else
        rawBytes.push_back(c);
      continue;
    }
    if (*p == '(') {
      ++depth;
      rawBytes.push_back(*p++);
      continue;
    }
    if (*p == ')') {
      --depth;
      if (depth == 0) {
        ++p;
        break;
      }
      rawBytes.push_back(*p++);
      continue;
    }
    rawBytes.push_back(*p++);
  }
  return true;
}

bool readHexString(char*& p, char* end, std::string& rawBytes) {
  rawBytes.clear();
  if (p >= end || *p != '<') return false;
  ++p;
  uint8_t acc = 0;
  bool haveNibble = false;
  while (p < end) {
    if (*p == '>') {
      ++p;
      if (haveNibble) rawBytes.push_back(static_cast<char>(acc));
      return true;
    }
    if (std::isspace(static_cast<unsigned char>(*p))) {
      ++p;
      continue;
    }
    int v = -1;
    if (*p >= '0' && *p <= '9')
      v = *p - '0';
    else if (*p >= 'A' && *p <= 'F')
      v = *p - 'A' + 10;
    else if (*p >= 'a' && *p <= 'f')
      v = *p - 'a' + 10;
    if (v < 0) return false;
    if (!haveNibble) {
      acc = static_cast<uint8_t>(v << 4);
      haveNibble = true;
    } else {
      acc |= static_cast<uint8_t>(v);
      rawBytes.push_back(static_cast<char>(acc));
      haveNibble = false;
    }
    ++p;
  }
  return false;
}

bool readNumber(char*& p, char* end, std::string& out) {
  out.clear();
  if (p >= end) return false;
  if (*p == '+' || *p == '-') out.push_back(*p++);
  bool sawDigit = false;
  while (p < end && *p >= '0' && *p <= '9') {
    sawDigit = true;
    out.push_back(*p++);
  }
  if (p < end && *p == '.') {
    out.push_back(*p++);
    while (p < end && *p >= '0' && *p <= '9') {
      sawDigit = true;
      out.push_back(*p++);
    }
  }
  return sawDigit || !out.empty();
}

bool readName(char*& p, char* end, std::string& out) {
  out.clear();
  if (p >= end || *p != '/') return false;
  ++p;
  while (p < end) {
    char c = *p;
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '/' || c == '[' || c == ']' || c == '<' || c == '(' ||
        c == '>' || c == '%') {
      break;
    }
    if (c == '#') {
      if (p + 2 >= end) break;
      char h1 = p[1];
      char h2 = p[2];
      auto hexv = [](char h) -> int {
        if (h >= '0' && h <= '9') return h - '0';
        if (h >= 'A' && h <= 'F') return h - 'A' + 10;
        if (h >= 'a' && h <= 'f') return h - 'a' + 10;
        return -1;
      };
      const int a = hexv(h1);
      const int b = hexv(h2);
      if (a >= 0 && b >= 0) {
        out.push_back(static_cast<char>((a << 4) | b));
        p += 3;
        continue;
      }
    }
    out.push_back(c);
    ++p;
  }
  return !out.empty();
}

bool readOperator(char*& p, char* end, std::string& out) {
  out.clear();
  while (p < end) {
    char c = *p;
    if (std::isalpha(static_cast<unsigned char>(c)) || c == '*' || c == '"' || c == '\'') {
      out.push_back(c);
      ++p;
    } else {
      break;
    }
  }
  return !out.empty();
}

// Capture a PDF array including nested `[` `]`; skips string literals and comments inside.
bool readArrayToken(char*& p, char* end, std::string& out) {
  out.clear();
  if (p >= end || *p != '[') return false;
  int depth = 1;
  char* start = p;
  ++p;
  while (p < end && depth > 0) {
    if (*p == '%') {
      while (p < end && *p != '\r' && *p != '\n') ++p;
      continue;
    }
    if (*p == '\\' && p + 1 < end) {
      p += 2;
      continue;
    }
    if (*p == '(') {
      std::string dummy;
      if (!readPdfStringLiteral(p, end, dummy)) return false;
      continue;
    }
    if (*p == '<' && p + 1 < end && p[1] != '<') {
      std::string dummy;
      if (!readHexString(p, end, dummy)) return false;
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
  out.assign(start, p);
  return true;
}

// Skip `<<` ... `>>` inline dictionary (marked-content properties, etc.).
bool skipInlineDictionary(char*& p, char* end) {
  if (p + 1 >= end || p[0] != '<' || p[1] != '<') return false;
  p += 2;
  int depth = 1;
  while (p < end && depth > 0) {
    if (*p == '%') {
      while (p < end && *p != '\r' && *p != '\n') ++p;
      continue;
    }
    if (p + 1 < end && p[0] == '<' && p[1] == '<') {
      depth++;
      p += 2;
      continue;
    }
    if (p + 1 < end && p[0] == '>' && p[1] == '>') {
      depth--;
      p += 2;
      continue;
    }
    if (*p == '(') {
      std::string dummy;
      if (!readPdfStringLiteral(p, end, dummy)) return false;
      continue;
    }
    if (*p == '<' && p + 1 < end && p[1] != '<') {
      std::string dummy;
      if (!readHexString(p, end, dummy)) return false;
      continue;
    }
    if (*p == '[') {
      std::string dummy;
      if (!readArrayToken(p, end, dummy)) return false;
      continue;
    }
    ++p;
  }
  return depth == 0;
}

// Operand count for graphics/text state operators (PDF content stream).
int popOperandsForOperator(const std::string& op) {
  if (op == "cm" || op == "c") return 6;
  if (op == "re") return 4;
  if (op == "v" || op == "y") return 4;
  if (op == "m" || op == "l") return 2;
  if (op == "w" || op == "J" || op == "j" || op == "M" || op == "ri" || op == "i" || op == "gs") return 1;
  if (op == "Tc" || op == "Tw" || op == "Tz" || op == "TL" || op == "Tr" || op == "Ts") return 1;
  if (op == "d") return 2;
  if (op == "rg" || op == "RG") return 3;
  if (op == "g" || op == "G") return 1;
  return 0;
}

float toFloat(const std::string& s) { return static_cast<float>(std::strtod(s.c_str(), nullptr)); }

[[gnu::noinline]] std::string resolveResourcesDict(FsFile& file, const XrefTable& xref, std::string_view pageBody) {
  static PdfFixedString<PDF_DICT_VALUE_MAX> r;
  if (!PdfObject::getDictValue("/Resources", pageBody, r)) {
    return {};
  }
  while (!r.empty() && (r[0] == ' ' || r[0] == '\t' || r[0] == '\r' || r[0] == '\n')) r.erase_prefix(1);
  if (r.empty()) return {};
  if (r.size() >= 2 && r[0] == '<' && r[1] == '<') return std::string(r.view());
  const uint32_t rid = PdfObject::getDictRef("/Resources", pageBody);
  if (rid == 0) return std::string(r.view());
  static PdfFixedString<PDF_OBJECT_BODY_MAX> body;
  if (!xref.readDictForObject(file, rid, body)) return std::string(r.view());
  return std::string(body.view());
}

[[gnu::noinline]] std::string resolveFontDict(FsFile& file, const XrefTable& xref, std::string_view pageBody) {
  const std::string resBody = resolveResourcesDict(file, xref, pageBody);
  static PdfFixedString<PDF_DICT_VALUE_MAX> r;
  if (!PdfObject::getDictValue("/Font", resBody, r)) {
    return {};
  }
  while (!r.empty() && (r[0] == ' ' || r[0] == '\t' || r[0] == '\r' || r[0] == '\n')) r.erase_prefix(1);
  if (r.empty()) {
    return {};
  }
  if (r.size() >= 2 && r[0] == '<' && r[1] == '<') {
    return std::string(r.view());
  }
  const uint32_t rid = PdfObject::getDictRef("/Font", resBody);
  if (rid == 0) {
    return std::string(r.view());
  }
  static PdfFixedString<PDF_OBJECT_BODY_MAX> body;
  if (!xref.readDictForObject(file, rid, body)) {
    return std::string(r.view());
  }
  return std::string(body.view());
}

uint32_t fontObjectIdForName(const std::string& fontDict, const std::string& name) {
  const std::string key = "/" + name;
  size_t pos = 0;
  while (pos < fontDict.size()) {
    pos = fontDict.find(key, pos);
    if (pos == std::string::npos) {
      break;
    }
    if (pos > 0 && fontDict[pos - 1] == '/') {
      pos += key.size();
      continue;
    }
    size_t v = pos + key.size();
    while (v < fontDict.size() && (fontDict[v] == ' ' || fontDict[v] == '\t')) {
      ++v;
    }
    char* end = nullptr;
    const unsigned long id = std::strtoul(fontDict.c_str() + v, &end, 10);
    if (end == fontDict.c_str() + v) {
      return 0;
    }
    return static_cast<uint32_t>(id);
  }
  return 0;
}

[[gnu::noinline]] std::string getXObjectDict(std::string_view resourcesBody) {
  static PdfFixedString<PDF_DICT_VALUE_MAX> out;
  if (!PdfObject::getDictValue("/XObject", resourcesBody, out)) {
    return {};
  }
  return std::string(out.view());
}

uint8_t fontStyleFromDict(std::string_view fontDictBody);

[[gnu::noinline]] static void updateCurrentCidMapForFont(FsFile& file, const XrefTable& xref,
                                                          const std::string& fontDictBody, const std::string& fontName,
                                                          uint8_t& currentFontStyle,
                                                          std::unordered_map<uint32_t, CidToUtf8Map>& fontCidMaps,
                                                          const CidToUtf8Map*& currentCidMap) {
  const uint32_t fid = fontObjectIdForName(fontDictBody, fontName);
  currentCidMap = nullptr;
  currentFontStyle = PdfTextStyleRegular;
  if (fid == 0) {
    return;
  }

  static PdfFixedString<PDF_OBJECT_BODY_MAX> fbody;
  if (!xref.readDictForObject(file, fid, fbody)) {
    return;
  }

  const std::string_view fbodyView = fbody.view();
  currentFontStyle = fontStyleFromDict(fbodyView);
  const bool isWinAnsi = fbodyView.find("/WinAnsiEncoding") != std::string_view::npos ||
                         fbodyView.find("/MacRomanEncoding") != std::string_view::npos;
  const bool isIdentity = fbodyView.find("/Identity-H") != std::string_view::npos ||
                          fbodyView.find("/Identity-V") != std::string_view::npos;
  if (isWinAnsi || !isIdentity) {
    return;
  }

  auto it = fontCidMaps.find(fid);
  if (it == fontCidMaps.end()) {
    CidToUtf8Map m;
    if (loadToUnicodeMapForFont(file, xref, fid, m) && !m.empty()) {
      it = fontCidMaps.insert({fid, std::move(m)}).first;
    }
  }
  if (it != fontCidMaps.end()) {
    currentCidMap = &it->second;
  }
}

uint32_t xobjectIdForName(const std::string& xobjDict, const std::string& name) {
  const std::string key = "/" + name;
  size_t pos = 0;
  while (pos < xobjDict.size()) {
    pos = xobjDict.find(key, pos);
    if (pos == std::string::npos) break;
    if (pos > 0 && xobjDict[pos - 1] == '/') {
      pos += key.size();
      continue;
    }
    size_t v = pos + key.size();
    while (v < xobjDict.size() && (xobjDict[v] == ' ' || xobjDict[v] == '\t')) ++v;
    char* end = nullptr;
    const unsigned long id = std::strtoul(xobjDict.c_str() + v, &end, 10);
    if (end == xobjDict.c_str() + v) return 0;
    return static_cast<uint32_t>(id);
  }
  return 0;
}

uint8_t fontStyleFromDict(std::string_view fontDictBody) {
  static PdfFixedString<PDF_DICT_VALUE_MAX> fontName;
  fontName.clear();
  if (!PdfObject::getDictValue("/BaseFont", fontDictBody, fontName) &&
      !PdfObject::getDictValue("/FontName", fontDictBody, fontName)) {
    return PdfTextStyleRegular;
  }
  const std::string_view name = fontName.view();
  uint8_t style = PdfTextStyleRegular;
  if (name.find("Bold") != std::string_view::npos || name.find("bold") != std::string_view::npos) {
    style = static_cast<uint8_t>(style | PdfTextStyleBold);
  }
  if (name.find("Italic") != std::string_view::npos || name.find("italic") != std::string_view::npos ||
      name.find("Oblique") != std::string_view::npos || name.find("oblique") != std::string_view::npos) {
    style = static_cast<uint8_t>(style | PdfTextStyleItalic);
  }
  return style;
}

bool fillImageDescriptor(FsFile& file, const XrefTable& xref, uint32_t objId, PdfImageDescriptor& out) {
  const uint32_t off = xref.getOffset(objId);
  if (off == 0) {
    return false;
  }
  static PdfFixedString<PDF_OBJECT_BODY_MAX> body;
  uint32_t so = 0;
  uint32_t sl = 0;
  if (!PdfObject::readAt(file, off, body, &so, &sl, &xref)) {
    return false;
  }
  PdfFixedString<PDF_DICT_VALUE_MAX> st;
  if (!PdfObject::getDictValue("/Subtype", body.view(), st)) return false;
  while (!st.empty() && (st[0] == ' ' || st[0] == '\t')) st.erase_prefix(1);
  while (!st.empty() && (st[st.size() - 1] == ' ' || st[st.size() - 1] == '\t')) st.resize(st.size() - 1);
  if (st.view() != "/Image") return false;

  out.pdfStreamOffset = so;
  out.pdfStreamLength = sl;
  out.width = static_cast<uint16_t>(PdfObject::getDictInt("/Width", body.view(), 0));
  out.height = static_cast<uint16_t>(PdfObject::getDictInt("/Height", body.view(), 0));
  if (body.view().find("/DCTDecode") != std::string_view::npos || body.view().find("/DCT") != std::string_view::npos) {
    out.format = 0;
  } else {
    out.format = 1;
  }
  return out.pdfStreamLength > 0 && out.width > 0 && out.height > 0;
}

struct TmpRun {
  float y = 0;
  std::string utf8;
  uint8_t style = PdfTextStyleRegular;
  uint16_t fontSize = 0;
  uint32_t seq = 0;
};

// UTF-8 code unit length starting at s[i] (1–4), or 1 if invalid/truncated.
static size_t utf8CodepointByteLen(const std::string& s, size_t i) {
  if (i >= s.size()) {
    return 0;
  }
  const unsigned char b0 = static_cast<unsigned char>(s[i]);
  if (b0 < 0x80) {
    return 1;
  }
  if ((b0 & 0xE0) == 0xC0) {
    return i + 1 < s.size() ? 2 : 1;
  }
  if ((b0 & 0xF0) == 0xE0) {
    return i + 2 < s.size() ? 3 : 1;
  }
  if ((b0 & 0xF8) == 0xF0) {
    return i + 3 < s.size() ? 4 : 1;
  }
  return 1;
}

// Decode one UTF-8 codepoint at i; returns bytes consumed (0 on error).
static size_t utf8DecodeAt(const std::string& s, size_t i, uint32_t* cpOut) {
  if (i >= s.size()) {
    return 0;
  }
  const unsigned char b0 = static_cast<unsigned char>(s[i]);
  if (b0 < 0x80) {
    *cpOut = b0;
    return 1;
  }
  if ((b0 & 0xE0) == 0xC0 && i + 1 < s.size()) {
    const unsigned char b1 = static_cast<unsigned char>(s[i + 1]);
    if ((b1 & 0xC0) != 0x80) {
      return 0;
    }
    *cpOut = ((b0 & 0x1FU) << 6) | (b1 & 0x3FU);
    return 2;
  }
  if ((b0 & 0xF0) == 0xE0 && i + 2 < s.size()) {
    const unsigned char b1 = static_cast<unsigned char>(s[i + 1]);
    const unsigned char b2 = static_cast<unsigned char>(s[i + 2]);
    if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80) {
      return 0;
    }
    *cpOut = ((b0 & 0x0FU) << 12) | ((b1 & 0x3FU) << 6) | (b2 & 0x3FU);
    return 3;
  }
  if ((b0 & 0xF8) == 0xF0 && i + 3 < s.size()) {
    const unsigned char b1 = static_cast<unsigned char>(s[i + 1]);
    const unsigned char b2 = static_cast<unsigned char>(s[i + 2]);
    const unsigned char b3 = static_cast<unsigned char>(s[i + 3]);
    if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80 || (b3 & 0xC0) != 0x80) {
      return 0;
    }
    *cpOut = ((b0 & 0x07U) << 18) | ((b1 & 0x3FU) << 12) | ((b2 & 0x3FU) << 6) | (b3 & 0x3FU);
    return 4;
  }
  return 0;
}

// Drop C0 controls (except tab/LF/CR), combining marks, and variation selectors — common PDF noise.
static bool keepExtractedCodepoint(uint32_t cp) {
  if (cp < 0x20U) {
    return cp == 0x09U || cp == 0x0AU || cp == 0x0DU;
  }
  if (cp >= 0x300U && cp <= 0x36FU) {
    return false;
  }
  if (cp >= 0xFE00U && cp <= 0xFE0FU) {
    return false;
  }
  return true;
}

static void stripPdfExtractedNoiseUtf8(std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size();) {
    uint32_t cp = 0;
    const size_t n = utf8DecodeAt(s, i, &cp);
    if (n == 0) {
      out.push_back(s[i]);
      ++i;
      continue;
    }
    if (keepExtractedCodepoint(cp)) {
      for (size_t j = 0; j < n; ++j) {
        out.push_back(s[i + j]);
      }
    }
    i += n;
  }
  s = std::move(out);
}

// Collapse ASCII + common PDF Unicode spaces to a single ASCII space; copy other UTF-8 as-is.
static void normalizePdfText(std::string& s) {
  std::string out;
  out.reserve(s.size());
  bool pendingSpace = false;
  for (size_t i = 0; i < s.size();) {
    const unsigned char b0 = static_cast<unsigned char>(s[i]);
    // U+00A0 NO-BREAK SPACE (WinAnsi 0xA0 → UTF-8) — looks empty in plain terminals
    if (b0 == 0xC2 && i + 1 < s.size() && static_cast<unsigned char>(s[i + 1]) == 0xA0) {
      pendingSpace = true;
      i += 2;
      continue;
    }
    if (b0 == ' ' || b0 == '\t' || b0 == '\r' || b0 == '\n') {
      pendingSpace = true;
      i += 1;
      continue;
    }
    if (pendingSpace && !out.empty()) {
      out.push_back(' ');
    }
    pendingSpace = false;
    const size_t n = utf8CodepointByteLen(s, i);
    for (size_t j = 0; j < n && i + j < s.size(); ++j) {
      out.push_back(s[i + j]);
    }
    i += n;
  }
  s = std::move(out);
  stripPdfExtractedNoiseUtf8(s);
}

void flushTextGroup(std::vector<TmpRun>& runs, PdfPage& page, uint32_t& blockCounter) {
  if (runs.empty()) return;
  std::sort(runs.begin(), runs.end(), [](const TmpRun& a, const TmpRun& b) {
    if (a.y != b.y) return a.y > b.y;
    return a.seq < b.seq;
  });
  constexpr float kLineThresh = 20.0f;

  auto pushBlock = [&](std::string& block, uint32_t hint, uint16_t maxFontSize, uint8_t styleBits) {
    normalizePdfText(block);
    if (block.empty()) {
      return;
    }
    if (block.size() <= 120) {
      const bool strongStyle = (styleBits & (PdfTextStyleBold | PdfTextStyleItalic)) != 0;
      const bool bigText = maxFontSize >= 14;
      if (bigText || (strongStyle && blockCounter < 3) || (blockCounter == 0 && block.size() <= 80)) {
        styleBits = static_cast<uint8_t>(styleBits | PdfTextStyleHeader);
      }
    }
    PdfTextBlock tb;
    if (!tb.text.assign(block)) {
      return;
    }
    tb.style = styleBits;
    tb.orderHint = hint;
    page.textBlocks.push_back(std::move(tb));
    page.drawOrder.push_back({false, static_cast<uint32_t>(page.textBlocks.size() - 1)});
    ++blockCounter;
  };

  std::string block = runs[0].utf8;
  uint32_t hint = static_cast<uint32_t>(runs[0].y);
  uint16_t maxFontSize = runs[0].fontSize;
  uint8_t styleBits = runs[0].style;
  for (size_t i = 1; i < runs.size(); ++i) {
    if (std::fabs(runs[i].y - runs[i - 1].y) < kLineThresh) {
      // TJ fragments are concatenated without an extra separator; spacing is inside the strings.
      block += runs[i].utf8;
      styleBits = static_cast<uint8_t>(styleBits | runs[i].style);
      maxFontSize = std::max(maxFontSize, runs[i].fontSize);
    } else {
      pushBlock(block, hint, maxFontSize, styleBits);
      block = runs[i].utf8;
      hint = static_cast<uint32_t>(runs[i].y);
      maxFontSize = runs[i].fontSize;
      styleBits = runs[i].style;
    }
  }
  pushBlock(block, hint, maxFontSize, styleBits);
  runs.clear();
}

bool runContentOperators(char* p, char* end, FsFile& file, const XrefTable& xref,
                         std::string_view pageObjectBody, PdfPage& outPage) {
  const std::string resBody = resolveResourcesDict(file, xref, pageObjectBody);
  const std::string xobjDict = getXObjectDict(resBody);
  const std::string fontDictBody = resolveFontDict(file, xref, pageObjectBody);
  std::unordered_map<uint32_t, CidToUtf8Map> fontCidMaps;
  const CidToUtf8Map* currentCidMap = nullptr;

  bool inText = false;
  float textX = 0;
  float textY = 0;
  float lineSpacing = 0;
  uint8_t currentFontStyle = PdfTextStyleRegular;
  uint16_t currentFontSize = 0;
  uint32_t textBlockCounter = 0;
  uint32_t seqCounter = 0;
  std::vector<TmpRun> runs;
  std::vector<std::string> stack;

  while (p < end) {
    skipWsComment(p, end);
    if (p >= end) break;

    if (*p == '(') {
      std::string raw;
      if (!readPdfStringLiteral(p, end, raw)) break;
      stack.push_back(std::move(raw));
      continue;
    }
    if (*p == '<' && p + 1 < end && p[1] == '<') {
      if (!skipInlineDictionary(p, end)) break;
      stack.push_back(std::string());
      continue;
    }
    if (*p == '<' && p + 1 < end && p[1] != '<') {
      std::string raw;
      if (!readHexString(p, end, raw)) break;
      stack.push_back(std::move(raw));
      continue;
    }
    if (*p == '[') {
      std::string arr;
      if (!readArrayToken(p, end, arr)) break;
      stack.push_back(std::move(arr));
      continue;
    }
    if (*p == '/' && (p + 1 >= end || p[1] != '/')) {
      std::string name;
      if (!readName(p, end, name)) break;
      stack.push_back(name);
      continue;
    }
    if ((*p >= '0' && *p <= '9') || *p == '-' || *p == '+' ||
        (*p == '.' && p + 1 < end && p[1] >= '0' && p[1] <= '9')) {
      std::string num;
      if (!readNumber(p, end, num)) break;
      stack.push_back(std::move(num));
      continue;
    }

    std::string op;
    if (!readOperator(p, end, op)) {
      ++p;
      continue;
    }

    if (op == "BT") {
      stack.clear();
      inText = true;
      runs.clear();
      textX = textY = 0;
      lineSpacing = 0;
      currentFontStyle = PdfTextStyleRegular;
      currentFontSize = 0;
    } else if (op == "ET") {
      if (inText) {
        flushTextGroup(runs, outPage, textBlockCounter);
      }
      inText = false;
    } else if (op == "Tm" && stack.size() >= 6) {
      const float f = toFloat(stack.back());
      stack.pop_back();
      const float e = toFloat(stack.back());
      stack.pop_back();
      stack.pop_back();
      stack.pop_back();
      stack.pop_back();
      stack.pop_back();
      textX = e;
      textY = f;
    } else if (op == "Td" && stack.size() >= 2) {
      const float dy = toFloat(stack.back());
      stack.pop_back();
      const float dx = toFloat(stack.back());
      stack.pop_back();
      textX += dx;
      textY += dy;
    } else if (op == "TD" && stack.size() >= 2) {
      const float dy = toFloat(stack.back());
      stack.pop_back();
      const float dx = toFloat(stack.back());
      stack.pop_back();
      lineSpacing = -dy;
      textX += dx;
      textY += dy;
    } else if (op == "T*") {
      textY -= lineSpacing;
    } else if (op == "Tj" && !stack.empty()) {
      const std::string raw = stack.back();
      stack.pop_back();
      TmpRun r;
      r.y = textY;
      r.style = currentFontStyle;
      r.fontSize = currentFontSize;
      r.seq = seqCounter++;
      r.utf8 = bytesToUtf8WithCidMap(reinterpret_cast<const uint8_t*>(raw.data()), raw.size(), currentCidMap);
      if (!r.utf8.empty()) runs.push_back(std::move(r));
    } else if (op == "TJ" && !stack.empty()) {
      std::string arr = stack.back();
      stack.pop_back();
      char* q = arr.data();
      char* qend = q + arr.size();
      skipWsComment(q, qend);
      if (q < qend && *q == '[') ++q;
      while (q < qend) {
        skipWsComment(q, qend);
        if (q >= qend || *q == ']') break;
        if (*q == '(') {
          std::string raw;
          if (!readPdfStringLiteral(q, qend, raw)) break;
          TmpRun r;
          r.y = textY;
          r.style = currentFontStyle;
          r.fontSize = currentFontSize;
          r.seq = seqCounter++;
          r.utf8 = bytesToUtf8WithCidMap(reinterpret_cast<const uint8_t*>(raw.data()), raw.size(), currentCidMap);
          if (!r.utf8.empty()) runs.push_back(std::move(r));
        } else if (*q == '<' && q + 1 < qend && q[1] != '<') {
          std::string raw;
          if (!readHexString(q, qend, raw)) break;
          TmpRun r;
          r.y = textY;
          r.style = currentFontStyle;
          r.fontSize = currentFontSize;
          r.seq = seqCounter++;
          r.utf8 = bytesToUtf8WithCidMap(reinterpret_cast<const uint8_t*>(raw.data()), raw.size(), currentCidMap);
          if (!r.utf8.empty()) runs.push_back(std::move(r));
        } else if ((*q >= '0' && *q <= '9') || *q == '-' || *q == '+' || *q == '.') {
          std::string num;
          readNumber(q, qend, num);
        } else {
          ++q;
        }
      }
    } else if ((op == "'" || op == "\"") && !stack.empty()) {
      textY -= lineSpacing;
      const std::string raw = stack.back();
      stack.pop_back();
      TmpRun r;
      r.y = textY;
      r.style = currentFontStyle;
      r.fontSize = currentFontSize;
      r.seq = seqCounter++;
      r.utf8 = bytesToUtf8WithCidMap(reinterpret_cast<const uint8_t*>(raw.data()), raw.size(), currentCidMap);
      if (!r.utf8.empty()) runs.push_back(std::move(r));
    } else if (op == "Tf" && stack.size() >= 2) {
      currentFontSize = static_cast<uint16_t>(std::max(0.0f, toFloat(stack.back())));
      stack.pop_back();
      const std::string fontName = stack.back();
      stack.pop_back();
      updateCurrentCidMapForFont(file, xref, fontDictBody, fontName, currentFontStyle, fontCidMaps, currentCidMap);
    } else if (op == "Do" && !stack.empty()) {
      const std::string name = stack.back();
      stack.pop_back();
      const uint32_t xid = xobjectIdForName(xobjDict, name);
      if (xid != 0) {
        PdfImageDescriptor img{};
        if (fillImageDescriptor(file, xref, xid, img)) {
          outPage.images.push_back(img);
          outPage.drawOrder.push_back({true, static_cast<uint32_t>(outPage.images.size() - 1)});
        }
      }
    } else if (op == "BDC" && stack.size() >= 2) {
      stack.pop_back();
      stack.pop_back();
    } else if (op == "EMC" || op == "BMC") {
    } else if ((op == "MP" || op == "DP") && !stack.empty()) {
      stack.pop_back();
    } else {
      const int n = popOperandsForOperator(op);
      if (n > 0 && stack.size() >= static_cast<size_t>(n)) {
        for (int i = 0; i < n; ++i) stack.pop_back();
      }
    }
  }

  return true;
}

}  // namespace

bool ContentStream::parse(FsFile& file, uint32_t streamOffset, uint32_t streamLen, bool isCompressed,
                          const XrefTable& xref, std::string_view pageObjectBody, PdfPage& outPage) {
  outPage.textBlocks.clear();
  outPage.images.clear();
  outPage.drawOrder.clear();

#ifdef HAL_STORAGE_STUB
  const size_t kMaxStream = pdfContentStreamMaxBytes();
  auto* buf = static_cast<uint8_t*>(malloc(kMaxStream + 1));
  if (!buf) {
    LOG_ERR("PDF", "ContentStream: malloc failed (need %zu bytes for page stream)", kMaxStream);
    return false;
  }

  size_t got = 0;
  if (isCompressed) {
    got = StreamDecoder::flateDecode(file, streamOffset, streamLen, buf, kMaxStream);
  } else {
    if (!file.seek(streamOffset)) {
      free(buf);
      return false;
    }
    const int rd = file.read(buf, std::min<size_t>(static_cast<size_t>(streamLen), kMaxStream));
    if (rd < 0) {
      free(buf);
      return false;
    }
    got = static_cast<size_t>(rd);
  }
  buf[got] = '\0';

  char* p = reinterpret_cast<char*>(buf);
  char* endp = p + got;
  const bool ok = runContentOperators(p, endp, file, xref, pageObjectBody, outPage);
  free(buf);
  return ok;
#else
  const size_t kMaxStream = pdfContentStreamMaxBytes();
  auto* buf = static_cast<uint8_t*>(malloc(kMaxStream + 1));
  if (!buf) {
    LOG_ERR("PDF", "ContentStream: malloc failed (need %zu bytes for page stream)", kMaxStream);
    return false;
  }

  size_t got = 0;
  if (isCompressed) {
    got = StreamDecoder::flateDecode(file, streamOffset, streamLen, buf, kMaxStream);
  } else {
    if (!file.seek(streamOffset)) {
      free(buf);
      return false;
    }
    const int rd = file.read(buf, std::min<size_t>(static_cast<size_t>(streamLen), kMaxStream));
    if (rd < 0) {
      free(buf);
      return false;
    }
    got = static_cast<size_t>(rd);
  }
  buf[got] = '\0';

  char* p = reinterpret_cast<char*>(buf);
  char* endp = p + got;
  const bool ok = runContentOperators(p, endp, file, xref, pageObjectBody, outPage);
  free(buf);
  return ok;
#endif
}

bool ContentStream::parseBuffer(const uint8_t* streamBytes, size_t streamLen, bool isCompressed, FsFile& file,
                                const XrefTable& xref, std::string_view pageObjectBody, PdfPage& outPage) {
  outPage.textBlocks.clear();
  outPage.images.clear();
  outPage.drawOrder.clear();

  const size_t kMaxStream = pdfContentStreamMaxBytes();
  auto* buf = static_cast<uint8_t*>(malloc(kMaxStream + 1));
  if (!buf) {
    LOG_ERR("PDF", "ContentStream: malloc failed (need %zu bytes for page stream)", kMaxStream);
    return false;
  }

  size_t got = 0;
  if (isCompressed) {
    got = StreamDecoder::flateDecodeBytes(streamBytes, streamLen, buf, kMaxStream);
  } else {
    got = std::min(streamLen, kMaxStream);
    std::memcpy(buf, streamBytes, got);
  }
  buf[got] = '\0';

  char* p = reinterpret_cast<char*>(buf);
  char* endp = p + got;
  const bool ok = runContentOperators(p, endp, file, xref, pageObjectBody, outPage);
  free(buf);
  return ok;
}
