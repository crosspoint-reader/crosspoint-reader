#pragma once

#include <cstdint>

// Compressor for 1-bit glyph bitmaps stored in the UI glyph pool (UiGlyphPool).
//
// Model: each pixel is predicted from three causal neighbors (W, N, NW) and the
// prediction error is entropy-coded with a binary range coder. The probability
// table is static (trained offline on CJK glyph corpora, see UiGlyphCodec.cpp),
// so encode/decode need no per-font state and no heap.
//
// Input bitmaps are the raw .cpfont form (2-bit greyscale or 1-bit); 2-bit
// sources are thresholded to 1-bit (any non-white level -> ink), which is
// pixel-exact for BW-mode rendering — the only mode UI text is drawn in.
// Output/decoded form is packed 1-bit, MSB-first, one continuous pixel stream
// (the layout GfxRenderer's 1-bit glyph branch consumes).
class UiGlyphCodec {
 public:
  // Packed 1-bit byte count for a glyph.
  static constexpr uint16_t packed1BitBytes(uint16_t pixelCount) { return static_cast<uint16_t>((pixelCount + 7) / 8); }

  // Read one source pixel (1 = ink) from a raw .cpfont bitmap.
  static bool sourcePixel(const uint8_t* src, bool srcIs2Bit, uint32_t pixelIndex);

  // Threshold-convert a raw source bitmap into packed 1-bit. dst must hold
  // packed1BitBytes(width * height) bytes.
  static void convertTo1Bit(const uint8_t* src, bool srcIs2Bit, uint8_t width, uint8_t height, uint8_t* dst);

  // Encode a glyph. Returns the encoded byte count, or 0 when the result would
  // not fit dstCapacity — the caller then stores the raw 1-bit form instead,
  // so pathological glyphs can never expand the pool.
  static uint16_t encode(const uint8_t* src, bool srcIs2Bit, uint8_t width, uint8_t height, uint8_t* dst,
                         uint16_t dstCapacity);

  // Decode an encoded glyph into packed 1-bit. dst must hold
  // packed1BitBytes(width * height) bytes. Reads past srcLen decode as zero
  // bytes (the encoder truncates trailing zeros).
  static void decode(const uint8_t* src, uint16_t srcLen, uint8_t width, uint8_t height, uint8_t* dst);
};
