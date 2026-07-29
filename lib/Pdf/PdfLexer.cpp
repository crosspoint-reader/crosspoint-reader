#include "PdfLexer.h"

#include <cstdlib>
#include <cstring>

namespace {

constexpr int MAX_DEPTH = 24;            // nested array/dict guard
constexpr size_t MAX_STRING = 1u << 20;  // 1MB string cap
constexpr size_t MAX_NAME = 512;
constexpr size_t MAX_ARRAY = 65536;
constexpr size_t MAX_DICT = 1024;

int hexVal(uint8_t c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

void PdfLexer::skipWs() {
  while (p < n) {
    const uint8_t c = b[p];
    if (isWs(c)) {
      p++;
      continue;
    }
    if (c == '%') {  // comment to end of line
      while (p < n && b[p] != '\n' && b[p] != '\r') p++;
      continue;
    }
    break;
  }
}

bool PdfLexer::keyword(const char* kw) {
  const size_t len = strlen(kw);
  if (p + len > n) {
    if (n > p && memcmp(b + p, kw, n - p) == 0) hitEnd = true;  // prefix cut off at window end
    return false;
  }
  if (memcmp(b + p, kw, len) != 0) return false;
  if (p + len < n) {
    const uint8_t next = b[p + len];
    if (isRegular(next)) return false;
  } else {
    hitEnd = true;  // keyword flush against window end: may continue in file
  }
  p += len;
  return true;
}

bool PdfLexer::readOperator(char out[24]) {
  skipWs();
  size_t k = 0;
  while (p < n && k < 23) {
    const uint8_t c = b[p];
    if (!isRegular(c)) break;
    out[k++] = (char)c;
    p++;
  }
  out[k] = 0;
  while (p < n && isRegular(b[p])) p++;  // drop overlong tail
  return k > 0;
}

bool PdfLexer::readUInt(uint32_t& out) {
  skipWs();
  if (p >= n) {
    hitEnd = true;
    return false;
  }
  if (b[p] < '0' || b[p] > '9') return false;
  uint64_t v = 0;
  while (p < n && b[p] >= '0' && b[p] <= '9') {
    v = v * 10 + (uint64_t)(b[p] - '0');
    if (v > 0xFFFFFFFFull) v = 0xFFFFFFFFull;
    p++;
  }
  if (p == n) hitEnd = true;  // digits may continue past window
  out = (uint32_t)v;
  return true;
}

bool PdfLexer::parseNumber(PdfObj& out) {
  char buf[32];
  size_t k = 0;
  bool real = false;
  bool anyDigit = false;
  if (p < n && (b[p] == '+' || b[p] == '-')) buf[k++] = (char)b[p++];
  while (p < n) {
    const uint8_t c = b[p];
    if (c >= '0' && c <= '9') {
      anyDigit = true;
    } else if (c == '.') {
      real = true;
    } else {
      break;
    }
    if (k < 31) buf[k++] = (char)c;
    p++;
  }
  if (!anyDigit && !real) return false;
  buf[k] = 0;
  out.kind = real ? PdfObj::Kind::Real : PdfObj::Kind::Int;
  out.num = strtod(buf, nullptr);
  if (p == n) hitEnd = true;
  return true;
}

bool PdfLexer::parseName(PdfObj& out) {
  p++;  // '/'
  out.kind = PdfObj::Kind::Name;
  out.str.clear();
  while (p < n) {
    const uint8_t c = b[p];
    if (!isRegular(c)) break;
    if (c == '#' && p + 2 < n && hexVal(b[p + 1]) >= 0 && hexVal(b[p + 2]) >= 0) {
      out.str += (char)((hexVal(b[p + 1]) << 4) | hexVal(b[p + 2]));
      p += 3;
    } else {
      out.str += (char)c;
      p++;
    }
    if (out.str.size() > MAX_NAME) return false;
  }
  if (p == n) hitEnd = true;
  return true;
}

bool PdfLexer::parseLiteralString(PdfObj& out) {
  p++;  // '('
  out.kind = PdfObj::Kind::String;
  out.str.clear();
  int depth = 1;
  while (p < n) {
    const uint8_t c = b[p++];
    if (c == '\\') {
      if (p >= n) break;
      const uint8_t e = b[p++];
      switch (e) {
        case 'n':
          out.str += '\n';
          break;
        case 'r':
          out.str += '\r';
          break;
        case 't':
          out.str += '\t';
          break;
        case 'b':
          out.str += '\b';
          break;
        case 'f':
          out.str += '\f';
          break;
        case '(':
          out.str += '(';
          break;
        case ')':
          out.str += ')';
          break;
        case '\\':
          out.str += '\\';
          break;
        case '\r':  // line continuation
          if (p < n && b[p] == '\n') p++;
          break;
        case '\n':
          break;
        default:
          if (e >= '0' && e <= '7') {  // \ddd octal, up to 3 digits
            uint32_t v = (uint32_t)(e - '0');
            for (int i = 0; i < 2 && p < n && b[p] >= '0' && b[p] <= '7'; i++) v = v * 8 + (uint32_t)(b[p++] - '0');
            out.str += (char)(v & 0xFF);
          } else {
            out.str += (char)e;  // unknown escape: char itself
          }
      }
    } else if (c == '(') {
      depth++;
      out.str += '(';
    } else if (c == ')') {
      if (--depth == 0) return true;
      out.str += ')';
    } else if (c == '\r') {  // EOL inside string records as \n
      out.str += '\n';
      if (p < n && b[p] == '\n') p++;
    } else {
      out.str += (char)c;
    }
    if (out.str.size() > MAX_STRING) return false;
  }
  hitEnd = true;
  return false;
}

bool PdfLexer::parseHexString(PdfObj& out) {
  p++;  // '<'
  out.kind = PdfObj::Kind::String;
  out.str.clear();
  int hi = -1;
  while (p < n) {
    const uint8_t c = b[p++];
    if (c == '>') {
      if (hi >= 0) out.str += (char)(hi << 4);  // odd count: pad with 0
      return true;
    }
    if (isWs(c)) continue;
    const int v = hexVal(c);
    if (v < 0) continue;  // lenient: skip junk
    if (hi < 0) {
      hi = v;
    } else {
      out.str += (char)((hi << 4) | v);
      hi = -1;
    }
    if (out.str.size() > MAX_STRING) return false;
  }
  hitEnd = true;
  return false;
}

bool PdfLexer::parseArray(PdfObj& out, int depth) {
  p++;  // '['
  out.kind = PdfObj::Kind::Array;
  out.arr.clear();
  while (true) {  // bounded: each pass consumes >= 1 byte or fails
    skipWs();
    if (p >= n) {
      hitEnd = true;
      return false;
    }
    if (b[p] == ']') {
      p++;
      return true;
    }
    PdfObj elem;
    if (!parseObject(elem, depth + 1)) return false;
    if (out.arr.size() >= MAX_ARRAY) return false;
    out.arr.push_back(std::move(elem));
  }
}

bool PdfLexer::parseDict(PdfObj& out, int depth) {
  p += 2;  // '<<'
  out.kind = PdfObj::Kind::Dict;
  out.dict.clear();
  while (true) {  // bounded: each pass consumes >= 1 byte or fails
    skipWs();
    if (p + 1 >= n) {
      hitEnd = true;
      return false;
    }
    if (b[p] == '>') {
      if (b[p + 1] != '>') return false;
      p += 2;
      return true;
    }
    if (b[p] != '/') return false;
    PdfObj key;
    if (!parseName(key)) return false;
    PdfObj val;
    if (!parseObject(val, depth + 1)) return false;
    if (out.dict.size() >= MAX_DICT) return false;
    out.dict.emplace_back(std::move(key.str), std::move(val));
  }
}

bool PdfLexer::parseObject(PdfObj& out, int depth) {
  if (depth > MAX_DEPTH) return false;
  skipWs();
  if (p >= n) {
    hitEnd = true;
    return false;
  }
  const uint8_t c = b[p];
  switch (c) {
    case '/':
      return parseName(out);
    case '(':
      return parseLiteralString(out);
    case '[':
      return parseArray(out, depth);
    case '<':
      if (p + 1 >= n) {
        hitEnd = true;
        return false;
      }
      return b[p + 1] == '<' ? parseDict(out, depth) : parseHexString(out);
    case 't':
      if (keyword("true")) {
        out.kind = PdfObj::Kind::Bool;
        out.boolVal = true;
        return true;
      }
      return false;
    case 'f':
      if (keyword("false")) {
        out.kind = PdfObj::Kind::Bool;
        out.boolVal = false;
        return true;
      }
      return false;
    case 'n':
      if (keyword("null")) {
        out.kind = PdfObj::Kind::Null;
        return true;
      }
      return false;
    default:
      break;
  }
  if (c == '+' || c == '-' || c == '.' || (c >= '0' && c <= '9')) {
    if (!parseNumber(out)) return false;
    // Indirect reference lookahead: <int> <int> R
    if (out.kind == PdfObj::Kind::Int && out.num >= 0 && out.num < 4294967295.0) {
      const size_t save = p;
      skipWs();
      if (p < n && b[p] >= '0' && b[p] <= '9') {
        uint64_t g = 0;
        while (p < n && b[p] >= '0' && b[p] <= '9') {
          g = g * 10 + (uint64_t)(b[p] - '0');
          if (g > 65535) g = 65535;
          p++;
        }
        skipWs();
        if (p < n && b[p] == 'R' && (p + 1 >= n || !isRegular(b[p + 1]))) {
          p++;
          out.ref = (uint32_t)out.num;
          out.gen = (uint16_t)g;
          out.num = 0;
          out.kind = PdfObj::Kind::Ref;
          return true;
        }
      }
      if (p >= n) hitEnd = true;  // ambiguous truncation at window end
      p = save;
    }
    return true;
  }
  return false;  // stray delimiter or junk byte
}
