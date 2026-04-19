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

  // Reverse grapheme clusters (base char + combining marks as a unit).
  // A naive codepoint-level reverse detaches Hebrew nikkud/cantillation
  // marks from their base letters (e.g. shin+qamats becomes qamats+shin).

  // Step 1: collect cluster ranges (start index, length).
  // Each cluster is a base codepoint followed by zero or more combining marks.
  struct Cluster {
    size_t start;
    size_t len;
  };
  Cluster clusters[64];
  size_t clusterCount = 0;
  size_t ci = 0;
  while (ci < count) {
    size_t start = ci;
    ci++;
    while (ci < count && utf8IsCombiningMark(codepoints[ci])) ci++;
    clusters[clusterCount++] = {start, ci - start};
  }

  // Step 2: build reversed output by iterating clusters in reverse order,
  // preserving codepoint order within each cluster (base + marks stay together).
  uint32_t reversed[64];
  size_t revIdx = 0;
  for (size_t c = clusterCount; c-- > 0;) {
    for (size_t j = 0; j < clusters[c].len; j++) {
      reversed[revIdx++] = codepoints[clusters[c].start + j];
    }
  }

  // Step 3: mirror brackets after reversal (only check ASCII range 0x28-0x7D)
  for (size_t idx = 0; idx < revIdx; idx++) {
    const uint32_t cp = reversed[idx];
    if (cp >= 0x28 && cp <= 0x7D) {
      switch (cp) {
        case '(':
          reversed[idx] = ')';
          break;
        case ')':
          reversed[idx] = '(';
          break;
        case '[':
          reversed[idx] = ']';
          break;
        case ']':
          reversed[idx] = '[';
          break;
        case '{':
          reversed[idx] = '}';
          break;
        case '}':
          reversed[idx] = '{';
          break;
        case '<':
          reversed[idx] = '>';
          break;
        case '>':
          reversed[idx] = '<';
          break;
      }
    }
  }

  // Re-encode to UTF-8
  std::string result;
  result.reserve(word.size());
  for (size_t idx = 0; idx < revIdx; idx++) {
    utf8Encode(reversed[idx], result);
  }
  word = std::move(result);
}

}  // namespace ScriptDetector
