#pragma once

#include <cstdint>

class GfxRenderer;

namespace TerminalFont {

constexpr int GLYPH_WIDTH = 8;
constexpr int GLYPH_HEIGHT = 16;
constexpr int CELL_WIDTH = 10;
constexpr int CELL_HEIGHT = 18;

bool hasGlyph(uint16_t codepoint);
void drawCell(const GfxRenderer& renderer, int x, int y, int cellWidth, uint16_t codepoint, uint8_t attributes,
              bool cursor);

}  // namespace TerminalFont
