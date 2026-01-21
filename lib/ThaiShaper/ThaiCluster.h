#pragma once

#include <cstdint>
#include <vector>

namespace ThaiShaper {

/**
 * Represents a positioned glyph within a Thai cluster.
 *
 * Thai clusters consist of a base consonant with optional:
 * - Leading vowel (displayed before base but stored after in Unicode)
 * - Above vowel/marks (stacked above base)
 * - Below vowel (positioned below base)
 * - Tone mark (stacked above vowels)
 * - Follow vowel (displayed after base)
 */
struct PositionedGlyph {
  uint32_t codepoint;  // Unicode codepoint to render
  int8_t xOffset;      // X offset from cluster origin (in font units fraction)
  int8_t yOffset;      // Y offset from baseline (negative = up, positive = down)
  bool zeroAdvance;    // If true, this glyph doesn't advance the cursor
};

/**
 * A Thai grapheme cluster - the minimal unit for proper rendering.
 *
 * A cluster typically contains:
 * - 0-1 leading vowel (displayed first)
 * - 1 base consonant
 * - 0-1 above vowel
 * - 0-1 below vowel
 * - 0-1 tone mark
 * - 0-1 follow vowel
 * - 0-1 thanthakhat/yamakkan
 */
struct ThaiCluster {
  std::vector<PositionedGlyph> glyphs;
  int totalAdvance;  // Total width of cluster in font advanceX units

  ThaiCluster() : totalAdvance(0) {}
};

/**
 * Y-offset constants for Thai mark positioning.
 * These are relative adjustments based on typical Thai font metrics.
 * Values are in "font units" where the typical em-height is ~1.0
 * Negative values move UP, positive move DOWN.
 */
namespace ThaiOffset {
// Above marks (relative to baseline, scaled by font size)
constexpr int8_t ABOVE_VOWEL = -2;  // Base above-vowel position
constexpr int8_t TONE_MARK = -4;    // Tone mark above vowel
constexpr int8_t TONE_MARK_ALONE =
    -2;  // Tone mark when no above vowel (sits lower at same position as above vowel would)

// Below marks
constexpr int8_t BELOW_VOWEL = 3;  // Below-vowel position

// X-offset for ascender consonants (tall consonants like ป ฝ ฟ)
// When these have above marks, the marks may need to shift
constexpr int8_t ASCENDER_X_SHIFT = 0;  // No shift needed in most fonts
}  // namespace ThaiOffset

}  // namespace ThaiShaper
