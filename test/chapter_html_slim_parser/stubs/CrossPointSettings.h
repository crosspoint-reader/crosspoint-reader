#pragma once

#include <cstdint>

struct DictionarySettingsStub {
  bool extraParagraphSpacing = false;
  uint8_t paragraphAlignment = 0;
  bool hyphenationEnabled = false;
  bool focusReadingEnabled = false;
  uint8_t wordSpacing = 100;
  uint8_t paragraphIndentSpaces = 0;
  int getReaderFontId() const { return 0; }
  float getReaderLineCompression() const { return 1.0f; }
  int8_t getCharacterSpacing() const { return 0; }
};

inline DictionarySettingsStub SETTINGS;
