#include "Utf8.h"

int utf8CodepointLen(const unsigned char c) {
  if (c < 0x80) return 1;          // 0xxxxxxx
  if ((c >> 5) == 0x6) return 2;   // 110xxxxx
  if ((c >> 4) == 0xE) return 3;   // 1110xxxx
  if ((c >> 3) == 0x1E) return 4;  // 11110xxx
  return 1;                        // fallback for invalid
}

uint32_t utf8NextCodepoint(const unsigned char** string) {
  if (**string == 0) {
    return 0;
  }

  const int bytes = utf8CodepointLen(**string);
  const uint8_t* chr = *string;

  if (bytes == 1) {
    *string += 1;
    return chr[0];
  }

  for (int i = 1; i < bytes; ++i) {
    // Validate each continuation byte starts with expected 0b10 sequence.
    // This also handles unexpected null string terminator in the middle of
    // a UTF-8 multibyte sequence.
    if ((chr[i] & 0xC0) != 0x80) {
      *string += 1;
      return REPLACEMENT_GLYPH;
    }
  }

  *string += bytes;

  // Mask off the 0b110, 0b1110, or 0b11110 header bits from the first byte.
  uint32_t cp = chr[0] & ((1 << (7 - bytes)) - 1);

  // Copy continuation bytes, masking off the 0b10 continuation header bits.
  for (int i = 1; i < bytes; i++) {
    cp = (cp << 6) | (chr[i] & 0x3F);
  }

  return cp;
}

size_t utf8RemoveLastChar(std::string& str) {
  if (str.empty()) return 0;
  size_t pos = str.size() - 1;
  while (pos > 0 && (static_cast<unsigned char>(str[pos]) & 0xC0) == 0x80) {
    --pos;
  }
  str.resize(pos);
  return pos;
}

// Truncate string by removing N UTF-8 characters from the end
void utf8TruncateChars(std::string& str, const size_t numChars) {
  for (size_t i = 0; i < numChars && !str.empty(); ++i) {
    utf8RemoveLastChar(str);
  }
}
