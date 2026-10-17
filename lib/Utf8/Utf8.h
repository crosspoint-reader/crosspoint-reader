#pragma once

#include <cstdint>
#include <string>
#define REPLACEMENT_GLYPH 0xFFFD

uint32_t utf8NextCodepoint(const unsigned char** string);
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

// Apply lightweight NFC-like normalization for Vietnamese precomposed characters and for
// conjoining Hangul jamo (L+V -> LV, LV+T -> LVT).
// Converts NFD sequences (base vowel + combining marks) into NFC precomposed
// codepoints from the U+1EA0-U+1EF9 range. Safe no-op for already-NFC text.
// Handles both canonical NFD ordering and the "natural" order produced by
// macOS, Word, and older Vietnamese publishing tools.
std::string utf8NfcNorm(std::string s);
