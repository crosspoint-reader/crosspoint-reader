#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

// Glyph rasterizer that resolves orientation, clipping and framebuffer
// addressing once per glyph instead of once per pixel. It has no dependency
// on GfxRenderer so it can be unit-tested on the host.
namespace glyphBitmap {

// Framebuffer plane a glyph is painted into.
enum class Plane : uint8_t { BW, GrayLSB, GrayMSB };

// 16-bit ink level masks for 4bpp glyphs (bit n = 4bpp ink level n, 0=white .. 15=black)
// on single-pass 4-gray and monochrome planes:
//   - BW paints levels 2..15 and suppresses faint level-1 outer halo (0xFFFC).
//   - GrayLSB paints dark gray levels 7..11 (0x0F80).
//   - GrayMSB paints light + dark gray levels 2..11, leaving 12..15 black (0x0FFC).
constexpr uint16_t kBw4BitMask = 0xFFFCu;
constexpr uint16_t kGrayLsb4BitMask = 0x0F80u;
constexpr uint16_t kGrayMsb4BitMask = 0x0FFCu;

// Position of glyph pixel (0, 0) plus the step for one move along the glyph's
// x and y axes, all in one coordinate space. The axes must describe an
// orthogonal unit rotation: each is +/-1 on exactly one component.
struct Frame {
  int x;
  int y;
  int dxX;
  int dxY;
  int dyX;
  int dyY;
};

// Where a glyph lands in physical framebuffer space.
struct Target {
  uint8_t* buffer;  // First byte of row originY (framebuffer or strip scratch)
  int width;        // Physical panel width in pixels
  int stride;       // Bytes per physical row
  int originY;      // First physical row held by buffer
  int rows;         // Number of physical rows held by buffer
  Frame frame;      // Physical placement of the glyph
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

// Narrow a glyph-local clip so every kept pixel maps inside the half-open
// rectangle [left, right) x [top, bottom) of the space frame is expressed in.
// Which glyph axis runs along that space's x depends on the rotation:
// dxX != 0 means glyph x does. Runs twice per glyph, so it stays inline
// under -Os.
__attribute__((always_inline)) inline void clipToRect(const Frame& frame, int left, int top, int right, int bottom,
                                                      Clip& clip) {
  if (frame.dxX != 0) {
    clipAxis(frame.x, frame.dxX, left, right, clip.left, clip.right);
    clipAxis(frame.y, frame.dyY, top, bottom, clip.top, clip.bottom);
  } else {
    clipAxis(frame.x, frame.dyX, left, right, clip.top, clip.bottom);
    clipAxis(frame.y, frame.dxY, top, bottom, clip.left, clip.right);
  }
}

// Keep pixel writes inline when decoding packed pixels.
__attribute__((always_inline)) inline void paint(uint8_t* buffer, int destination, uint8_t ink, uint16_t setMask,
                                                 uint16_t clearMask) {
  const uint16_t bit = static_cast<uint16_t>(1u << ink);
  if (((setMask | clearMask) & bit) == 0) return;
  const uint8_t mask = static_cast<uint8_t>(0x80u >> (destination & 7));
  if (clearMask & bit)
    buffer[destination >> 3] &= static_cast<uint8_t>(~mask);
  else
    buffer[destination >> 3] |= mask;
}

// Pack 8 4bpp pixel levels (MSB pixel i0 .. LSB pixel i7) against a 16-bit level mask into one byte.
__attribute__((always_inline)) inline uint8_t pack8Pixels4Bit(uint8_t i0, uint8_t i1, uint8_t i2, uint8_t i3,
                                                              uint8_t i4, uint8_t i5, uint8_t i6, uint8_t i7,
                                                              uint16_t mask) {
  return static_cast<uint8_t>((((mask >> i0) & 1u) << 7) | (((mask >> i1) & 1u) << 6) | (((mask >> i2) & 1u) << 5) |
                              (((mask >> i3) & 1u) << 4) | (((mask >> i4) & 1u) << 3) | (((mask >> i5) & 1u) << 2) |
                              (((mask >> i6) & 1u) << 1) | ((mask >> i7) & 1u));
}

__attribute__((always_inline)) inline void applyByte8Pixels4Bit(uint8_t* dst, uint8_t i0, uint8_t i1, uint8_t i2,
                                                                uint8_t i3, uint8_t i4, uint8_t i5, uint8_t i6,
                                                                uint8_t i7, uint16_t effectiveSetMask,
                                                                uint16_t clearMask) {
  if (effectiveSetMask == 0u) {
    const uint8_t c = pack8Pixels4Bit(i0, i1, i2, i3, i4, i5, i6, i7, clearMask);
    if (c != 0u) *dst &= static_cast<uint8_t>(~c);
  } else if (clearMask == 0u) {
    const uint8_t s = pack8Pixels4Bit(i0, i1, i2, i3, i4, i5, i6, i7, effectiveSetMask);
    if (s != 0u) *dst |= s;
  } else {
    const uint8_t s = pack8Pixels4Bit(i0, i1, i2, i3, i4, i5, i6, i7, effectiveSetMask);
    const uint8_t c = pack8Pixels4Bit(i0, i1, i2, i3, i4, i5, i6, i7, clearMask);
    if ((s | c) != 0u) *dst = static_cast<uint8_t>((*dst & ~c) | s);
  }
}

// Out-of-line rotated-Portrait 4bpp fast path (dxX == 0, dxY == +/-1, dyX == +1, dyY == 0):
// packs 8 vertically stacked glyph rows into 1 physical destination byte RMW while keeping
// GfxRenderer::drawText's hot loop compact inside ESP32-C3's 16 KB instruction cache.
__attribute__((noinline)) inline void drawRotatedPortrait4Bit(const uint8_t* bitmap, int width, uint8_t* buffer,
                                                              int stride, int dxY, const Clip& clip, int rowBit,
                                                              uint16_t effectiveSetMask, uint16_t clearMask,
                                                              bool zeroTransparent) {
  const ptrdiff_t colByteStep = static_cast<ptrdiff_t>(dxY) * stride;
  const auto paintRowConstBit = [&](const int yRow, const int rowBitPos) {
    const uint8_t bitMask = static_cast<uint8_t>(0x80u >> (rowBitPos & 7));
    const uint8_t invMask = static_cast<uint8_t>(~bitMask);
    uint8_t* dst = buffer + (rowBitPos >> 3);
    int srcIdx = yRow * width + clip.left;
    int rem = clip.right - clip.left;
    const auto applyOne = [&](uint8_t* d, const uint8_t ink) {
      const uint16_t b = static_cast<uint16_t>(1u << ink);
      if (clearMask & b)
        *d &= invMask;
      else if (effectiveSetMask & b)
        *d |= bitMask;
    };
    if (rem && (srcIdx & 1)) {
      applyOne(dst, bitmap[srcIdx >> 1] & 0x0Fu);
      ++srcIdx;
      dst += colByteStep;
      --rem;
    }
    while (rem >= 2) {
      const uint8_t packed = bitmap[srcIdx >> 1];
      if (!zeroTransparent || packed != 0u) {
        applyOne(dst, packed >> 4);
        applyOne(dst + colByteStep, packed & 0x0Fu);
      }
      srcIdx += 2;
      dst += 2 * colByteStep;
      rem -= 2;
    }
    if (rem) {
      applyOne(dst, bitmap[srcIdx >> 1] >> 4);
    }
  };

  int y = clip.top;
  while (y < clip.bottom && (rowBit & 7) != 0) {
    paintRowConstBit(y, rowBit);
    ++y;
    ++rowBit;
  }
  while (clip.bottom - y >= 8) {
    uint8_t* dst = buffer + (rowBit >> 3);
    int rem = clip.right - clip.left;
    if ((width & 1) == 0) {
      const size_t rowByteStride = static_cast<size_t>(width >> 1);
      int src0 = y * width + clip.left;
      if (rem && (src0 & 1)) {
        const uint8_t* s = bitmap + (src0 >> 1);
        const uint8_t i0 = s[0] & 0x0Fu;
        const uint8_t i1 = s[rowByteStride] & 0x0Fu;
        const uint8_t i2 = s[rowByteStride * 2] & 0x0Fu;
        const uint8_t i3 = s[rowByteStride * 3] & 0x0Fu;
        const uint8_t i4 = s[rowByteStride * 4] & 0x0Fu;
        const uint8_t i5 = s[rowByteStride * 5] & 0x0Fu;
        const uint8_t i6 = s[rowByteStride * 6] & 0x0Fu;
        const uint8_t i7 = s[rowByteStride * 7] & 0x0Fu;
        if (!zeroTransparent || ((i0 | i1 | i2 | i3 | i4 | i5 | i6 | i7) != 0u)) {
          applyByte8Pixels4Bit(dst, i0, i1, i2, i3, i4, i5, i6, i7, effectiveSetMask, clearMask);
        }
        ++src0;
        dst += colByteStep;
        --rem;
      }
      const uint8_t* sCol = bitmap + (src0 >> 1);
      while (rem >= 2) {
        const uint8_t b0 = sCol[0];
        const uint8_t b1 = sCol[rowByteStride];
        const uint8_t b2 = sCol[rowByteStride * 2];
        const uint8_t b3 = sCol[rowByteStride * 3];
        const uint8_t b4 = sCol[rowByteStride * 4];
        const uint8_t b5 = sCol[rowByteStride * 5];
        const uint8_t b6 = sCol[rowByteStride * 6];
        const uint8_t b7 = sCol[rowByteStride * 7];
        if (!zeroTransparent || ((b0 | b1 | b2 | b3 | b4 | b5 | b6 | b7) != 0u)) {
          applyByte8Pixels4Bit(dst, b0 >> 4, b1 >> 4, b2 >> 4, b3 >> 4, b4 >> 4, b5 >> 4, b6 >> 4, b7 >> 4,
                               effectiveSetMask, clearMask);
          applyByte8Pixels4Bit(dst + colByteStep, b0 & 0x0Fu, b1 & 0x0Fu, b2 & 0x0Fu, b3 & 0x0Fu, b4 & 0x0Fu,
                               b5 & 0x0Fu, b6 & 0x0Fu, b7 & 0x0Fu, effectiveSetMask, clearMask);
        }
        ++sCol;
        dst += 2 * colByteStep;
        rem -= 2;
      }
      if (rem) {
        const uint8_t i0 = sCol[0] >> 4;
        const uint8_t i1 = sCol[rowByteStride] >> 4;
        const uint8_t i2 = sCol[rowByteStride * 2] >> 4;
        const uint8_t i3 = sCol[rowByteStride * 3] >> 4;
        const uint8_t i4 = sCol[rowByteStride * 4] >> 4;
        const uint8_t i5 = sCol[rowByteStride * 5] >> 4;
        const uint8_t i6 = sCol[rowByteStride * 6] >> 4;
        const uint8_t i7 = sCol[rowByteStride * 7] >> 4;
        if (!zeroTransparent || ((i0 | i1 | i2 | i3 | i4 | i5 | i6 | i7) != 0u)) {
          applyByte8Pixels4Bit(dst, i0, i1, i2, i3, i4, i5, i6, i7, effectiveSetMask, clearMask);
        }
      }
    } else {
      for (int gx = clip.left; gx < clip.right; ++gx, dst += colByteStep) {
        int idx = y * width + gx;
        const uint8_t i0 = (idx & 1) ? (bitmap[idx >> 1] & 0x0Fu) : (bitmap[idx >> 1] >> 4);
        idx += width;
        const uint8_t i1 = (idx & 1) ? (bitmap[idx >> 1] & 0x0Fu) : (bitmap[idx >> 1] >> 4);
        idx += width;
        const uint8_t i2 = (idx & 1) ? (bitmap[idx >> 1] & 0x0Fu) : (bitmap[idx >> 1] >> 4);
        idx += width;
        const uint8_t i3 = (idx & 1) ? (bitmap[idx >> 1] & 0x0Fu) : (bitmap[idx >> 1] >> 4);
        idx += width;
        const uint8_t i4 = (idx & 1) ? (bitmap[idx >> 1] & 0x0Fu) : (bitmap[idx >> 1] >> 4);
        idx += width;
        const uint8_t i5 = (idx & 1) ? (bitmap[idx >> 1] & 0x0Fu) : (bitmap[idx >> 1] >> 4);
        idx += width;
        const uint8_t i6 = (idx & 1) ? (bitmap[idx >> 1] & 0x0Fu) : (bitmap[idx >> 1] >> 4);
        idx += width;
        const uint8_t i7 = (idx & 1) ? (bitmap[idx >> 1] & 0x0Fu) : (bitmap[idx >> 1] >> 4);
        if (!zeroTransparent || ((i0 | i1 | i2 | i3 | i4 | i5 | i6 | i7) != 0u)) {
          applyByte8Pixels4Bit(dst, i0, i1, i2, i3, i4, i5, i6, i7, effectiveSetMask, clearMask);
        }
      }
    }
    y += 8;
    rowBit += 8;
  }
  while (y < clip.bottom) {
    paintRowConstBit(y, rowBit);
    ++y;
    ++rowBit;
  }
}

// Paint a packed 1bpp, 2bpp, or 4bpp glyph into target.
//
// bitmap: rows are contiguous, MSB first, 1, 2, or 4 bits per pixel; widths
//   need not be byte-aligned. 4bpp values are 0=white .. 15=black; 2bpp values
//   are 0=white, 1=light gray, 2=dark gray, 3=black; 1bpp value 1 is ink.
// plane: which values are painted. BW honours state (true clears the bit, i.e.
//   black). The gray planes only set bits on 2bpp/4bpp glyphs (0 means leave
//   alone, 1 means update); 1bpp glyphs paint ink with state on every plane.
// Clipping happens before pixel decoding, so fully hidden glyphs cost nothing.
inline void draw(const uint8_t* bitmap, int width, int height, uint8_t bitsPerPixel, Plane plane, bool state,
                 const Target& target, Clip clip) {
  uint16_t setMask = 0;
  uint16_t clearMask = 0;
  const auto applyStatePolarity = [&](uint16_t levels) {
    if (state)
      clearMask = levels;
    else
      setMask = levels;
  };

  switch (plane) {
    case Plane::BW:
      applyStatePolarity(bitsPerPixel == 4 ? kBw4BitMask : (bitsPerPixel == 2 ? 0x000Eu : 0x0002u));
      break;
    case Plane::GrayLSB: {
      const uint16_t levels = bitsPerPixel == 4 ? kGrayLsb4BitMask : (bitsPerPixel == 2 ? 0x0004u : 0x0002u);
      if (bitsPerPixel > 1)
        setMask = levels;
      else
        applyStatePolarity(levels);
      break;
    }
    case Plane::GrayMSB: {
      const uint16_t levels = bitsPerPixel == 4 ? kGrayMsb4BitMask : (bitsPerPixel == 2 ? 0x0006u : 0x0002u);
      if (bitsPerPixel > 1)
        setMask = levels;
      else
        applyStatePolarity(levels);
      break;
    }
  }
  if ((setMask | clearMask) == 0) return;

  // Intersect with the glyph bounds, then with the panel width and the
  // buffered row band.
  clip.left = std::max(clip.left, 0);
  clip.top = std::max(clip.top, 0);
  clip.right = std::min(clip.right, width);
  clip.bottom = std::min(clip.bottom, height);
  clipToRect(target.frame, 0, target.originY, target.width, target.originY + target.rows, clip);
  if (clip.left >= clip.right || clip.top >= clip.bottom) return;

  // Walk the framebuffer as a flat bit index. One glyph column advances
  // stepX bits and one glyph row advances stepY bits; each is +/-1 for the
  // axis that maps to physical x, or +/-strideBits for physical y.
  const Frame& frame = target.frame;
  const int strideBits = target.stride * 8;
  const int stepX = frame.dxY * strideBits + frame.dxX;
  const int stepY = frame.dyY * strideBits + frame.dyX;
  const uint16_t effectiveSetMask = static_cast<uint16_t>(setMask & ~clearMask);
  const bool zeroTransparent = (((effectiveSetMask | clearMask) & 1u) == 0u);
  int rowBit = (frame.y - target.originY) * strideBits + frame.x + clip.left * stepX + clip.top * stepY;

  // Rotated-Portrait 4bpp fast path (landscape panel rotated 90 deg:
  // dxX == 0, dxY == +/-1, dyX == +1, dyY == 0 -> stepY == +1, stepX == dxY * strideBits).
  // 8 vertically stacked glyph rows (y .. y+7) at each glyph column land in 1 physical
  // destination byte (bits 7..0), cutting destination RMWs by 8x.
  if (bitsPerPixel == 4 && frame.dxX == 0 && frame.dyX == 1 && frame.dyY == 0) {
    drawRotatedPortrait4Bit(bitmap, width, target.buffer, target.stride, frame.dxY, clip, rowBit, effectiveSetMask,
                            clearMask, zeroTransparent);
    return;
  }

  for (int y = clip.top; y < clip.bottom; ++y, rowBit += stepY) {
    int source = y * width + clip.left;
    int destination = rowBit;
    int remaining = clip.right - clip.left;
    if (bitsPerPixel == 4) {
      // Portrait scanline fast path (dxX == +1, dxY == 0 -> stepX == 1): align to
      // a destination byte boundary, then accumulate 8 4bpp pixels into a single
      // destination byte RMW instead of 8 per-pixel RMWs.
      if (stepX == 1 && remaining >= 8) {
        while ((destination & 7) != 0) {
          const uint8_t ink = (source & 1) ? (bitmap[source >> 1] & 0x0Fu) : (bitmap[source >> 1] >> 4);
          paint(target.buffer, destination, ink, effectiveSetMask, clearMask);
          ++source;
          ++destination;
          --remaining;
        }
        uint8_t* dst = target.buffer + (destination >> 3);
        const uint8_t* src = bitmap + (source >> 1);
        if ((source & 1) == 0) {
          while (remaining >= 8) {
            const uint8_t b0 = src[0];
            const uint8_t b1 = src[1];
            const uint8_t b2 = src[2];
            const uint8_t b3 = src[3];
            if (!zeroTransparent || ((b0 | b1 | b2 | b3) != 0u)) {
              applyByte8Pixels4Bit(dst, b0 >> 4, b0 & 0x0Fu, b1 >> 4, b1 & 0x0Fu, b2 >> 4, b2 & 0x0Fu, b3 >> 4,
                                   b3 & 0x0Fu, effectiveSetMask, clearMask);
            }
            src += 4;
            ++dst;
            source += 8;
            destination += 8;
            remaining -= 8;
          }
        } else {
          while (remaining >= 8) {
            const uint8_t i0 = src[0] & 0x0Fu;
            const uint8_t b1 = src[1];
            const uint8_t b2 = src[2];
            const uint8_t b3 = src[3];
            const uint8_t i7 = src[4] >> 4;
            if (!zeroTransparent || ((i0 | b1 | b2 | b3 | i7) != 0u)) {
              applyByte8Pixels4Bit(dst, i0, b1 >> 4, b1 & 0x0Fu, b2 >> 4, b2 & 0x0Fu, b3 >> 4, b3 & 0x0Fu, i7,
                                   effectiveSetMask, clearMask);
            }
            src += 4;
            ++dst;
            source += 8;
            destination += 8;
            remaining -= 8;
          }
        }
      }
      if (remaining && (source & 1)) {
        paint(target.buffer, destination, bitmap[source >> 1] & 0x0Fu, effectiveSetMask, clearMask);
        ++source;
        destination += stepX;
        --remaining;
      }
      while (remaining >= 2) {
        const uint8_t packed = bitmap[source >> 1];
        paint(target.buffer, destination, packed >> 4, effectiveSetMask, clearMask);
        paint(target.buffer, destination + stepX, packed & 0x0Fu, effectiveSetMask, clearMask);
        source += 2;
        destination += 2 * stepX;
        remaining -= 2;
      }
      if (remaining) {
        paint(target.buffer, destination, bitmap[source >> 1] >> 4, effectiveSetMask, clearMask);
      }
      continue;
    }
    if (bitsPerPixel == 2) {
      // Rows are bit-contiguous, not byte-padded. Align after any clipped prefix.
      while (remaining && (source & 3)) {
        paint(target.buffer, destination, (bitmap[source >> 2] >> (6 - (source & 3) * 2)) & 3, setMask, clearMask);
        ++source;
        destination += stepX;
        --remaining;
      }
      while (remaining >= 4) {
        const uint8_t packed = bitmap[source >> 2];
        paint(target.buffer, destination, packed >> 6, setMask, clearMask);
        paint(target.buffer, destination + stepX, (packed >> 4) & 3, setMask, clearMask);
        paint(target.buffer, destination + 2 * stepX, (packed >> 2) & 3, setMask, clearMask);
        paint(target.buffer, destination + 3 * stepX, packed & 3, setMask, clearMask);
        source += 4;
        destination += 4 * stepX;
        remaining -= 4;
      }
    }
    // One-bit glyphs and the clipped 2-bit tail use scalar decoding.
    while (remaining--) {
      const uint8_t ink = (bitsPerPixel == 2) ? ((bitmap[source >> 2] >> (6 - (source & 3) * 2)) & 3)
                                              : ((bitmap[source >> 3] >> (7 - (source & 7))) & 1);
      paint(target.buffer, destination, ink, setMask, clearMask);
      ++source;
      destination += stepX;
    }
  }
}

}  // namespace glyphBitmap
