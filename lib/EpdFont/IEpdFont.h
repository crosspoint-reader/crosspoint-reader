#pragma once
#include <cstdint>

#include "EpdFontData.h"

class IEpdFont {
 public:
  virtual ~IEpdFont() = default;

  virtual void getTextDimensions(const char* string, int* w, int* h) const = 0;
  virtual bool hasPrintableChars(const char* string) const = 0;
  virtual const EpdGlyph* getGlyph(uint32_t cp) const = 0;
  virtual const EpdFontData* getFontData() const = 0;
  // Optional kerning and ligature hooks. Fonts without shaping data can use defaults.
  virtual int getKerning(uint32_t leftCp, uint32_t rightCp) const {
    (void)leftCp;
    (void)rightCp;
    return 0;
  }
  virtual uint32_t applyLigatures(uint32_t cp, const char*& text) const {
    (void)text;
    return cp;
  }
};
