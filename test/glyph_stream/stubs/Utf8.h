#pragma once

#include <cstdint>

inline uint32_t utf8NextCodepoint(const unsigned char** text) {
  const unsigned char lead = **text;
  if (lead == 0) return 0;
  if (lead < 0x80) {
    ++*text;
    return lead;
  }

  uint8_t length = 0;
  uint32_t codepoint = 0;
  if ((lead & 0xE0) == 0xC0) {
    length = 2;
    codepoint = lead & 0x1F;
  } else if ((lead & 0xF0) == 0xE0) {
    length = 3;
    codepoint = lead & 0x0F;
  } else {
    length = 4;
    codepoint = lead & 0x07;
  }

  for (uint8_t i = 1; i < length; i++) {
    codepoint = (codepoint << 6) | ((*text)[i] & 0x3F);
  }
  *text += length;
  return codepoint;
}
