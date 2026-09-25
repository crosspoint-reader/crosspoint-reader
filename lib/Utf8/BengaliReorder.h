#pragma once

#include <string>

// Bengali syllable reordering for fonts rendered without OpenType shaping.
//
// The glyph pipeline draws codepoints left to right, so logical-order Bengali
// puts pre-base vowel signs after their consonant (কি renders as ক + ি instead
// of ি + ক). This rewrites each syllable into visual order:
//   * pre-base vowel signs (ি ে ৈ) move in front of the consonant cluster,
//   * two-part vowels split around it (ো -> ে + cluster + া, ৌ -> ে + cluster + ৗ),
//   * nukta sequences use the precomposed letters (ড় ঢ় য়).
// A syllable-initial reph (র্ before a consonant) stays in front of the
// cluster. Conjuncts are not formed; they still render with a visible hasant.
//
// Returns true and writes the reordered text to `out` when anything changed.
// Returns false, leaving `out` unspecified, when `text` needs no reordering
// (the common case: no Bengali block bytes, found by a byte scan).
bool bengaliReorderForDisplay(const char* text, std::string& out);
