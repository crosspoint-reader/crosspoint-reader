#pragma once

#include <EpdFontFamily.h>

#include <string>

class GfxRenderer {
 public:
  int getSpaceWidth(const int, const EpdFontFamily::Style) const { return 5; }

  int getTextAdvanceX(const int, const char* text, const EpdFontFamily::Style) const {
    int width = 0;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p != 0; ++p) {
      if ((*p & 0xC0) == 0x80) {
        continue;
      }
      width += 10;
    }
    return width;
  }

  int getSpaceAdvance(const int, const uint32_t, const uint32_t, const EpdFontFamily::Style) const { return 5; }
  int getKerning(const int, const uint32_t, const uint32_t, const EpdFontFamily::Style) const { return 0; }
};