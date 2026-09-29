#pragma once

#include <cstdint>

// Shaped text travels through the renderer as ordinary UTF-8 made of
// private-use "tokens", so every draw, measure and prewarm path keeps working
// on codepoint strings:
//
//   * Glyph     U+F0000 + glyph ID   a glyph chosen by the shaper. Fonts that
//               shape store the bitmap for glyph ID g at this codepoint
//               (.cpfont) or rasterize g directly (TTF).
//   * Advance   U+FE000 + advance    the glyph that follows advances by this
//               12.4 fixed-point amount instead of its own advanceX.
//   * Offset    U+100000 + (dx, dy)  the glyph that follows is drawn this many
//               whole pixels right/down of the pen (int8 each); the pen itself
//               does not move.
//
// All three lie in the supplementary private-use planes, which the renderer
// never otherwise draws, so unshaped text cannot be mistaken for tokens.
// Mirrored by GLYPH_TOKEN_BASE in lib/EpdFont/scripts/shaping_blob.py.
namespace shaping {

constexpr uint32_t GLYPH_TOKEN_BASE = 0xF0000;
constexpr uint32_t GLYPH_TOKEN_MAX_GID = 0xDFFF;
constexpr uint32_t ADVANCE_TOKEN_BASE = 0xFE000;
constexpr uint32_t ADVANCE_TOKEN_MAX = 0x1FFD;
constexpr uint32_t OFFSET_TOKEN_BASE = 0x100000;

constexpr bool isGlyphToken(const uint32_t cp) {
  return cp >= GLYPH_TOKEN_BASE && cp <= GLYPH_TOKEN_BASE + GLYPH_TOKEN_MAX_GID;
}
constexpr uint32_t glyphTokenId(const uint32_t cp) { return cp - GLYPH_TOKEN_BASE; }
constexpr uint32_t glyphToken(const uint32_t gid) { return GLYPH_TOKEN_BASE + gid; }

constexpr bool isAdvanceToken(const uint32_t cp) {
  return cp >= ADVANCE_TOKEN_BASE && cp <= ADVANCE_TOKEN_BASE + ADVANCE_TOKEN_MAX;
}
constexpr uint16_t advanceTokenValue(const uint32_t cp) { return static_cast<uint16_t>(cp - ADVANCE_TOKEN_BASE); }
constexpr uint32_t advanceToken(const int32_t advance12_4) {
  if (advance12_4 < 0) return ADVANCE_TOKEN_BASE;
  const auto value = static_cast<uint32_t>(advance12_4);
  return ADVANCE_TOKEN_BASE + (value > ADVANCE_TOKEN_MAX ? ADVANCE_TOKEN_MAX : value);
}

constexpr bool isOffsetToken(const uint32_t cp) { return cp >= OFFSET_TOKEN_BASE && cp <= OFFSET_TOKEN_BASE + 0xFFFF; }
constexpr int offsetTokenDx(const uint32_t cp) { return static_cast<int8_t>((cp >> 8) & 0xFF); }
constexpr int offsetTokenDy(const uint32_t cp) { return static_cast<int8_t>(cp & 0xFF); }
constexpr uint32_t offsetToken(const int dx, const int dy) {
  const auto clamp8 = [](const int v) { return v < -128 ? -128 : v > 127 ? 127 : v; };
  return OFFSET_TOKEN_BASE | (static_cast<uint32_t>(static_cast<uint8_t>(clamp8(dx))) << 8) |
         static_cast<uint8_t>(clamp8(dy));
}

// Advance and offset tokens modify the next glyph; they have no glyph of their own.
constexpr bool isPositionToken(const uint32_t cp) { return isAdvanceToken(cp) || isOffsetToken(cp); }

}  // namespace shaping
