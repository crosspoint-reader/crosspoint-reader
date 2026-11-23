#include <gtest/gtest.h>

#include <array>
#include <utility>
#include <vector>

#include "lib/GfxRenderer/GlyphBitmap.h"

namespace {
constexpr int PANEL_WIDTH = 40;
constexpr int PANEL_HEIGHT = 32;
constexpr int STRIDE = PANEL_WIDTH / 8;

using glyphBitmap::Plane;

// Logical placements used by the renderer: unrotated text, and side-button
// labels rotated 90 degrees clockwise (glyph x runs up, glyph y runs right).
constexpr std::array<std::array<int, 4>, 2> AXES = {{{1, 0, 0, 1}, {0, -1, 1, 0}}};

std::pair<int, int> physical(int orientation, int x, int y) {
  switch (orientation) {
    case 0:
      return {y, PANEL_HEIGHT - 1 - x};
    case 1:
      return {PANEL_WIDTH - 1 - x, PANEL_HEIGHT - 1 - y};
    case 2:
      return {PANEL_WIDTH - 1 - y, x};
    default:
      return {x, y};
  }
}

// Logical rectangle the renderer clips to; the glyph-local clip is derived from it.
struct Rect {
  int left;
  int top;
  int right;
  int bottom;
};

void compare(int orientation, int rotation, bool twoBit, Plane plane, bool state, int width, int height, int x, int y,
             int originY, int rows, Rect clip) {
  SCOPED_TRACE(::testing::Message() << orientation << ',' << rotation << ',' << twoBit << ',' << static_cast<int>(plane)
                                    << ',' << state << " size=" << width << 'x' << height << " at=" << x << ',' << y
                                    << " band=" << originY << ',' << rows);
  const auto [dxX, dxY, dyX, dyY] = AXES[rotation];
  const glyphBitmap::Frame logical{x, y, dxX, dxY, dyX, dyY};
  std::vector<uint8_t> bitmap((width * height * (twoBit ? 2 : 1) + 7) / 8);
  for (size_t i = 0; i < bitmap.size(); ++i) bitmap[i] = static_cast<uint8_t>(i * 73 + 0x1b);
  // Nonuniform background and guards verify transparent pixels and all off-band bytes.
  std::vector<uint8_t> expected(rows * STRIDE + 32);
  for (size_t i = 0; i < expected.size(); ++i) expected[i] = static_cast<uint8_t>(i * 53 + 0xa5);
  auto actual = expected;
  for (int gy = 0; gy < height; ++gy) {
    for (int gx = 0; gx < width; ++gx) {
      const int lx = x + gx * dxX + gy * dyX;
      const int ly = y + gx * dxY + gy * dyY;
      if (lx < clip.left || ly < clip.top || lx >= clip.right || ly >= clip.bottom) continue;
      const auto [px, py] = physical(orientation, lx, ly);
      if (px < 0 || px >= PANEL_WIDTH || py < originY || py >= originY + rows) continue;
      const int source = gy * width + gx;
      bool draw = false;
      bool black = state;
      if (twoBit) {
        const int value = 3 - ((bitmap[source / 4] >> ((3 - source % 4) * 2)) & 3);
        if (plane == Plane::BW) draw = value < 3;
        if (plane == Plane::GrayLSB) draw = value == 1;
        if (plane == Plane::GrayMSB) draw = value == 1 || value == 2;
        if (plane != Plane::BW) black = false;
      } else {
        draw = (bitmap[source / 8] >> (7 - source % 8)) & 1;
      }
      if (!draw) continue;
      auto& byte = expected[16 + (py - originY) * STRIDE + px / 8];
      if (black)
        byte &= static_cast<uint8_t>(~(0x80 >> (px % 8)));
      else
        byte |= 0x80 >> (px % 8);
    }
  }

  glyphBitmap::Clip local{0, 0, width, height};
  glyphBitmap::clipToRect(logical, clip.left, clip.top, clip.right, clip.bottom, local);

  const auto [px, py] = physical(orientation, x, y);
  const auto [xx, xy] = physical(orientation, x + dxX, y + dxY);
  const auto [yx, yy] = physical(orientation, x + dyX, y + dyY);
  const glyphBitmap::Target target{
      actual.data() + 16, PANEL_WIDTH, STRIDE, originY, rows, {px, py, xx - px, xy - py, yx - px, yy - py}};
  glyphBitmap::draw(bitmap.data(), width, height, static_cast<uint8_t>(twoBit ? 2 : 1), plane, state, target, local);
  EXPECT_EQ(expected, actual);
}
}  // namespace

TEST(GlyphBitmap, MatchesPerPixelReferenceAcrossOrientationsRotationsPlanesAndClipping) {
  for (int orientation = 0; orientation < 4; ++orientation) {
    for (int rotation = 0; rotation < 2; ++rotation) {
      for (bool twoBit : {false, true}) {
        for (Plane plane : {Plane::BW, Plane::GrayLSB, Plane::GrayMSB}) {
          for (bool state : {false, true}) {
            for (int width : {1, 3, 7, 16, 31}) {
              for (const auto [x, y] : {std::pair{-5, -3}, std::pair{0, 0}, std::pair{9, 11}, std::pair{29, 31}}) {
                for (const auto [origin, rows] :
                     {std::pair{0, 32}, std::pair{0, 7}, std::pair{7, 11}, std::pair{28, 4}}) {
                  compare(orientation, rotation, twoBit, plane, state, width, 13, x, y, origin, rows,
                          {-20, -20, 60, 60});
                  compare(orientation, rotation, twoBit, plane, state, width, 13, x, y, origin, rows, {2, 3, 18, 20});
                }
              }
            }
          }
        }
      }
    }
  }
}

TEST(GlyphBitmap, EmptyAndFullyClippedGlyphsDoNotWrite) {
  for (int orientation = 0; orientation < 4; ++orientation) {
    for (int rotation = 0; rotation < 2; ++rotation) {
      compare(orientation, rotation, true, Plane::BW, true, 0, 0, 0, 0, 0, 32, {0, 0, 40, 32});
      compare(orientation, rotation, true, Plane::BW, true, 11, 9, 100, 100, 0, 32, {0, 0, 40, 32});
      compare(orientation, rotation, true, Plane::BW, true, 11, 9, 0, 0, 0, 32, {12, 10, 15, 20});
    }
  }
}

namespace {
uint16_t expected4BitMask(Plane plane) {
  switch (plane) {
    case Plane::BW:
      return glyphBitmap::kBw4BitMask;
    case Plane::GrayLSB:
      return glyphBitmap::kGrayLsb4BitMask;
    case Plane::GrayMSB:
      return glyphBitmap::kGrayMsb4BitMask;
  }
  return 0;
}

void compare4Bit(int orientation, int rotation, Plane plane, bool state, int width, int height, int x, int y,
                 int originY, int rows, Rect clip) {
  const auto [dxX, dxY, dyX, dyY] = AXES[rotation];
  const glyphBitmap::Frame logical{x, y, dxX, dxY, dyX, dyY};
  std::vector<uint8_t> bitmap((width * height + 1) / 2);
  for (size_t i = 0; i < bitmap.size(); ++i) {
    bitmap[i] = static_cast<uint8_t>(((i * 5 + 3) << 4) | ((i * 11 + 7) & 0x0F));
  }
  if ((width * height) & 1) {
    bitmap.back() &= 0xF0u;
  }

  std::vector<uint8_t> expected(rows * STRIDE + 32);
  for (size_t i = 0; i < expected.size(); ++i) expected[i] = static_cast<uint8_t>(i * 37 + 0x5A);
  auto actual = expected;

  const uint16_t mask = expected4BitMask(plane);
  const bool clearBits = plane == Plane::BW ? state : false;
  for (int gy = 0; gy < height; ++gy) {
    for (int gx = 0; gx < width; ++gx) {
      const int lx = x + gx * dxX + gy * dyX;
      const int ly = y + gx * dxY + gy * dyY;
      if (lx < clip.left || ly < clip.top || lx >= clip.right || ly >= clip.bottom) continue;
      const auto [px, py] = physical(orientation, lx, ly);
      if (px < 0 || px >= PANEL_WIDTH || py < originY || py >= originY + rows) continue;
      const int source = gy * width + gx;
      const uint8_t ink = (source & 1) ? (bitmap[source >> 1] & 0x0Fu) : (bitmap[source >> 1] >> 4);
      if ((mask & (1u << ink)) == 0) continue;
      auto& byte = expected[16 + (py - originY) * STRIDE + px / 8];
      const uint8_t bit = static_cast<uint8_t>(0x80 >> (px % 8));
      if (clearBits)
        byte &= static_cast<uint8_t>(~bit);
      else
        byte |= bit;
    }
  }

  glyphBitmap::Clip local{0, 0, width, height};
  glyphBitmap::clipToRect(logical, clip.left, clip.top, clip.right, clip.bottom, local);
  const auto [px, py] = physical(orientation, x, y);
  const auto [xx, xy] = physical(orientation, x + dxX, y + dxY);
  const auto [yx, yy] = physical(orientation, x + dyX, y + dyY);
  const glyphBitmap::Target target{
      actual.data() + 16, PANEL_WIDTH, STRIDE, originY, rows, {px, py, xx - px, xy - py, yx - px, yy - py}};
  glyphBitmap::draw(bitmap.data(), width, height, 4, plane, state, target, local);
  EXPECT_EQ(expected, actual);
}
}  // namespace

TEST(GlyphBitmap, FourBitMatchesReferenceAcrossAllPlanesOrientationsAndOddWidths) {
  constexpr std::array<Plane, 3> ALL_PLANES = {Plane::BW, Plane::GrayLSB, Plane::GrayMSB};
  for (int orientation = 0; orientation < 4; ++orientation) {
    for (int rotation = 0; rotation < 2; ++rotation) {
      for (Plane plane : ALL_PLANES) {
        for (bool state : {false, true}) {
          for (int width : {1, 3, 7, 13, 16, 31}) {
            for (const auto [x, y] : {std::pair{-3, -2}, std::pair{0, 0}, std::pair{7, 9}}) {
              for (const auto [origin, rows] : {std::pair{0, 32}, std::pair{5, 11}}) {
                compare4Bit(orientation, rotation, plane, state, width, 11, x, y, origin, rows, {-10, -10, 50, 50});
                compare4Bit(orientation, rotation, plane, state, width, 11, x, y, origin, rows, {2, 3, 19, 21});
              }
            }
          }
        }
      }
    }
  }
}

TEST(GlyphBitmap, FourBitRampProducesExactFourGrayPlaneMasks) {
  const uint8_t ramp4bpp[8] = {0x01u, 0x23u, 0x45u, 0x67u, 0x89u, 0xABu, 0xCDu, 0xEFu};
  const glyphBitmap::Clip clip{0, 0, 16, 1};
  const glyphBitmap::Frame frame{0, 0, 1, 0, 0, 1};

  auto runPlane = [&](Plane plane, uint8_t clearVal) -> uint16_t {
    uint8_t row[STRIDE];
    std::fill_n(row, STRIDE, clearVal);
    glyphBitmap::draw(ramp4bpp, 16, 1, 4, plane, true, {row, PANEL_WIDTH, STRIDE, 0, 1, frame}, clip);
    uint16_t bitsByInk = 0;
    for (int ink = 0; ink < 16; ++ink) {
      if (((row[ink / 8] >> (7 - (ink % 8))) & 1u) != 0) {
        bitsByInk |= static_cast<uint16_t>(1u << ink);
      }
    }
    return bitsByInk;
  };

  EXPECT_EQ(static_cast<uint16_t>(~0xFFFCu), runPlane(Plane::BW, 0xFFu));
  EXPECT_EQ(0x0F80u, runPlane(Plane::GrayLSB, 0x00u));
  EXPECT_EQ(0x0FFCu, runPlane(Plane::GrayMSB, 0x00u));
}

TEST(GlyphBitmap, Portrait4BitFastPathBitExactAgainstScalarPaintAcrossPlanesOffsetsWidthsAndClips) {
  constexpr int FUZZ_WIDTH = 96;
  constexpr int FUZZ_HEIGHT = 12;
  constexpr int FUZZ_STRIDE = FUZZ_WIDTH / 8;
  constexpr int GUARD_BYTES = 16;

  constexpr std::array<Plane, 3> ALL_PLANES = {Plane::BW, Plane::GrayLSB, Plane::GrayMSB};

  auto computeMasks = [](Plane plane, bool state) -> std::pair<uint16_t, uint16_t> {
    uint16_t setMask = 0;
    uint16_t clearMask = 0;
    switch (plane) {
      case Plane::BW:
        if (state)
          clearMask = glyphBitmap::kBw4BitMask;
        else
          setMask = glyphBitmap::kBw4BitMask;
        break;
      case Plane::GrayLSB:
        setMask = glyphBitmap::kGrayLsb4BitMask;
        break;
      case Plane::GrayMSB:
        setMask = glyphBitmap::kGrayMsb4BitMask;
        break;
    }
    return {setMask, clearMask};
  };

  uint32_t rng = 0x59B4B170u;
  auto nextByte = [&rng]() -> uint8_t {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return static_cast<uint8_t>(rng & 0xFFu);
  };

  std::vector<uint8_t> expected(FUZZ_HEIGHT * FUZZ_STRIDE + 2 * GUARD_BYTES);
  std::vector<uint8_t> actual(expected.size());
  std::vector<uint8_t> bitmap;

  constexpr int GLYPH_H = 4;
  for (Plane plane : ALL_PLANES) {
    for (bool state : {true, false}) {
      const auto [setMask, clearMask] = computeMasks(plane, state);
      for (int width = 1; width <= 48; ++width) {
        bitmap.resize((width * GLYPH_H + 1) / 2);
        for (size_t i = 0; i < bitmap.size(); ++i) {
          bitmap[i] = nextByte();
        }
        // Embed a full zero span in row 1 to exercise the zeroTransparent 8-pixel skip.
        for (int gx = 0; gx < width; ++gx) {
          const int srcIdx = 1 * width + gx;
          if (srcIdx & 1)
            bitmap[srcIdx >> 1] &= 0xF0u;
          else
            bitmap[srcIdx >> 1] &= 0x0Fu;
        }
        if ((width * GLYPH_H) & 1) {
          bitmap.back() &= 0xF0u;
        }

        const std::array<std::pair<int, int>, 4> clips = {{
            {0, width},
            {1, std::max(1, width - 1)},
            {2, std::max(2, width - 3)},
            {std::min(width, 5), std::max(std::min(width, 5), width - 4)},
        }};

        for (int x = 0; x <= 15; ++x) {
          for (const auto [clipL, clipR] : clips) {
            for (size_t i = 0; i < expected.size(); ++i) {
              expected[i] = static_cast<uint8_t>(i * 41u + x * 13u + width * 7u + 0x5Au);
            }
            actual = expected;

            const int placeX =
                (clipL == 2 && width >= 24) ? (FUZZ_WIDTH - width + 5) : (clipL == 1 && width >= 16 ? -3 + (x & 3) : x);
            const int placeY = 2;
            const glyphBitmap::Frame frame{placeX, placeY, 1, 0, 0, 1};
            const glyphBitmap::Target target{
                actual.data() + GUARD_BYTES, FUZZ_WIDTH, FUZZ_STRIDE, 0, FUZZ_HEIGHT, frame};
            const glyphBitmap::Clip clip{clipL, 0, clipR, GLYPH_H};

            glyphBitmap::draw(bitmap.data(), width, GLYPH_H, 4, plane, state, target, clip);

            // Independent scalar reference using glyphBitmap::paint() per pixel.
            for (int gy = std::max(0, clip.top); gy < std::min(GLYPH_H, clip.bottom); ++gy) {
              const int py = placeY + gy;
              if (py < 0 || py >= FUZZ_HEIGHT) continue;
              for (int gx = std::max(0, clip.left); gx < std::min(width, clip.right); ++gx) {
                const int px = placeX + gx;
                if (px < 0 || px >= FUZZ_WIDTH) continue;
                const int srcIdx = gy * width + gx;
                const uint8_t ink = (srcIdx & 1) ? (bitmap[srcIdx >> 1] & 0x0Fu) : (bitmap[srcIdx >> 1] >> 4);
                glyphBitmap::paint(expected.data() + GUARD_BYTES, py * (FUZZ_STRIDE * 8) + px, ink, setMask, clearMask);
              }
            }

            ASSERT_EQ(expected, actual) << "plane=" << static_cast<int>(plane) << " state=" << state
                                        << " width=" << width << " x=" << x << " placeX=" << placeX << " clip=["
                                        << clipL << "," << clipR << ")";
          }
        }
      }
    }
  }

  // Also fuzz the rotated-Portrait 8-row vertical byte-accumulated fast path
  // (dxX == 0, dxY == -1 / +1, dyX == +1, dyY == 0) across widths 1..19, heights 1..48,
  // all 3 planes, bit offsets 0..15, and 4-side clipping bounds.
  constexpr int ROT_W = 64;
  constexpr int ROT_H = 56;
  constexpr int ROT_STRIDE = ROT_W / 8;
  expected.resize(ROT_H * ROT_STRIDE + 2 * GUARD_BYTES);
  actual.resize(expected.size());
  for (Plane plane : ALL_PLANES) {
    for (bool state : {true, false}) {
      const auto [setMask, clearMask] = computeMasks(plane, state);
      for (int dxY : {-1, 1}) {
        for (int height : {1, 2, 7, 8, 9, 15, 16, 17, 23, 24, 25, 33, 48}) {
          for (int width : {1, 2, 3, 4, 7, 8, 9, 15, 16, 19}) {
            bitmap.resize((width * height + 1) / 2);
            for (size_t i = 0; i < bitmap.size(); ++i) bitmap[i] = nextByte();
            // Zero-out column 0 to exercise zeroTransparent 8-row column skip.
            for (int gy = 0; gy < height; ++gy) {
              const int sIdx = gy * width;
              if (sIdx & 1)
                bitmap[sIdx >> 1] &= 0xF0u;
              else
                bitmap[sIdx >> 1] &= 0x0Fu;
            }
            if ((width * height) & 1) bitmap.back() &= 0xF0u;

            for (int physX : {0, 1, 2, 3, 5, 7, 8, 11, 15}) {
              for (const auto [clipTop, clipBottom] : {std::pair{0, height}, std::pair{1, std::max(1, height - 2)},
                                                       std::pair{3, std::max(3, height - 5)}}) {
                for (const auto [clipL, clipR] : {std::pair{0, width}, std::pair{1, std::max(1, width - 1)}}) {
                  for (size_t i = 0; i < expected.size(); ++i) {
                    expected[i] = static_cast<uint8_t>(i * 29u + physX * 17u + height * 5u + 0xA3u);
                  }
                  actual = expected;
                  const int physY = dxY < 0 ? (ROT_H - 3) : 2;
                  const glyphBitmap::Frame frame{physX, physY, 0, dxY, 1, 0};
                  const glyphBitmap::Target target{actual.data() + GUARD_BYTES, ROT_W, ROT_STRIDE, 0, ROT_H, frame};
                  const glyphBitmap::Clip clip{clipL, clipTop, clipR, clipBottom};

                  glyphBitmap::draw(bitmap.data(), width, height, 4, plane, state, target, clip);

                  for (int gy = std::max(0, clip.top); gy < std::min(height, clip.bottom); ++gy) {
                    const int px = physX + gy;
                    if (px < 0 || px >= ROT_W) continue;
                    for (int gx = std::max(0, clip.left); gx < std::min(width, clip.right); ++gx) {
                      const int py = physY + gx * dxY;
                      if (py < 0 || py >= ROT_H) continue;
                      const int srcIdx = gy * width + gx;
                      const uint8_t ink = (srcIdx & 1) ? (bitmap[srcIdx >> 1] & 0x0Fu) : (bitmap[srcIdx >> 1] >> 4);
                      glyphBitmap::paint(expected.data() + GUARD_BYTES, py * (ROT_STRIDE * 8) + px, ink, setMask,
                                         clearMask);
                    }
                  }

                  ASSERT_EQ(expected, actual) << "RotatedPortrait plane=" << static_cast<int>(plane) << " dxY=" << dxY
                                              << " w=" << width << " h=" << height << " physX=" << physX;
                }
              }
            }
          }
        }
      }
    }
  }
}
