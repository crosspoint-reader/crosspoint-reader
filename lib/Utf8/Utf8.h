#pragma once

#include <cstdint>
#include <string>
#define REPLACEMENT_GLYPH 0xFFFD

uint32_t utf8NextCodepoint(const unsigned char** string);
// Remove the last UTF-8 codepoint from a std::string and return the new size.
size_t utf8RemoveLastChar(std::string& str);
// Truncate string by removing N UTF-8 codepoints from the end.
void utf8TruncateChars(std::string& str, size_t numChars);

// Truncate a raw char buffer to the last complete UTF-8 codepoint boundary.
// Returns the new length (<= len). If the buffer ends mid-sequence, the
// incomplete trailing bytes are excluded.
int utf8SafeTruncateBuffer(const char* buf, int len);

// Returns true for Unicode combining diacritical marks that should not advance the cursor.
inline bool utf8IsCombiningMark(const uint32_t cp) {
  return (cp >= 0x0300 && cp <= 0x036F)      // Combining Diacritical Marks
         || (cp >= 0x1DC0 && cp <= 0x1DFF)   // Combining Diacritical Marks Supplement
         || (cp >= 0x20D0 && cp <= 0x20FF)   // Combining Diacritical Marks for Symbols
         || (cp >= 0xFE20 && cp <= 0xFE2F);  // Combining Half Marks
}

// Encode a Unicode codepoint to UTF-8. Writes 1-4 bytes to buf (must be >= 4 bytes).
// Returns the number of bytes written.
int utf8EncodeCodepoint(uint32_t cp, char* buf);

// Append a Unicode codepoint as UTF-8 to a string.
void utf8AppendCodepoint(std::string& str, uint32_t cp);

// Returns true if the string ends with '-' or soft-hyphen (U+00AD = 0xC2 0xAD in UTF-8).
inline bool utf8EndsWithHyphen(const char* str, size_t len) {
  if (len == 0) return false;
  if (str[len - 1] == '-') return true;
  return len >= 2 && static_cast<uint8_t>(str[len - 2]) == 0xC2 && static_cast<uint8_t>(str[len - 1]) == 0xAD;
}

// Remove trailing '-' or soft-hyphen (U+00AD) from string.
inline void utf8RemoveTrailingHyphen(std::string& str) {
  if (str.empty()) return;
  if (str.back() == '-') {
    str.pop_back();
  } else if (str.size() >= 2 && static_cast<uint8_t>(str[str.size() - 2]) == 0xC2 &&
             static_cast<uint8_t>(str[str.size() - 1]) == 0xAD) {
    str.erase(str.size() - 2);
  }
}
