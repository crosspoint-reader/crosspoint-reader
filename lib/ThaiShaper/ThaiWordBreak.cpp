#include "ThaiWordBreak.h"

#include <Utf8.h>

#include "ThaiCharacter.h"

namespace ThaiShaper {

/**
 * Get the byte offset of the next cluster boundary.
 *
 * A Thai grapheme cluster is the minimal breakable unit for line wrapping:
 *   [leading vowel] + consonant + [above/below vowels] + [tone marks] + [follow vowels]
 *
 * For non-Thai codepoints, returns the boundary after a single codepoint.
 *
 * @param text         UTF-8 encoded text (null-terminated)
 * @param startOffset  Starting byte offset into text
 * @return Byte offset of next boundary, or startOffset if at end / null
 */
size_t nextClusterBoundary(const char* text, size_t startOffset) {
  if (text == nullptr) {
    return 0;
  }

  const uint8_t* ptr = reinterpret_cast<const uint8_t*>(text + startOffset);

  if (*ptr == '\0') {
    return startOffset;
  }

  // Get first codepoint
  uint32_t cp = utf8NextCodepoint(&ptr);

  // Non-Thai: just return next codepoint boundary
  if (!isThaiCodepoint(cp)) {
    return reinterpret_cast<const char*>(ptr) - text;
  }

  // For Thai leading vowels (เ แ โ ไ ใ), include the following consonant and marks
  ThaiCharType type = getThaiCharType(cp);
  if (type == ThaiCharType::LEADING_VOWEL) {
    // Consume the leading vowel, continue to get consonant + marks
    if (*ptr != '\0') {
      cp = utf8NextCodepoint(&ptr);
    }
  }

  // Now consume any combining marks that follow the base
  while (*ptr != '\0') {
    const uint8_t* peekPtr = ptr;
    cp = utf8NextCodepoint(&peekPtr);

    if (!isThaiCodepoint(cp)) {
      break;
    }

    type = getThaiCharType(cp);

    // These types combine with the base, continue consuming
    if (type == ThaiCharType::ABOVE_VOWEL || type == ThaiCharType::BELOW_VOWEL || type == ThaiCharType::TONE_MARK ||
        type == ThaiCharType::NIKHAHIT || type == ThaiCharType::YAMAKKAN || type == ThaiCharType::FOLLOW_VOWEL) {
      ptr = peekPtr;
    } else {
      // New cluster starts (consonant, leading vowel, digit, etc.)
      break;
    }
  }

  return reinterpret_cast<const char*>(ptr) - text;
}

}  // namespace ThaiShaper
