#pragma once
#include "EpdFont.h"

class EpdFontFamily {
 public:
  enum Style : uint8_t { REGULAR = 0, BOLD = 1, ITALIC = 2, BOLD_ITALIC = 3 };

  explicit EpdFontFamily(const EpdFont* regular, const EpdFont* bold = nullptr, const EpdFont* italic = nullptr,
                         const EpdFont* boldItalic = nullptr)
      : regular(regular), bold(bold), italic(italic), boldItalic(boldItalic) {}
  ~EpdFontFamily() = default;
  void getTextDimensions(const char* string, int* w, int* h, Style style = REGULAR) const;
  bool hasPrintableChars(const char* string, Style style = REGULAR) const;
  const EpdFontData* getData(Style style = REGULAR) const;
  const EpdGlyph* getGlyph(uint32_t cp, Style style = REGULAR) const;

  // Check if bold variant is available (for synthetic bold decision)
  bool hasBold() const { return bold != nullptr; }

 private:
  const EpdFont* regular;
  const EpdFont* bold;
  const EpdFont* italic;
  const EpdFont* boldItalic;

  const EpdFont* getFont(Style style) const;
};

// Global typedef for use outside class scope (needed by SdFontFamily and GfxRenderer)
using EpdFontStyle = EpdFontFamily::Style;
constexpr EpdFontStyle REGULAR = EpdFontFamily::REGULAR;
constexpr EpdFontStyle BOLD = EpdFontFamily::BOLD;
constexpr EpdFontStyle ITALIC = EpdFontFamily::ITALIC;
constexpr EpdFontStyle BOLD_ITALIC = EpdFontFamily::BOLD_ITALIC;
