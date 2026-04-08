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

// Returns true for any combining mark relevant to Vietnamese NFC composition,
// including U+031B (COMBINING HORN) which sits outside the standard range above.
inline bool utf8IsVietnameseCombining(const uint32_t cp) {
  return utf8IsCombiningMark(cp) || cp == 0x031B;
}

// Apply lightweight NFC-like normalization for Vietnamese precomposed characters.
// Converts NFD sequences (base vowel + combining marks) into NFC precomposed
// codepoints from the U+1EA0-U+1EF9 range. Safe no-op for already-NFC text.
// Handles both canonical NFD ordering and the "natural" order commonly produced
// by macOS, Microsoft Word, and older Vietnamese publishing tools.
std::string utf8NfcNorm(std::string s);