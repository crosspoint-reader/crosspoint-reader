#include "ScriptDetector.h"

#include <Utf8.h>

#include <algorithm>

namespace ScriptDetector {

bool startsWithRtl(const char* text, int maxLetters) {
  if (!text || maxLetters <= 0) return false;
  auto* p = reinterpret_cast<const unsigned char*>(text);
  int letterCount = 0;
  while (*p && letterCount < maxLetters) {
    uint32_t cp = utf8NextCodepoint(&p);
    if (cp == 0 || cp == REPLACEMENT_GLYPH) break;
    // Skip whitespace and combining marks
    if (cp <= 0x20) continue;
    if (utf8IsCombiningMark(cp)) continue;
    // Only count letter codepoints — digits and punctuation don't indicate direction
    if (!isLetterCodepoint(cp)) continue;
    letterCount++;
    return isRtlCodepoint(cp);
  }
  return false;
}

// Encode a single Unicode codepoint as UTF-8, appending to `out`.
static void utf8Encode(uint32_t cp, std::string& out) {
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

void reverseIfRtl(std::string& word) {
  if (word.empty()) return;

  // Stack buffer — words longer than 64 codepoints are not expected for our supported languages.
  uint32_t codepoints[64];
  size_t count = 0;
  bool hasRtl = false;

  auto* p = reinterpret_cast<const unsigned char*>(word.c_str());
  while (*p && count < 64) {
    uint32_t cp = utf8NextCodepoint(&p);
    if (cp == 0 || cp == REPLACEMENT_GLYPH) break;
    if (isRtlCodepoint(cp)) hasRtl = true;
    codepoints[count++] = cp;
  }

  if (!hasRtl || count <= 1) return;

  // Reverse the codepoints array
  std::reverse(codepoints, codepoints + count);

  // Mirror brackets after reversal (only check ASCII range 0x28-0x7D)
  for (size_t i = 0; i < count; i++) {
    const uint32_t cp = codepoints[i];
    if (cp >= 0x28 && cp <= 0x7D) {  // Fast range check covers all ASCII brackets
      switch (cp) {
        case '(':
          codepoints[i] = ')';
          break;
        case ')':
          codepoints[i] = '(';
          break;
        case '[':
          codepoints[i] = ']';
          break;
        case ']':
          codepoints[i] = '[';
          break;
        case '{':
          codepoints[i] = '}';
          break;
        case '}':
          codepoints[i] = '{';
          break;
        case '<':
          codepoints[i] = '>';
          break;
        case '>':
          codepoints[i] = '<';
          break;
      }
    }
  }

  // Re-encode to UTF-8
  std::string reversed;
  reversed.reserve(word.size());
  for (size_t i = 0; i < count; i++) {
    utf8Encode(codepoints[i], reversed);
  }
  word = std::move(reversed);
}

}  // namespace ScriptDetector
