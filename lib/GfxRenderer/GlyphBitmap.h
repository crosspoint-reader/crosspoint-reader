#pragma once

#include <algorithm>
#include <cstdint>

// Glyph rasterizer that resolves orientation, clipping and framebuffer
// addressing once per glyph instead of once per pixel. It has no dependency
// on GfxRenderer so it can be unit-tested on the host.
namespace glyphBitmap {

// Where a glyph lands in physical framebuffer space.
struct Target {
  uint8_t* buffer;  // First byte of row originY (framebuffer or strip scratch)
  int width;        // Physical panel width in pixels
  int stride;       // Bytes per physical row
  int originY;      // First physical row held by buffer
  int rows;         // Number of physical rows held by buffer
  int x;            // Physical position of glyph pixel (0, 0)
  int y;
  int dxX;  // Physical delta for one step along the glyph's x axis
  int dxY;
  int dyX;  // Physical delta for one step along the glyph's y axis
  int dyY;
};

// Half-open rectangle in glyph-local pixel coordinates.
struct Clip {
  int left;
  int top;
  int right;
  int bottom;
};

// Narrow [start, end) on one glyph axis so that base + i * step stays within
// [lower, upper). step is +1 or -1 because the transform is orthogonal.
inline void clipAxis(int base, int step, int lower, int upper, int& start, int& end) {
  if (step > 0) {
    start = std::max(start, lower - base);
    end = std::min(end, upper - base);
  } else {
    start = std::max(start, base - upper + 1);
    end = std::min(end, base - lower + 1);
  }
}

// Keep pixel writes inline when decoding a group of four pixels.
__attribute__((always_inline)) inline void paint(uint8_t* buffer, int destination, uint8_t ink, uint8_t levels,
                                                 bool clearBits) {
  if ((levels & (1u << ink)) == 0) return;
  const uint8_t mask = 0x80u >> (destination & 7);
  if (clearBits)
    buffer[destination >> 3] &= static_cast<uint8_t>(~mask);
  else
    buffer[destination >> 3] |= mask;
}

// Paint the selected ink values of a packed glyph into target.
//
// bitmap: rows are contiguous, MSB first, 1 or 2 bits per pixel; widths need
//   not be byte-aligned.
// levels: bitmask of source ink values to paint. Bit n set means ink value n
//   is painted (for 2bpp, 0 is transparent and 3 is black).
// clearBits: painted pixels clear their framebuffer bit (black in the BW
//   plane) when true, otherwise set it (white in BW, "update" in gray planes).
// Target axes must describe an orthogonal unit rotation. Clipping happens
// before pixel decoding, so fully hidden glyphs cost nothing.
inline void draw(const uint8_t* bitmap, int width, int height, bool twoBit, uint8_t levels, bool clearBits,
                 const Target& target, Clip clip) {
  // Intersect with the glyph bounds, then with the panel width and the
  // buffered row band. Which glyph axis maps to physical x depends on the
  // rotation: dxX != 0 means glyph x runs along physical x.
  clip.left = std::max(clip.left, 0);
  clip.top = std::max(clip.top, 0);
  clip.right = std::min(clip.right, width);
  clip.bottom = std::min(clip.bottom, height);
  if (target.dxX != 0) {
    clipAxis(target.x, target.dxX, 0, target.width, clip.left, clip.right);
    clipAxis(target.y, target.dyY, target.originY, target.originY + target.rows, clip.top, clip.bottom);
  } else {
    clipAxis(target.x, target.dyX, 0, target.width, clip.top, clip.bottom);
    clipAxis(target.y, target.dxY, target.originY, target.originY + target.rows, clip.left, clip.right);
  }
  if (clip.left >= clip.right || clip.top >= clip.bottom) return;

  // Walk the framebuffer as a flat bit index. One glyph column advances
  // stepX bits and one glyph row advances stepY bits; each is +/-1 for the
  // axis that maps to physical x, or +/-strideBits for physical y.
  const int strideBits = target.stride * 8;
  const int stepX = target.dxY * strideBits + target.dxX;
  const int stepY = target.dyY * strideBits + target.dyX;
  int rowBit = (target.y - target.originY) * strideBits + target.x + clip.left * stepX + clip.top * stepY;
  for (int y = clip.top; y < clip.bottom; ++y, rowBit += stepY) {
    int source = y * width + clip.left;
    int destination = rowBit;
    int remaining = clip.right - clip.left;
    if (twoBit) {
      // Rows are bit-contiguous, not byte-padded. Align after any clipped prefix.
      while (remaining && (source & 3)) {
        paint(target.buffer, destination, (bitmap[source >> 2] >> (6 - (source & 3) * 2)) & 3, levels, clearBits);
        ++source;
        destination += stepX;
        --remaining;
      }
      while (remaining >= 4) {
        const uint8_t packed = bitmap[source >> 2];
        paint(target.buffer, destination, packed >> 6, levels, clearBits);
        paint(target.buffer, destination + stepX, (packed >> 4) & 3, levels, clearBits);
        paint(target.buffer, destination + 2 * stepX, (packed >> 2) & 3, levels, clearBits);
        paint(target.buffer, destination + 3 * stepX, packed & 3, levels, clearBits);
        source += 4;
        destination += 4 * stepX;
        remaining -= 4;
      }
    }
    // One-bit glyphs and the clipped tail use scalar decoding.
    while (remaining--) {
      const uint8_t ink = twoBit ? ((bitmap[source >> 2] >> (6 - (source & 3) * 2)) & 3)
                                 : ((bitmap[source >> 3] >> (7 - (source & 7))) & 1);
      paint(target.buffer, destination, ink, levels, clearBits);
      ++source;
      destination += stepX;
    }
  }
}

}  // namespace glyphBitmap
