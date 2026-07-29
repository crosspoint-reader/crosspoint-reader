#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// Shared text helpers for the PDF converter.

inline void pdfAppendUtf8(std::string& out, uint32_t cp) {
  if (cp == 0 || cp > 0x10FFFF) cp = 0xFFFD;
  if (cp < 0x80) {
    out += (char)cp;
  } else if (cp < 0x800) {
    out += (char)(0xC0 | (cp >> 6));
    out += (char)(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += (char)(0xE0 | (cp >> 12));
    out += (char)(0x80 | ((cp >> 6) & 0x3F));
    out += (char)(0x80 | (cp & 0x3F));
  } else {
    out += (char)(0xF0 | (cp >> 18));
    out += (char)(0x80 | ((cp >> 12) & 0x3F));
    out += (char)(0x80 | ((cp >> 6) & 0x3F));
    out += (char)(0x80 | (cp & 0x3F));
  }
}

// UTF-16BE code units -> UTF-8 (handles surrogate pairs).
inline void pdfUtf16ToUtf8(const uint16_t* units, size_t n, std::string& out) {
  for (size_t i = 0; i < n; i++) {
    uint32_t cp = units[i];
    if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < n && units[i + 1] >= 0xDC00 && units[i + 1] <= 0xDFFF) {
      cp = 0x10000 + ((cp - 0xD800) << 10) + (units[i + 1] - 0xDC00);
      i++;
    } else if (cp >= 0xD800 && cp <= 0xDFFF) {
      cp = 0xFFFD;  // unpaired surrogate
    }
    if (cp < 0x20) continue;  // strip control characters
    pdfAppendUtf8(out, cp);
  }
}

// Append UTF-8 text with the XML specials escaped.
inline void pdfXmlEscapeAppend(std::string& out, const char* s, size_t len) {
  for (size_t i = 0; i < len; i++) {
    const char c = s[i];
    switch (c) {
      case '&':
        out += "&amp;";
        break;
      case '<':
        out += "&lt;";
        break;
      case '>':
        out += "&gt;";
        break;
      default:
        out += c;
    }
  }
}
