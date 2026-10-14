#pragma once

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "EpdFontData.h"

namespace ttf_test {

/// A bundled font source, relative to lib/EpdFont/builtinFonts/source.
/// Exactly the file's bytes, with no spare capacity, so AddressSanitizer sees a read one byte past the end.
inline std::vector<uint8_t> readFont(const std::string& relative) {
  std::ifstream in(std::string(TTF_FONT_DIR) + "/" + relative, std::ios::binary | std::ios::ate);
  if (!in) return {};
  std::vector<uint8_t> data(static_cast<size_t>(in.tellg()));
  in.seekg(0);
  in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
  return data;
}

/// Every TrueType source the reader bundles (the OpenDyslexic sources are CFF).
inline const std::vector<std::string>& bundledTtfs() {
  static const std::vector<std::string> fonts = {
      "Bookerly/Bookerly-Regular.ttf",    "Bookerly/Bookerly-Bold.ttf", "Bookerly/Bookerly-Italic.ttf",
      "Bookerly/Bookerly-BoldItalic.ttf", "Inter/Inter-Regular.ttf",    "Inter/Inter-Bold.ttf",
      "NotoSans/NotoSans-Regular.ttf",    "NotoSans/NotoSans-Bold.ttf", "NotoSans/NotoSans-Italic.ttf",
      "NotoSans/NotoSans-BoldItalic.ttf", "Ubuntu/Ubuntu-Regular.ttf",  "Ubuntu/Ubuntu-Bold.ttf",
  };
  return fonts;
}

/// A 2-bit bitmap as text, top row first: '.' for 0, then '1', '2', '3'.
inline std::string picture(const EpdGlyph& glyph, const uint8_t* bits) {
  std::string out;
  for (int y = 0; y < glyph.height; ++y) {
    for (int x = 0; x < glyph.width; ++x) {
      const int p = y * glyph.width + x;
      out += ".123"[(bits[p >> 2] >> (6 - 2 * (p & 3))) & 3];
    }
    out += '\n';
  }
  return out;
}

}  // namespace ttf_test
