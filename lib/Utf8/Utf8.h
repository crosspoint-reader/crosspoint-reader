#pragma once

#include <cstdint>
#include <string>
#define REPLACEMENT_GLYPH 0xFFFD

uint32_t utf8NextCodepoint(const unsigned char** string);
// Appends a Unicode codepoint to a std::string in UTF-8 encoding.
void utf8AppendCodepoint(uint32_t cp, std::string& out);
// Remove the last UTF-8 codepoint from a std::string and return the new size.
size_t utf8RemoveLastChar(std::string& str);
// Truncate string by removing N UTF-8 codepoints from the end.
void utf8TruncateChars(std::string& str, size_t numChars);

// Canonical composition (NFC) for the Latin / Vietnamese range and Hangul:
// precomposes a base letter followed by combining diacritical mark(s), and
// conjoining Hangul jamo sequences (L+V[+T]), into single codepoints. Needed
// because the device fonts have no combining-mark positioning and carry only
// precomposed Hangul syllables, so NFD text (some EPUB chapter titles; every
// filename written by macOS) otherwise renders broken or blank.
std::string utf8ComposeNfc(const std::string& in);

// Compose a null-terminated display buffer without allocating or growing it.
// Uncomposed bytes (including malformed UTF-8) are preserved unchanged.
void utf8ComposeNfcInPlace(char* buffer);

// The base letter a precomposed codepoint decomposes to, or 0 when there is
// none ("é" -> "e", but "ø" -> 0: it is a letter in its own right, not
// o-with-stroke). Lives here rather than in a caller because the compose table
// is a ~5 KB static array in the header: a second includer is a second copy in
// flash.
//
// A linear scan, unlike utf8ComposePair's binary search above it: the table is
// sorted by (base, mark), which this lookup searches against the grain. Adding
// a second table sorted by composed would cost more flash than sharing this one
// saves.
uint32_t utf8DecomposedBase(uint32_t cp);

// Truncate a raw char buffer to the last complete UTF-8 codepoint boundary.
// Returns the new length (<= len). If the buffer ends mid-sequence, the
// incomplete trailing bytes are excluded.
int utf8SafeTruncateBuffer(const char* buf, int len);

// Returns true for CJK characters that break without hyphenation. Covers CJK Unified Ideographs,
// Hiragana, Katakana, Hangul, CJK punctuation, and fullwidth forms. Han and Kana have implicit
// word boundaries per character; Hangul is space-delimited, so layout keeps Hangul words whole
// (see utf8IsHangul) and splits one without a hyphen only through the hyphenation path.
inline bool utf8IsCjkBreakable(const uint32_t cp) {
  return (cp >= 0x1100 && cp <= 0x11FF)        // Hangul Jamo
         || (cp >= 0x3000 && cp <= 0x303F)     // CJK Symbols and Punctuation
         || (cp >= 0x3040 && cp <= 0x309F)     // Hiragana
         || (cp >= 0x30A0 && cp <= 0x30FF)     // Katakana
         || (cp >= 0x3130 && cp <= 0x318F)     // Hangul Compatibility Jamo
         || (cp >= 0x3400 && cp <= 0x4DBF)     // CJK Extension A
         || (cp >= 0x4E00 && cp <= 0x9FFF)     // CJK Unified Ideographs
         || (cp >= 0xAC00 && cp <= 0xD7AF)     // Hangul Syllables
         || (cp >= 0xD7B0 && cp <= 0xD7FF)     // Hangul Jamo Extended-B
         || (cp >= 0xF900 && cp <= 0xFAFF)     // CJK Compatibility Ideographs
         || (cp >= 0xFE30 && cp <= 0xFE4F)     // CJK Compatibility Forms
         || (cp >= 0xFF01 && cp <= 0xFF60)     // Fullwidth Latin / Punctuation
         || (cp >= 0xFF65 && cp <= 0xFFEF)     // Halfwidth Katakana / Hangul
         || (cp >= 0x20000 && cp <= 0x2A6DF)   // CJK Extension B
         || (cp >= 0x2A700 && cp <= 0x2B73F);  // CJK Extension C
}

// Returns true for Hangul letters. Korean separates words with spaces, so a boundary touching
// Hangul is never an implicit line-break opportunity (CSS word-break: keep-all).
inline bool utf8IsHangul(const uint32_t cp) {
  return (cp >= 0x1100 && cp <= 0x11FF)      // Hangul Jamo
         || (cp >= 0x3130 && cp <= 0x318F)   // Hangul Compatibility Jamo
         || (cp >= 0xA960 && cp <= 0xA97F)   // Hangul Jamo Extended-A
         || (cp >= 0xAC00 && cp <= 0xD7FF)   // Hangul Syllables, Hangul Jamo Extended-B
         || (cp >= 0xFFA0 && cp <= 0xFFDC);  // Halfwidth Hangul
}

// Returns true for any codepoint in a CJK script block (Han, Kana, Hangul, Bopomofo,
// radicals, and CJK punctuation/compatibility/enclosed forms). Used for fallback font
// selection — deliberately broader than utf8IsCjkBreakable, whose ranges are tuned to
// implicit line-break opportunities and must not grow without rethinking layout.
inline bool utf8IsCjkCodepoint(const uint32_t cp) {
  return (cp >= 0x1100 && cp <= 0x11FF)        // Hangul Jamo
         || (cp >= 0x2E80 && cp <= 0x2FDF)     // CJK Radicals Supplement, Kangxi Radicals
         || (cp >= 0x3000 && cp <= 0x33FF)     // CJK punctuation, Kana, Bopomofo, Hangul Compat
                                               // Jamo, Kanbun, strokes, enclosed + compat forms
         || (cp >= 0x3400 && cp <= 0x4DBF)     // CJK Extension A
         || (cp >= 0x4E00 && cp <= 0x9FFF)     // CJK Unified Ideographs
         || (cp >= 0xA960 && cp <= 0xA97F)     // Hangul Jamo Extended-A
         || (cp >= 0xAC00 && cp <= 0xD7FF)     // Hangul Syllables, Hangul Jamo Extended-B
         || (cp >= 0xF900 && cp <= 0xFAFF)     // CJK Compatibility Ideographs
         || (cp >= 0xFE10 && cp <= 0xFE1F)     // Vertical Forms
         || (cp >= 0xFE30 && cp <= 0xFE4F)     // CJK Compatibility Forms
         || (cp >= 0xFF01 && cp <= 0xFF60)     // Fullwidth Latin / Punctuation
         || (cp >= 0xFF65 && cp <= 0xFFEF)     // Halfwidth Katakana / Hangul
         || (cp >= 0x20000 && cp <= 0x2EBEF)   // CJK Extensions B-F
         || (cp >= 0x2F800 && cp <= 0x2FA1F)   // CJK Compatibility Ideographs Supplement
         || (cp >= 0x30000 && cp <= 0x323AF);  // CJK Extensions G-H
}

// Writes cp as UTF-8 (1-4 bytes, no terminator) and returns the position after it.
inline char* utf8AppendCodepoint(char* output, const uint32_t codepoint) {
  if (codepoint < 0x80) {
    *output++ = static_cast<char>(codepoint);
  } else if (codepoint < 0x800) {
    *output++ = static_cast<char>(0xC0 | (codepoint >> 6));
    *output++ = static_cast<char>(0x80 | (codepoint & 0x3F));
  } else if (codepoint < 0x10000) {
    *output++ = static_cast<char>(0xE0 | (codepoint >> 12));
    *output++ = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
    *output++ = static_cast<char>(0x80 | (codepoint & 0x3F));
  } else {
    *output++ = static_cast<char>(0xF0 | (codepoint >> 18));
    *output++ = static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
    *output++ = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
    *output++ = static_cast<char>(0x80 | (codepoint & 0x3F));
  }
  return output;
}

// Uppercase partner of a lowercase Latin, Greek or Cyrillic letter; other codepoints are
// returned unchanged. Used to synthesize small caps, so letters without a single-codepoint
// uppercase form (e.g. U+00DF) stay as they are.
inline uint32_t utf8SmallCapsUpper(const uint32_t cp) {
  if (cp >= 'a' && cp <= 'z') return cp - 0x20;
  if (cp < 0xE0) return cp;
  if (cp <= 0xFE) return cp == 0xF7 ? cp : cp - 0x20;  // Latin-1 (skips the division sign)
  if (cp == 0xFF) return 0x178;
  if (cp <= 0x17F) {
    // Latin Extended-A pairs: lowercase on odd codepoints, except the two odd-aligned runs.
    const bool oddAligned = (cp >= 0x139 && cp <= 0x148) || (cp >= 0x179 && cp <= 0x17E);
    if (cp == 0x131 || cp == 0x138 || cp == 0x149 || cp == 0x17F) return cp;
    return (cp % 2 == (oddAligned ? 0u : 1u)) ? cp - 1 : cp;
  }
  if (cp >= 0x3B1 && cp <= 0x3C9) return cp == 0x3C2 ? 0x3A3 : cp - 0x20;  // Greek (final sigma -> Sigma)
  if (cp >= 0x430 && cp <= 0x44F) return cp - 0x20;                        // Cyrillic
  if (cp >= 0x450 && cp <= 0x45F) return cp - 0x50;
  return cp;
}

// Returns true for Unicode combining diacritical marks that should not advance the cursor.
inline bool utf8IsCombiningMark(const uint32_t cp) {
  return (cp >= 0x0300 && cp <= 0x036F)      // Combining Diacritical Marks
         || (cp >= 0x1DC0 && cp <= 0x1DFF)   // Combining Diacritical Marks Supplement
         || (cp >= 0x20D0 && cp <= 0x20FF)   // Combining Diacritical Marks for Symbols
         || (cp >= 0xFE20 && cp <= 0xFE2F);  // Combining Half Marks
}
