#pragma once
#include "EpdFontData.h"
#include "EpdFontStyles.h"

class EpdFont {
 protected:
  void getTextBounds(const char* string, int startX, int startY, int* minX, int* minY, int* maxX, int* maxY,
                     const EpdFontStyles::Style style = EpdFontStyles::REGULAR) const;

 public:
  const EpdFontData* data;
  explicit EpdFont(const EpdFontData* data) : data(data) {}
  virtual ~EpdFont() = default;

  void getTextDimensions(const char* string, int* w, int* h,
                         const EpdFontStyles::Style style = EpdFontStyles::REGULAR) const;
  int getTextAdvance(const char* string, const EpdFontStyles::Style style = EpdFontStyles::REGULAR) const;
  bool hasPrintableChars(const char* string, const EpdFontStyles::Style style = EpdFontStyles::REGULAR) const;

  virtual const EpdGlyph* getGlyph(uint32_t cp, const EpdFontStyles::Style style = EpdFontStyles::REGULAR) const;
  virtual const uint8_t* loadGlyphBitmap(const EpdGlyph* glyph, uint8_t* buffer,
                                         const EpdFontStyles::Style style = EpdFontStyles::REGULAR) const;

  virtual const EpdFontData* getData(const EpdFontStyles::Style style = EpdFontStyles::REGULAR) const { return data; }
};
