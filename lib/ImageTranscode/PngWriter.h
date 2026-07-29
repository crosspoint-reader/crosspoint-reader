#pragma once

// Minimal PNG encoder for on-device image transcoding.
//
// Why it exists: the reader only decodes JPEG and PNG, but books carry images
// in other formats (MOBI stores GIF; PDF stores raw Flate-compressed rasters).
// Re-encoding those as PNG is what makes them displayable. JPEG stays
// untouched wherever it appears — it is passed through byte-for-byte.
//
// The vendored miniz has its deflate APIs compiled out (MinizConfig.h), so the
// IDAT payload is a zlib stream of *stored* (uncompressed) deflate blocks. The
// result is a valid PNG that any decoder reads; it is simply not compressed,
// which costs SD space but no CPU — the right trade on this hardware, where a
// transcode happens once per image at conversion time.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace png {

enum class Color : uint8_t { Gray8, Rgb8, Palette8 };

struct Palette {
  const uint8_t* rgb = nullptr;  // 3 bytes per entry
  uint16_t count = 0;
  int16_t transparentIndex = -1;  // -1 = fully opaque
};

// Encodes `rows` (top-to-bottom, tightly packed, no filter bytes) into a PNG.
// Gray8/Palette8: 1 byte per pixel. Rgb8: 3 bytes per pixel.
// Returns false when the inputs are inconsistent or allocation fails.
bool encode(const uint8_t* pixels, uint32_t width, uint32_t height, Color color, const Palette& palette,
            std::vector<uint8_t>& out);

}  // namespace png
