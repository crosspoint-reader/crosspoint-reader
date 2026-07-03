#pragma once

#include <EpdFontFamily.h>
#include <GfxRenderer.h>

#include <cstdint>
#include <string_view>

namespace FocusReading {

struct SplitInfo {
  uint8_t boundaryBytes = 0;
  bool wholeBold = false;
};

bool isWordCharacter(uint32_t cp);
SplitInfo computeSplitInfo(std::string_view word);

struct Annotation {
  uint8_t boundary = 0;
  uint16_t suffixX = 0;
};

int getTextAdvanceX(const GfxRenderer& renderer, int fontId, const char* text, EpdFontFamily::Style style,
                    bool focusReadingEnabled, const Annotation* annotation = nullptr,
                    BidiUtils::BidiBaseDir baseDir = BidiUtils::BidiBaseDir::AUTO);

void drawText(const GfxRenderer& renderer, int fontId, int x, int y, const char* text, bool black,
              EpdFontFamily::Style style, bool focusReadingEnabled, const Annotation* annotation = nullptr,
              BidiUtils::BidiBaseDir baseDir = BidiUtils::BidiBaseDir::AUTO);

}  // namespace FocusReading
