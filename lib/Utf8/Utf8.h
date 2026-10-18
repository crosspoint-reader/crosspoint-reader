#pragma once

#include <cstdint>
#include <string>
#define REPLACEMENT_GLYPH 0xFFFD

uint32_t utf8NextCodepoint(const unsigned char** string);
// Append `cp` encoded as UTF-8 (one to four bytes).
void utf8AppendCodepoint(std::string& out, uint32_t cp);
// Remove the last UTF-8 codepoint from a std::string and return the new size.
size_t utf8RemoveLastChar(std::string& str);
// Truncate string by removing N UTF-8 codepoints from the end.
void utf8TruncateChars(std::string& str, size_t numChars);

// Simple (single-codepoint) lowercase for the scripts the reader's fonts and languages cover:
// ASCII, Latin-1 Supplement, Latin Extended-A, the Vietnamese and Romanian letters of Latin
// Extended-B, Latin Extended Additional, monotonic Greek and Cyrillic. Anything else, and any
// letter whose lowercase is not one codepoint, comes back unchanged. One mapping for every
// caller that folds case: the hyphenators and the dictionary lookup.
uint32_t utf8LowerCodepoint(uint32_t cp);

// The inverse of utf8LowerCodepoint(): the capital of every small letter it produces, plus the
// lowercase-only ı, ſ and final ς (I, S, Σ). Same coverage, so a letter with no single-codepoint
// capital (ß, ŉ, ΐ) and anything outside the covered blocks come back unchanged. Derived from
// utf8LowerCodepoint() rather than kept as a second table, so the two cannot disagree. Never
// lengthens the UTF-8 encoding. Used by small caps to pick the glyph a lowercase letter draws with.
uint32_t utf8UpperCodepoint(uint32_t cp);

// utf8LowerCodepoint() over a single word. A capital sigma ending the word becomes the final
// form ς, the spelling Greek text and dictionaries use. Never longer than the input.
std::string utf8ToLower(const std::string& s);

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

// Returns true for Unicode Default_Ignorable_Code_Point (BMP): formatting controls that carry
// no text and must never occupy a cell, produce ink, or join a word.
//
// This matters far beyond tidiness. Books watermarked with zero-width steganography encode a
// per-copy fingerprint as long runs of U+200B/U+200C/U+200D between words; one measured EPUB
// carried 3,263,520 of them, 39% of its total chapter bytes, in runs of ~1400 with no
// intervening space. Kept in the text they are laid out as ordinary characters: they inflate
// the parse, they defeat line breaking (the longest resulting single unbreakable "word" in that
// book was 3,217 characters), and they draw whatever the font maps them to.
//
// U+00AD SOFT HYPHEN is deliberately excluded: it is Default_Ignorable, but this firmware breaks
// lines on it and must draw a hyphen when it does.
inline bool utf8IsDefaultIgnorable(const uint32_t cp) {
  // Ordered by how often each range is actually hit, cheapest common case first.
  if (cp < 0x034F) return false;
  return (cp >= 0x200B && cp <= 0x200F)      // ZWSP, ZWNJ, ZWJ, LRM, RLM
         || (cp >= 0x202A && cp <= 0x202E)   // bidi embedding / override
         || (cp >= 0x2060 && cp <= 0x2065)   // word joiner, invisible operators
         || (cp >= 0x206A && cp <= 0x206F)   // deprecated format characters
         || cp == 0xFEFF                     // zero-width no-break space / BOM
         || (cp >= 0xFE00 && cp <= 0xFE0F)   // variation selectors 1-16
         || cp == 0x034F                     // combining grapheme joiner
         || cp == 0x061C                     // Arabic letter mark
         || (cp >= 0x115F && cp <= 0x1160)   // Hangul choseong/jungseong fillers
         || (cp >= 0x17B4 && cp <= 0x17B5)   // Khmer inherent vowels
         || (cp >= 0x180B && cp <= 0x180F)   // Mongolian variation selectors + FVS
         || cp == 0x3164                     // Hangul filler
         || cp == 0xFFA0                     // halfwidth Hangul filler
         || (cp >= 0xFFF0 && cp <= 0xFFF8);  // unassigned, reserved ignorable
}

// Returns true for any combining mark relevant to Vietnamese NFC composition,
// including U+031B (COMBINING HORN) which falls outside the standard mark range.
inline bool utf8IsVietnameseCombining(const uint32_t cp) { return utf8IsCombiningMark(cp) || cp == 0x031B; }

// Conjoining Hangul jamo: modern leading consonants (L), medial vowels (V) and trailing
// consonants (T). These are not combining marks -- they are ordinary letters that COMPOSE
// with the preceding one -- so utf8IsVietnameseCombining does not and should not match them.
// Text stored in NFD (every filename macOS writes) carries Korean as these rather than as
// precomposed syllables, and the fonts only have the syllables.
inline bool utf8IsConjoiningJamo(const uint32_t cp) {
  return (cp >= 0x1100 && cp <= 0x1112)      // L: choseong
         || (cp >= 0x1161 && cp <= 0x1175)   // V: jungseong
         || (cp >= 0x11A8 && cp <= 0x11C2);  // T: jongseong
}

// Ported from crosspoint-reader (lib/Utf8/Utf8.h at 83f9cc05): utf8IsCjkBreakable from PR #2288
// ("feat: Support for Korean line breaks and glyph spacing", Justin Mitchell), utf8IsHangul from
// PR #3700 ("fix: justify Korean text by word spaces only", Sung-jin Brian Hong). utf8IsUnspacedCjk
// is ours.
//
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

// Han, kana, CJK punctuation and fullwidth forms: the CJK text written without spaces between
// words. A source line break between two of these is not a word gap (CSS Text 3 segment-break rule).
inline bool utf8IsUnspacedCjk(const uint32_t cp) { return utf8IsCjkBreakable(cp) && !utf8IsHangul(cp); }

// Apply lightweight NFC-like normalization for Vietnamese precomposed characters and for
// conjoining Hangul jamo (L+V -> LV, LV+T -> LVT).
// Converts NFD sequences (base vowel + combining marks) into NFC precomposed
// codepoints from the U+1EA0-U+1EF9 range. Safe no-op for already-NFC text.
// Handles both canonical NFD ordering and the "natural" order produced by
// macOS, Word, and older Vietnamese publishing tools.
std::string utf8NfcNorm(std::string s);
