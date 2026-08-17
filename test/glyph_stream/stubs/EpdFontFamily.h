#pragma once

#include <EpdFontData.h>

class EpdFont {
 public:
  explicit EpdFont(const EpdFontData* fontData) : data(fontData) {}

  const EpdFontData* data;
};

class EpdFontFamily {
 public:
  enum Style : uint8_t { REGULAR = 0, BOLD = 1, ITALIC = 2, BOLD_ITALIC = 3 };

  explicit EpdFontFamily(const EpdFont* regular, const EpdFont* bold = nullptr, const EpdFont* italic = nullptr,
                         const EpdFont* boldItalic = nullptr)
      : regular(regular), bold(bold), italic(italic), boldItalic(boldItalic) {}

  const EpdFontData* getData(const Style style = REGULAR) const {
    const bool wantsBold = (style & BOLD) != 0;
    const bool wantsItalic = (style & ITALIC) != 0;
    if (wantsBold && wantsItalic) {
      if (boldItalic) return boldItalic->data;
      if (bold) return bold->data;
      if (italic) return italic->data;
    } else if (wantsBold && bold) {
      return bold->data;
    } else if (wantsItalic && italic) {
      return italic->data;
    }
    return regular->data;
  }

 private:
  const EpdFont* regular;
  const EpdFont* bold;
  const EpdFont* italic;
  const EpdFont* boldItalic;
};
