#pragma once

#include <cstdint>

namespace grayscaleGlyph {

// Two-bit fonts encode black coverage as: 0=black, 1=dark gray,
// 2=light gray, 3=white after GfxRenderer's normalization. Overlay MSB marks
// both edge shades. LSB selects the darker shade; white text reverses the
// shade order so coverage is inverted against a black base.
constexpr bool marksMsb(const uint8_t value) { return value == 1 || value == 2; }
constexpr bool marksLsb(const uint8_t value, const bool blackText) { return value == (blackText ? 1 : 2); }
constexpr bool isBwInk(const uint8_t value) { return value < 3; }

}  // namespace grayscaleGlyph
