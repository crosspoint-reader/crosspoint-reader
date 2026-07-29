#pragma once

#include <cstddef>
#include <cstdint>

#include "PdfObject.h"

// In-memory PDF object lexer/parser (ISO 32000-1 §7.2/§7.3). Operates on a
// contiguous buffer; PdfDoc reads windowed slices of the file and retries with
// a larger window when a parse runs off the end (ranOffEnd()). Also serves as
// the token source for content streams and ToUnicode CMaps (readOperator).
class PdfLexer {
 public:
  PdfLexer(const uint8_t* buf, size_t len) : b(buf), n(len) {}

  size_t pos() const { return p; }
  void setPos(size_t newPos) { p = newPos <= n ? newPos : n; }
  bool atEnd() const { return p >= n; }
  // True when the last failure was (or may have been) due to buffer
  // truncation rather than malformed input — caller should grow the window.
  bool ranOffEnd() const { return hitEnd; }
  uint8_t peek() const { return p < n ? b[p] : 0; }

  void skipWs();  // whitespace + %comments
  // Parses one object at the cursor. False on malformed input.
  bool parseObject(PdfObj& out, int depth = 0);
  // Consume `kw` if present at the cursor and followed by ws/delimiter/EOF.
  bool keyword(const char* kw);
  // Operator token: run of regular characters (max 23). False when the next
  // byte is a delimiter or the buffer is exhausted.
  bool readOperator(char out[24]);
  // Unsigned decimal integer (xref headers, objstm headers). Skips leading ws.
  bool readUInt(uint32_t& out);

  static bool isWs(uint8_t c) { return c == 0 || c == 9 || c == 10 || c == 12 || c == 13 || c == 32; }
  static bool isDelim(uint8_t c) {
    return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']' || c == '{' || c == '}' || c == '/' ||
           c == '%';
  }
  static bool isRegular(uint8_t c) { return !isWs(c) && !isDelim(c); }

 private:
  bool parseNumber(PdfObj& out);
  bool parseName(PdfObj& out);
  bool parseLiteralString(PdfObj& out);
  bool parseHexString(PdfObj& out);
  bool parseArray(PdfObj& out, int depth);
  bool parseDict(PdfObj& out, int depth);

  const uint8_t* b;
  size_t n;
  size_t p = 0;
  bool hitEnd = false;
};
