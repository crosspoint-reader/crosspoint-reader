#pragma once
#include "IEpdFont.h"

class EpdFontFamily {
 public:
  enum Style : uint8_t { REGULAR = 0, BOLD = 1, ITALIC = 2, BOLD_ITALIC = 3, UNDERLINE = 4 };

  explicit EpdFontFamily(const IEpdFont* regular, const IEpdFont* bold = nullptr, const IEpdFont* italic = nullptr,
                         const IEpdFont* boldItalic = nullptr)
      : regular(regular), bold(bold), italic(italic), boldItalic(boldItalic) {}
  ~EpdFontFamily() = default;
  void getTextDimensions(const char* string, int* w, int* h, Style style = REGULAR) const;
  bool hasPrintableChars(const char* string, Style style = REGULAR) const;
  const EpdFontData* getData(Style style = REGULAR) const;
  const EpdGlyph* getGlyph(uint32_t cp, Style style = REGULAR) const;
  int getKerning(uint32_t leftCp, uint32_t rightCp, Style style = REGULAR) const;
  uint32_t applyLigatures(uint32_t cp, const char*& text, Style style = REGULAR) const;
  const IEpdFont* getFont(Style style) const;

 private:
  const IEpdFont* regular;
  const IEpdFont* bold;
  const IEpdFont* italic;
  const IEpdFont* boldItalic;
};
