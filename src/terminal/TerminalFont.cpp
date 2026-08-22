#include "TerminalFont.h"

#include <GfxRenderer.h>

#if defined(KNIETTY_FONT_TERMINUS)
#include "TerminalFontData.terminus.generated.h"
#elif defined(KNIETTY_FONT_UNIFONT)
#include "TerminalFontData.unifont.generated.h"
#else
#include "TerminalFontData.generated.h"
#endif
#include "TerminalScreen.h"

namespace {

const TerminalFontData::Glyph* findGlyph(const uint16_t codepoint) {
  uint16_t first = 0;
  uint16_t count = TerminalFontData::GLYPH_COUNT;
  while (count > 0) {
    const uint16_t step = count / 2;
    const uint16_t index = first + step;
    if (TerminalFontData::GLYPHS[index].codepoint < codepoint) {
      first = index + 1;
      count -= step + 1;
    } else {
      count = step;
    }
  }
  if (first < TerminalFontData::GLYPH_COUNT && TerminalFontData::GLYPHS[first].codepoint == codepoint) {
    return &TerminalFontData::GLYPHS[first];
  }
  return nullptr;
}

}  // namespace

bool TerminalFont::hasGlyph(const uint16_t codepoint) { return findGlyph(codepoint) != nullptr; }

void TerminalFont::drawCell(const GfxRenderer& renderer, const int x, const int y, const int cellWidth,
                            const uint16_t codepoint, const uint8_t attributes, const bool cursor) {
  const bool inverse = (attributes & TerminalScreen::ATTR_INVERSE) != 0;
  renderer.fillRect(x, y, cellWidth, CELL_HEIGHT, inverse);

  const auto* glyph = findGlyph(codepoint);
  if (glyph == nullptr) glyph = findGlyph('?');

  const bool ink = !inverse;
  const int glyphInset = (cellWidth - GLYPH_WIDTH) / 2;
  for (int glyphY = 0; glyphY < GLYPH_HEIGHT; ++glyphY) {
    const uint8_t bits = glyph->rows[glyphY];
    for (int glyphX = 0; glyphX < GLYPH_WIDTH; ++glyphX) {
      if ((bits & (uint8_t{0x80} >> glyphX)) == 0) continue;
      const int pixelX = x + glyphInset + glyphX;
      const int pixelY = y + 1 + glyphY;
      renderer.drawPixel(pixelX, pixelY, ink);
      if ((attributes & TerminalScreen::ATTR_BOLD) != 0 && pixelX + 1 < x + cellWidth) {
        renderer.drawPixel(pixelX + 1, pixelY, ink);
      }
    }
  }

  if ((attributes & TerminalScreen::ATTR_UNDERLINE) != 0) {
    renderer.drawLine(x + glyphInset, y + CELL_HEIGHT - 2, x + cellWidth - 1, y + CELL_HEIGHT - 2, ink);
  }
  if (cursor) {
    // A block cursor inverts almost the complete 10x18 cell and repeatedly
    // re-drives the glyph beneath it. Keep the character readable and move only
    // eight pixels at the otherwise-unused bottom edge of the cell.
    renderer.drawLine(x + glyphInset, y + CELL_HEIGHT - 1, x + glyphInset + GLYPH_WIDTH - 1, y + CELL_HEIGHT - 1, ink);
  }
}
