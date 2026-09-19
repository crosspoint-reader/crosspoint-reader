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

// Simple (single-codepoint) case fold: returns a canonical case-insensitive
// form of cp (per Unicode's case-folding data, not necessarily its display
// lowercase — e.g. Greek final sigma U+03C2 folds to U+03C3), or cp unchanged
// if it has no case or its fold isn't a single codepoint. Table-driven from
// Unicode data rather than per-script logic, so it covers any script with a
// simple case pairing (Latin, Greek, Cyrillic, Armenian, Cherokee, Deseret,
// ...) without the caller special-casing a language. For comparison keys
// only — not for display text.
uint32_t utf8SimpleCaseFold(uint32_t cp);

// Case-insensitive strcmp over UTF-8 strings: decodes both sides codepoint by
// codepoint and compares their utf8SimpleCaseFold() values, so two codepoints
// compare equal only when they share a simple, single-codepoint case fold —
// per utf8SimpleCaseFold's own caveats, a codepoint with no case or whose fold
// isn't a single codepoint compares by its own value instead. Use this (not
// StringUtils::asciiCaseCmp, which only folds ASCII) wherever a stored
// on-disk key may contain non-ASCII text and needs case-insensitive
// comparison — e.g. dictionary index lookups.
int utf8CaseInsensitiveCmp(const char* a, const char* b);

// True if cp is a Letter, Number, or Mark (Unicode general category L*, N*,
// or M*) — i.e. part of a word rather than incidental punctuation/symbols at
// its edges. Table-driven from Unicode category data, so it works for any
// script without a hand-picked list of punctuation ranges to keep up to date.
bool utf8IsWordChar(uint32_t cp);

// Canonical composition (NFC) for the Latin / Vietnamese range and Hangul:
// precomposes a base letter followed by combining diacritical mark(s), and
// conjoining Hangul jamo sequences (L+V[+T]), into single codepoints. Needed
// because the device fonts have no combining-mark positioning and carry only
// precomposed Hangul syllables, so NFD text (some EPUB chapter titles; every
// filename written by macOS) otherwise renders broken or blank.
std::string utf8ComposeNfc(const std::string& in);

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

// Returns true for CJK characters that allow line breaks on either side without hyphenation.
// Covers CJK Unified Ideographs, Hiragana, Katakana, Hangul Syllables, CJK punctuation,
// and fullwidth forms — the ranges where word boundaries are implicit per character.
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

// Returns true for Unicode combining diacritical marks that should not advance the cursor.
inline bool utf8IsCombiningMark(const uint32_t cp) {
  return (cp >= 0x0300 && cp <= 0x036F)      // Combining Diacritical Marks
         || (cp >= 0x1DC0 && cp <= 0x1DFF)   // Combining Diacritical Marks Supplement
         || (cp >= 0x20D0 && cp <= 0x20FF)   // Combining Diacritical Marks for Symbols
         || (cp >= 0xFE20 && cp <= 0xFE2F);  // Combining Half Marks
}
