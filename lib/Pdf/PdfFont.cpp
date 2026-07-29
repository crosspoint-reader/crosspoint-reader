#include "PdfFont.h"

#include <Logging.h>
#include <Memory.h>

#include <cstring>

#include "PdfDoc.h"
#include "PdfLexer.h"
#include "PdfUtil.h"

using Kind = PdfObj::Kind;

namespace {

constexpr size_t MAX_CMAP_ENTRIES = 65536;

// cp1252 0x80-0x9F -> Unicode (0 = undefined -> U+FFFD at use).
constexpr uint16_t CP1252_HIGH[32] = {8364, 0,    8218, 402,  8222, 8230, 8224, 8225, 710,  8240, 352,
                                      8249, 338,  0,    381,  0,    0,    8216, 8217, 8220, 8221, 8226,
                                      8211, 8212, 732,  8482, 353,  8250, 339,  0,    382,  376};

// MacRomanEncoding 0x80-0xFF -> Unicode.
constexpr uint16_t MACROMAN_HIGH[128] = {
    0x00C4, 0x00C5, 0x00C7, 0x00C9, 0x00D1, 0x00D6, 0x00DC, 0x00E1, 0x00E0, 0x00E2, 0x00E4, 0x00E3, 0x00E5,
    0x00E7, 0x00E9, 0x00E8, 0x00EA, 0x00EB, 0x00ED, 0x00EC, 0x00EE, 0x00EF, 0x00F1, 0x00F3, 0x00F2, 0x00F4,
    0x00F6, 0x00F5, 0x00FA, 0x00F9, 0x00FB, 0x00FC, 0x2020, 0x00B0, 0x00A2, 0x00A3, 0x00A7, 0x2022, 0x00B6,
    0x00DF, 0x00AE, 0x00A9, 0x2122, 0x00B4, 0x00A8, 0x2260, 0x00C6, 0x00D8, 0x221E, 0x00B1, 0x2264, 0x2265,
    0x00A5, 0x00B5, 0x2202, 0x2211, 0x220F, 0x03C0, 0x222B, 0x00AA, 0x00BA, 0x03A9, 0x00E6, 0x00F8, 0x00BF,
    0x00A1, 0x00AC, 0x221A, 0x0192, 0x2248, 0x2206, 0x00AB, 0x00BB, 0x2026, 0x00A0, 0x00C0, 0x00C3, 0x00D5,
    0x0152, 0x0153, 0x2013, 0x2014, 0x201C, 0x201D, 0x2018, 0x2019, 0x00F7, 0x25CA, 0x00FF, 0x0178, 0x2044,
    0x20AC, 0x2039, 0x203A, 0xFB01, 0xFB02, 0x2021, 0x00B7, 0x201A, 0x201E, 0x2030, 0x00C2, 0x00CA, 0x00C1,
    0x00CB, 0x00C8, 0x00CD, 0x00CE, 0x00CF, 0x00CC, 0x00D3, 0x00D4, 0xF8FF, 0x00D2, 0x00DA, 0x00DB, 0x00D9,
    0x0131, 0x02C6, 0x02DC, 0x00AF, 0x02D8, 0x02D9, 0x02DA, 0x00B8, 0x02DD, 0x02DB, 0x02C7};

// Adobe Glyph List subset: names used by /Differences in text-bearing PDFs.
// Single-character names (a-z A-Z 0-9 etc.) are handled programmatically.
struct GlyphName {
  const char* name;
  uint16_t cp;
};
constexpr GlyphName AGL[] = {
    {"space", 0x20},
    {"exclam", 0x21},
    {"quotedbl", 0x22},
    {"numbersign", 0x23},
    {"dollar", 0x24},
    {"percent", 0x25},
    {"ampersand", 0x26},
    {"quotesingle", 0x27},
    {"parenleft", 0x28},
    {"parenright", 0x29},
    {"asterisk", 0x2A},
    {"plus", 0x2B},
    {"comma", 0x2C},
    {"hyphen", 0x2D},
    {"period", 0x2E},
    {"slash", 0x2F},
    {"zero", 0x30},
    {"one", 0x31},
    {"two", 0x32},
    {"three", 0x33},
    {"four", 0x34},
    {"five", 0x35},
    {"six", 0x36},
    {"seven", 0x37},
    {"eight", 0x38},
    {"nine", 0x39},
    {"colon", 0x3A},
    {"semicolon", 0x3B},
    {"less", 0x3C},
    {"equal", 0x3D},
    {"greater", 0x3E},
    {"question", 0x3F},
    {"at", 0x40},
    {"bracketleft", 0x5B},
    {"backslash", 0x5C},
    {"bracketright", 0x5D},
    {"asciicircum", 0x5E},
    {"underscore", 0x5F},
    {"grave", 0x60},
    {"braceleft", 0x7B},
    {"bar", 0x7C},
    {"braceright", 0x7D},
    {"asciitilde", 0x7E},
    {"quoteleft", 0x2018},
    {"quoteright", 0x2019},
    {"quotedblleft", 0x201C},
    {"quotedblright", 0x201D},
    {"quotesinglbase", 0x201A},
    {"quotedblbase", 0x201E},
    {"endash", 0x2013},
    {"emdash", 0x2014},
    {"bullet", 0x2022},
    {"ellipsis", 0x2026},
    {"fi", 0xFB01},
    {"fl", 0xFB02},
    {"dagger", 0x2020},
    {"daggerdbl", 0x2021},
    {"guillemotleft", 0xAB},
    {"guillemotright", 0xBB},
    {"guilsinglleft", 0x2039},
    {"guilsinglright", 0x203A},
    {"germandbls", 0xDF},
    {"fraction", 0x2044},
    {"florin", 0x192},
    {"minus", 0x2212},
    {"periodcentered", 0xB7},
    {"perthousand", 0x2030},
    {"euro", 0x20AC},
    {"sterling", 0xA3},
    {"yen", 0xA5},
    {"cent", 0xA2},
    {"currency", 0xA4},
    {"copyright", 0xA9},
    {"registered", 0xAE},
    {"trademark", 0x2122},
    {"degree", 0xB0},
    {"plusminus", 0xB1},
    {"onehalf", 0xBD},
    {"onequarter", 0xBC},
    {"threequarters", 0xBE},
    {"questiondown", 0xBF},
    {"exclamdown", 0xA1},
    {"section", 0xA7},
    {"paragraph", 0xB6},
    {"brokenbar", 0xA6},
    {"logicalnot", 0xAC},
    {"mu", 0xB5},
    {"multiply", 0xD7},
    {"divide", 0xF7},
    {"ordfeminine", 0xAA},
    {"ordmasculine", 0xBA},
    {"onesuperior", 0xB9},
    {"twosuperior", 0xB2},
    {"threesuperior", 0xB3},
    {"acute", 0xB4},
    {"cedilla", 0xB8},
    {"dieresis", 0xA8},
    {"macron", 0xAF},
    {"tilde", 0x2DC},
    {"circumflex", 0x2C6},
    {"caron", 0x2C7},
    {"breve", 0x2D8},
    {"dotaccent", 0x2D9},
    {"ring", 0x2DA},
    {"ogonek", 0x2DB},
    {"hungarumlaut", 0x2DD},
    {"dotlessi", 0x131},
    {"agrave", 0xE0},
    {"aacute", 0xE1},
    {"acircumflex", 0xE2},
    {"atilde", 0xE3},
    {"adieresis", 0xE4},
    {"aring", 0xE5},
    {"ae", 0xE6},
    {"ccedilla", 0xE7},
    {"egrave", 0xE8},
    {"eacute", 0xE9},
    {"ecircumflex", 0xEA},
    {"edieresis", 0xEB},
    {"igrave", 0xEC},
    {"iacute", 0xED},
    {"icircumflex", 0xEE},
    {"idieresis", 0xEF},
    {"eth", 0xF0},
    {"ntilde", 0xF1},
    {"ograve", 0xF2},
    {"oacute", 0xF3},
    {"ocircumflex", 0xF4},
    {"otilde", 0xF5},
    {"odieresis", 0xF6},
    {"oslash", 0xF8},
    {"ugrave", 0xF9},
    {"uacute", 0xFA},
    {"ucircumflex", 0xFB},
    {"udieresis", 0xFC},
    {"yacute", 0xFD},
    {"thorn", 0xFE},
    {"ydieresis", 0xFF},
    {"Agrave", 0xC0},
    {"Aacute", 0xC1},
    {"Acircumflex", 0xC2},
    {"Atilde", 0xC3},
    {"Adieresis", 0xC4},
    {"Aring", 0xC5},
    {"AE", 0xC6},
    {"Ccedilla", 0xC7},
    {"Egrave", 0xC8},
    {"Eacute", 0xC9},
    {"Ecircumflex", 0xCA},
    {"Edieresis", 0xCB},
    {"Igrave", 0xCC},
    {"Iacute", 0xCD},
    {"Icircumflex", 0xCE},
    {"Idieresis", 0xCF},
    {"Eth", 0xD0},
    {"Ntilde", 0xD1},
    {"Ograve", 0xD2},
    {"Oacute", 0xD3},
    {"Ocircumflex", 0xD4},
    {"Otilde", 0xD5},
    {"Odieresis", 0xD6},
    {"Oslash", 0xD8},
    {"Ugrave", 0xD9},
    {"Uacute", 0xDA},
    {"Ucircumflex", 0xDB},
    {"Udieresis", 0xDC},
    {"Yacute", 0xDD},
    {"Thorn", 0xDE},
    {"oe", 0x153},
    {"OE", 0x152},
    {"scaron", 0x161},
    {"Scaron", 0x160},
    {"zcaron", 0x17E},
    {"Zcaron", 0x17D},
    {"Ydieresis", 0x178},
    {"nbspace", 0xA0},
};

int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

uint16_t glyphNameToUnicode(const std::string& n) {
  if (n.empty()) return 0xFFFD;
  if (n.size() == 1 && n[0] >= 0x21 && n[0] <= 0x7E) return (uint16_t)n[0];  // a-z A-Z 0-9 punctuation: self
  // uniXXXX / uXXXX
  if (n.size() == 7 && n.compare(0, 3, "uni") == 0) {
    uint32_t v = 0;
    for (int i = 3; i < 7; i++) {
      const int h = hexVal(n[(size_t)i]);
      if (h < 0) return 0xFFFD;
      v = (v << 4) | (uint32_t)h;
    }
    return (uint16_t)v;
  }
  if (n.size() == 5 && n[0] == 'u') {
    uint32_t v = 0;
    for (int i = 1; i < 5; i++) {
      const int h = hexVal(n[(size_t)i]);
      if (h < 0) {
        v = 0;
        break;
      }
      v = (v << 4) | (uint32_t)h;
    }
    if (v) return (uint16_t)v;
  }
  for (const auto& g : AGL) {
    if (n == g.name) return g.cp;
  }
  return 0xFFFD;
}

// Big-endian bytes of a CMap hex-string operand -> integer code.
uint32_t codeOf(const std::string& bytes) {
  uint32_t v = 0;
  const size_t n = bytes.size() > 4 ? 4 : bytes.size();
  for (size_t i = 0; i < n; i++) v = (v << 8) | (uint8_t)bytes[i];
  return v;
}

}  // namespace

// ---------------------------------------------------------------------------

void PdfFont::build(PdfDoc& doc, const PdfObj& fontDict) {
  const PdfObj* st = fontDict.find("Subtype");
  isCID = st && st->isName("Type0");
  codeBytes = isCID ? 2 : 1;

  for (int i = 0; i < 256; i++) simple[i] = (uint16_t)i;  // StandardEncoding ~ latin1 approximation

  if (!isCID) {
    PdfObj encStore, diffStore;
    const PdfObj* enc = doc.resolve(fontDict.find("Encoding"), encStore);
    const PdfObj* baseName = nullptr;
    const PdfObj* diffs = nullptr;
    if (enc) {
      if (enc->kind == Kind::Name) {
        baseName = enc;
      } else if (enc->isDict()) {
        baseName = enc->find("BaseEncoding");
        diffs = doc.resolve(enc->find("Differences"), diffStore);
      }
    }
    if (baseName && baseName->isName("WinAnsiEncoding")) {
      for (int i = 0; i < 32; i++) simple[0x80 + i] = CP1252_HIGH[i];
    } else if (baseName && baseName->isName("MacRomanEncoding")) {
      for (int i = 0; i < 128; i++) simple[0x80 + i] = MACROMAN_HIGH[i];
    }
    if (diffs && diffs->kind == Kind::Array) {
      uint32_t cur = 256;
      for (const auto& e : diffs->arr) {
        if (e.isNum()) {
          cur = (e.num >= 0 && e.num < 256) ? (uint32_t)e.num : 256;
        } else if (e.kind == Kind::Name && cur < 256) {
          simple[cur++] = glyphNameToUnicode(e.str);
        }
      }
    }
  }

  PdfObj tuStore;
  const PdfObj* tu = doc.resolve(fontDict.find("ToUnicode"), tuStore);
  if (tu && tu->kind == Kind::Stream) {
    ByteBuf data;
    if (doc.getStreamData(*tu, data, 1u << 20)) {
      parseToUnicode(data.data(), data.size());
      hasToUnicode = !cmap.empty();
    }
  }
  if (isCID && !hasToUnicode) {
    LOG_DBG("PDF", "Type0 font without usable ToUnicode: text will be lost");
  }
}

void PdfFont::addEntry(uint32_t lo, uint32_t hi, const std::string& dstBytes) {
  if (cmap.size() >= MAX_CMAP_ENTRIES || hi < lo) return;
  MapEntry e;
  e.lo = lo;
  e.hi = hi;
  e.dstOfs = (uint32_t)dstPool.size();
  if (dstBytes.size() >= 2) {
    for (size_t i = 0; i + 1 < dstBytes.size(); i += 2) {
      dstPool.push_back((uint16_t)(((uint8_t)dstBytes[i] << 8) | (uint8_t)dstBytes[i + 1]));
    }
  } else if (dstBytes.size() == 1) {
    dstPool.push_back((uint8_t)dstBytes[0]);
  }
  e.dstLen = (uint16_t)(dstPool.size() - e.dstOfs);
  if (e.dstLen == 0) return;
  cmap.push_back(e);
}

void PdfFont::parseToUnicode(const uint8_t* d, size_t len) {
  PdfLexer lx(d, len);
  bool sawCodeSpace = false;

  while (!lx.atEnd() && cmap.size() < MAX_CMAP_ENTRIES) {
    lx.skipWs();
    if (lx.atEnd()) break;
    const size_t before = lx.pos();
    const uint8_t c = lx.peek();
    if (c == '/' || c == '(' || c == '<' || c == '[' || c == '+' || c == '-' || c == '.' || (c >= '0' && c <= '9')) {
      PdfObj o;
      if (lx.parseObject(o)) continue;  // counts/dicts before begin* ops: discard
      lx.setPos(before + 1);            // resync
      continue;
    }
    char op[24];
    if (!lx.readOperator(op)) {
      lx.setPos(before + 1);
      continue;
    }

    if (strcmp(op, "begincodespacerange") == 0) {
      for (int i = 0; i < 256; i++) {
        lx.skipWs();
        if (lx.atEnd() || lx.keyword("endcodespacerange")) break;
        PdfObj lo, hi;
        if (!lx.parseObject(lo) || !lx.parseObject(hi)) break;
        if (lo.kind == Kind::String && !lo.str.empty()) {
          codeBytes = lo.str.size() >= 2 ? 2 : 1;
          sawCodeSpace = true;
        }
      }
    } else if (strcmp(op, "beginbfchar") == 0) {
      for (size_t i = 0; i < MAX_CMAP_ENTRIES; i++) {
        lx.skipWs();
        if (lx.atEnd() || lx.keyword("endbfchar")) break;
        PdfObj src, dst;
        if (!lx.parseObject(src) || !lx.parseObject(dst)) break;
        if (src.kind == Kind::String && dst.kind == Kind::String) {
          const uint32_t code = codeOf(src.str);
          addEntry(code, code, dst.str);
          if (!sawCodeSpace && src.str.size() >= 2) codeBytes = 2;
        }
      }
    } else if (strcmp(op, "beginbfrange") == 0) {
      for (size_t i = 0; i < MAX_CMAP_ENTRIES; i++) {
        lx.skipWs();
        if (lx.atEnd() || lx.keyword("endbfrange")) break;
        PdfObj lo, hi, dst;
        if (!lx.parseObject(lo) || !lx.parseObject(hi) || !lx.parseObject(dst)) break;
        if (lo.kind != Kind::String || hi.kind != Kind::String) break;
        const uint32_t cLo = codeOf(lo.str);
        const uint32_t cHi = codeOf(hi.str);
        if (!sawCodeSpace && lo.str.size() >= 2) codeBytes = 2;
        if (dst.kind == Kind::String) {
          addEntry(cLo, cHi, dst.str);
        } else if (dst.kind == Kind::Array) {
          for (size_t k = 0; k < dst.arr.size() && cLo + k <= cHi; k++) {
            if (dst.arr[k].kind == Kind::String) addEntry(cLo + (uint32_t)k, cLo + (uint32_t)k, dst.arr[k].str);
          }
        }
      }
    }
  }
}

bool PdfFont::mapCode(uint32_t code, std::string& out) const {
  // ponytail: linear scan; sort + binary search if CMap-heavy PDFs are slow
  for (const auto& e : cmap) {
    if (code < e.lo || code > e.hi) continue;
    uint16_t units[8];
    size_t k = e.dstLen > 8 ? 8 : e.dstLen;
    for (size_t i = 0; i < k; i++) units[i] = dstPool[e.dstOfs + i];
    units[k - 1] = (uint16_t)(units[k - 1] + (code - e.lo));
    pdfUtf16ToUtf8(units, k, out);
    return true;
  }
  return false;
}

bool PdfFont::decode(const uint8_t* s, size_t len, std::string& out) const {
  if (isCID && !hasToUnicode) return false;
  const size_t step = codeBytes;
  for (size_t i = 0; i + step <= len; i += step) {
    const uint32_t code = step == 2 ? (((uint32_t)s[i] << 8) | s[i + 1]) : s[i];
    if (hasToUnicode && mapCode(code, out)) continue;
    if (isCID) {
      pdfAppendUtf8(out, 0xFFFD);  // ToUnicode exists but misses this code
      continue;
    }
    const uint16_t u = simple[code & 0xFF];
    if (u >= 0x20)
      pdfAppendUtf8(out, u);
    else if (u == '\t' || u == '\n' || u == '\r')
      out += ' ';
  }
  return true;
}

// ---------------------------------------------------------------------------

const PdfFont* PdfFontCache::get(PdfDoc& doc, const PdfObj* fontRef) {
  if (!fontRef) return nullptr;
  const uint32_t num = fontRef->kind == PdfObj::Kind::Ref ? fontRef->ref : 0;
  if (num) {
    for (const auto& f : fonts) {
      if (f->objNum == num) return f.get();
    }
  }
  PdfObj store;
  const PdfObj* fd = doc.resolve(fontRef, store);
  if (!fd || !fd->isDict()) return nullptr;
  auto font = makeUniqueNoThrow<PdfFont>();
  if (!font) {
    LOG_ERR("PDF", "OOM: font");
    return nullptr;
  }
  font->objNum = num;
  font->build(doc, *fd);
  fonts.push_back(std::move(font));
  return fonts.back().get();
}

void PdfFontCache::trim() {
  if (fonts.size() > 32) fonts.clear();  // ponytail: wholesale flush between pages; LRU if it thrashes
}
