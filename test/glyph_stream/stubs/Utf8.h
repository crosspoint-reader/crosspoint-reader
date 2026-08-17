#pragma once

#include <cstdint>

inline uint32_t utf8NextCodepoint(const unsigned char** text) {
  const uint32_t value = **text;
  if (value != 0) ++*text;
  return value;
}
