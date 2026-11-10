#pragma once

#include <Epub/ReaderRenderSpec.h>

#include <cstdint>

// Requested values remain independent of font availability and size snapping.
struct ReaderTextSettings {
  char sdFontFamilyName[32] = "";
  uint8_t fontPointSize = 14;
  uint8_t lineSpacing = 2;
  uint8_t wordSpacing = 100;
  uint8_t characterSpacing = 2;
  uint8_t extraParagraphSpacing = 1;
  uint8_t paragraphIndentSpaces = 2;
  uint8_t paragraphAlignment = 0;
  uint8_t screenMargin = 5;
  uint8_t focusReadingEnabled = 0;
  uint8_t hyphenationEnabled = 0;
  uint8_t embeddedStyle = 1;
  uint8_t textAntiAliasing = 1;

  static ReaderTextSettings defaults();
  bool saveDefaults() const;
  bool isValid() const;
  int8_t getCharacterSpacing() const;
  float getReaderLineCompression() const;
  ReaderRenderSpec renderSpec(int fontId, uint16_t width, uint16_t height, uint8_t imageRendering) const;
  bool operator==(const ReaderTextSettings& other) const;
};
