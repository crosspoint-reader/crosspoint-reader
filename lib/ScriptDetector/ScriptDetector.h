#pragma once

#include <cstdint>
#include <string>

namespace ScriptDetector {

/// Check if a codepoint is a Hebrew consonant (U+05D0–U+05EA).
inline bool isHebrewCodepoint(uint32_t cp) { return cp >= 0x05D0 && cp <= 0x05EA; }

/// Check if a codepoint is RTL (Hebrew block for now; extend for Arabic later).
inline bool isRtlCodepoint(uint32_t cp) { return isHebrewCodepoint(cp); }

/// Check if a codepoint is a Unicode letter (Latin, Hebrew, or other alphabetic scripts).
/// Used to distinguish letters from digits/punctuation in RTL detection.
inline bool isLetterCodepoint(uint32_t cp) {
  if (cp >= 'A' && cp <= 'Z') return true;
  if (cp >= 'a' && cp <= 'z') return true;
  if (cp >= 0x00C0 && cp <= 0x024F) return true;  // Latin Extended
  if (cp >= 0x05D0 && cp <= 0x05EA) return true;  // Hebrew
  if (cp >= 0x0600 && cp <= 0x06FF) return true;  // Arabic (future-proof)
  return false;
}

/// Scan text for the first strong directional letter codepoint.
/// Skips whitespace, combining marks, digits, and punctuation — only letters count.
/// Returns true if the first letter found is RTL.
/// `maxLetters` limits how many letter codepoints to inspect before giving up.
bool startsWithRtl(const char* text, int maxLetters = 5);

/// Reverse the UTF-8 codepoints of a word in-place if it contains any RTL characters.
/// Leaves pure-LTR words (Latin, numbers, punctuation-only) untouched.
void reverseIfRtl(std::string& word);

}  // namespace ScriptDetector
