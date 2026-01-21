#pragma once

#include <cstdint>

/**
 * Thai Character Classification
 *
 * Thai Unicode block (U+0E00-U+0E7F) contains:
 * - Consonants (ก-ฮ): U+0E01-U+0E2E
 * - Vowels that can appear in various positions
 * - Tone marks that stack above consonants/vowels
 * - Thai digits and punctuation
 *
 * Thai text rendering requires special handling because:
 * 1. Leading vowels (เ แ โ ไ ใ) display BEFORE the consonant but
 *    appear AFTER in Unicode codepoint order
 * 2. Above vowels (ิ ี ึ ื etc.) must be positioned above consonants
 * 3. Below vowels (ุ ู) must be positioned below consonants
 * 4. Tone marks must stack above vowels/consonants
 */

namespace ThaiShaper {

// Thai character types for positioning and cluster formation
enum class ThaiCharType : uint8_t {
  NON_THAI,        // Not a Thai character
  CONSONANT,       // Base consonant (ก-ฮ)
  LEADING_VOWEL,   // Vowels that display before consonant (เ แ โ ไ ใ)
  ABOVE_VOWEL,     // Vowels above consonant (ั ิ ี ึ ื ็)
  BELOW_VOWEL,     // Vowels below consonant (ุ ู ฺ)
  FOLLOW_VOWEL,    // Vowels that follow consonant (ะ า ำ)
  TONE_MARK,       // Tone marks (่ ้ ๊ ๋)
  NIKHAHIT,        // Nikhahit (ํ) - special combining mark
  YAMAKKAN,        // Yamakkan (์) - cancellation mark
  THAI_DIGIT,      // Thai digits (๐-๙)
  THAI_SYMBOL,     // Thai punctuation and symbols
};

// Check if a codepoint is in the Thai Unicode block
inline bool isThaiCodepoint(uint32_t cp) { return cp >= 0x0E00 && cp <= 0x0E7F; }

// Get the character type for a Thai codepoint
ThaiCharType getThaiCharType(uint32_t cp);

// Check if codepoint is a Thai consonant (can be a cluster base)
inline bool isThaiConsonant(uint32_t cp) { return cp >= 0x0E01 && cp <= 0x0E2E; }

// Check if codepoint is a leading vowel (needs reordering)
inline bool isThaiLeadingVowel(uint32_t cp) {
  return cp == 0x0E40 ||  // SARA E (เ)
         cp == 0x0E41 ||  // SARA AE (แ)
         cp == 0x0E42 ||  // SARA O (โ)
         cp == 0x0E43 ||  // SARA AI MAIMUAN (ใ)
         cp == 0x0E44;    // SARA AI MAIMALAI (ไ)
}

// Check if codepoint is an above vowel/mark (positioned above base)
inline bool isThaiAboveVowel(uint32_t cp) {
  return cp == 0x0E31 ||  // MAI HAN-AKAT (ั)
         cp == 0x0E34 ||  // SARA I (ิ)
         cp == 0x0E35 ||  // SARA II (ี)
         cp == 0x0E36 ||  // SARA UE (ึ)
         cp == 0x0E37 ||  // SARA UEE (ื)
         cp == 0x0E47;    // MAITAIKHU (็)
}

// Check if codepoint is a below vowel (positioned below base)
inline bool isThaibelowVowel(uint32_t cp) {
  return cp == 0x0E38 ||  // SARA U (ุ)
         cp == 0x0E39 ||  // SARA UU (ู)
         cp == 0x0E3A;    // PHINTHU (ฺ)
}

// Check if codepoint is a tone mark (positioned above)
inline bool isThaiToneMark(uint32_t cp) {
  return cp == 0x0E48 ||  // MAI EK (่)
         cp == 0x0E49 ||  // MAI THO (้)
         cp == 0x0E4A ||  // MAI TRI (๊)
         cp == 0x0E4B;    // MAI CHATTAWA (๋)
}

// Check if codepoint is a Thai digit
inline bool isThaiDigit(uint32_t cp) { return cp >= 0x0E50 && cp <= 0x0E59; }

// Check if a codepoint is a combining character (needs to attach to base)
inline bool isThaiCombining(uint32_t cp) {
  return isThaiAboveVowel(cp) || isThaibelowVowel(cp) || isThaiToneMark(cp) || cp == 0x0E4C ||  // THANTHAKHAT (์)
         cp == 0x0E4D ||                                                                        // NIKHAHIT (ํ)
         cp == 0x0E4E;                                                                          // YAMAKKAN
}

// Check if text contains any Thai codepoints (for fast path detection)
bool containsThai(const char* text);

}  // namespace ThaiShaper
