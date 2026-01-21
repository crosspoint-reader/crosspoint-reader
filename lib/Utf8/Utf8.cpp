#include "Utf8.h"

// Debug logging for Thai rendering investigation
// Set to 1 to enable verbose UTF-8 decode logging
#define UTF8_DEBUG_LOGGING 0

#if UTF8_DEBUG_LOGGING
#include <Arduino.h>
#endif

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
  *string += bytes;

  if (bytes == 1) {
    return chr[0];
  }

  uint32_t cp = chr[0] & ((1 << (7 - bytes)) - 1);  // mask header bits

  for (int i = 1; i < bytes; i++) {
    // Validate continuation bytes: must be 10xxxxxx (0x80-0xBF)
    if ((chr[i] & 0xC0) != 0x80) {
#if UTF8_DEBUG_LOGGING
      Serial.printf("[UTF8] Invalid continuation byte at pos %d: 0x%02X (expected 0x80-0xBF), bytes=[", i, chr[i]);
      for (int j = 0; j < bytes; j++) {
        Serial.printf("0x%02X%s", chr[j], j < bytes - 1 ? " " : "");
      }
      Serial.printf("]\n");
#endif
      return REPLACEMENT_GLYPH;  // Return U+FFFD for invalid sequence
    }
    cp = (cp << 6) | (chr[i] & 0x3F);
  }

#if UTF8_DEBUG_LOGGING
  // Log Thai codepoints specifically
  if (cp >= 0x0E00 && cp <= 0x0E7F) {
    Serial.printf("[UTF8] Thai codepoint U+%04X from bytes=[", cp);
    for (int j = 0; j < bytes; j++) {
      Serial.printf("0x%02X%s", chr[j], j < bytes - 1 ? " " : "");
    }
    Serial.printf("]\n");
  }
#endif

  return cp;
}
