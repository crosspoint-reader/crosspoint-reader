#pragma once
#include "IEpdFont.h"

class EpdFont : public IEpdFont {
  void getTextBounds(const char* string, int startX, int startY, int* minX, int* minY, int* maxX, int* maxY) const;
  uint32_t getLigature(uint32_t leftCp, uint32_t rightCp) const;

 public:
  const EpdFontData* data;
  explicit EpdFont(const EpdFontData* data) : data(data) {}
  virtual ~EpdFont() = default;

  void getTextDimensions(const char* string, int* w, int* h) const override;
  bool hasPrintableChars(const char* string) const override;
  const EpdGlyph* getGlyph(uint32_t cp) const override;
  int getKerning(uint32_t leftCp, uint32_t rightCp) const override;
  uint32_t applyLigatures(uint32_t cp, const char*& text) const override;
  const EpdFontData* getFontData() const override { return data; }
};
