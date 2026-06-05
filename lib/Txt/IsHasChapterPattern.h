#pragma once
#include <cstdint>

inline bool isHasChapterPattern(const char* s, int len) {
  if (!s || len < 6) return false;
  bool hasDi = false, hasZhang = false;
  for (int i = 0; i < len - 2; ++i) {
    const auto b0 = static_cast<unsigned char>(s[i]);
    const auto b1 = static_cast<unsigned char>(s[i+1]);
    const auto b2 = static_cast<unsigned char>(s[i+2]);
    if (b0 == 0xE7 && b1 == 0xAC && b2 == 0xAC) hasDi = true;
    if (b0 == 0xE7 && b1 == 0xAB && b2 == 0xA0) hasZhang = true;
    if (hasDi && hasZhang) return true;
  }
  return false;
}
