#pragma once
#include "EpdFont.h"

class EpdFontFamily {
 public:
  static constexpr uint8_t DECORATION_MASK_SHIFT = 2;
  static constexpr uint8_t DECORATION_MASK = 0b111 << DECORATION_MASK_SHIFT;
  enum Style : uint8_t {
    // Font Style bitmask (bits 0-1)
    REGULAR = 0,
    BOLD = 1,
    ITALIC = 2,
    BOLD_ITALIC = 3,
    // Text Decoration bitmask (bits 2-4) - Maps to CssTextDecoration
    UNDERLINE = 1 << 2,
    LINETHROUGH = 1 << 3,
    OVERLINE = 1 << 4,
  };

  explicit EpdFontFamily(const EpdFont* regular, const EpdFont* bold = nullptr, const EpdFont* italic = nullptr,
                         const EpdFont* boldItalic = nullptr)
      : regular(regular), bold(bold), italic(italic), boldItalic(boldItalic) {}
  ~EpdFontFamily() = default;
  void getTextDimensions(const char* string, int* w, int* h, Style style = REGULAR) const;
  const EpdFontData* getData(Style style = REGULAR) const;
  const EpdGlyph* getGlyph(uint32_t cp, Style style = REGULAR) const;
  int8_t getKerning(uint32_t leftCp, uint32_t rightCp, Style style = REGULAR) const;
  uint32_t applyLigatures(uint32_t cp, const char*& text, Style style = REGULAR) const;
  static constexpr bool hasDecoration(const Style style) { return DECORATION_MASK & style; }

 private:
  const EpdFont* regular;
  const EpdFont* bold;
  const EpdFont* italic;
  const EpdFont* boldItalic;

  const EpdFont* getFont(Style style) const;
};
