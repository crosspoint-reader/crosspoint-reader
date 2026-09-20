#include <gtest/gtest.h>

#include <array>
#include <utility>
#include <vector>

#include "lib/GfxRenderer/GlyphBitmap.h"

namespace {
constexpr int PANEL_WIDTH = 40;
constexpr int PANEL_HEIGHT = 32;
constexpr int STRIDE = PANEL_WIDTH / 8;

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

void compare(int orientation, bool twoBit, int mode, bool state, int width, int height, int x, int y, int originY,
             int rows, glyphBitmap::Clip clip) {
  SCOPED_TRACE(::testing::Message() << orientation << ',' << twoBit << ',' << mode << ',' << state << " size=" << width
                                    << 'x' << height << " at=" << x << ',' << y << " band=" << originY << ',' << rows);
  std::vector<uint8_t> bitmap((width * height * (twoBit ? 2 : 1) + 7) / 8);
  for (size_t i = 0; i < bitmap.size(); ++i) bitmap[i] = static_cast<uint8_t>(i * 73 + 0x1b);
  // Nonuniform background and guards verify transparent pixels and all off-band bytes.
  std::vector<uint8_t> expected(rows * STRIDE + 32);
  for (size_t i = 0; i < expected.size(); ++i) expected[i] = static_cast<uint8_t>(i * 53 + 0xa5);
  auto actual = expected;
  for (int gy = 0; gy < height; ++gy) {
    for (int gx = 0; gx < width; ++gx) {
      if (gx < clip.left || gy < clip.top || gx >= clip.right || gy >= clip.bottom) continue;
      const auto [px, py] = physical(orientation, x + gx, y + gy);
      if (px < 0 || px >= PANEL_WIDTH || py < originY || py >= originY + rows) continue;
      const int source = gy * width + gx;
      bool draw = false;
      bool black = state;
      if (twoBit) {
        const int value = 3 - ((bitmap[source / 4] >> ((3 - source % 4) * 2)) & 3);
        if (mode == 0) draw = value < 3;
        if (mode == 1) draw = value == 1;
        if (mode == 2) draw = value == 1 || value == 2;
        if (mode != 0) black = false;
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

  const auto [px, py] = physical(orientation, x, y);
  const auto [xx, xy] = physical(orientation, x + 1, y);
  const auto [yx, yy] = physical(orientation, x, y + 1);
  const glyphBitmap::Target target{
      actual.data() + 16, PANEL_WIDTH, STRIDE, originY, rows, px, py, xx - px, xy - py, yx - px, yy - py};
  const uint8_t levels = !twoBit ? 0x02 : mode == 0 ? 0x0e : mode == 2 ? 0x06 : 0x04;
  glyphBitmap::draw(bitmap.data(), width, height, twoBit, levels, !twoBit || mode == 0 ? state : false, target, clip);
  EXPECT_EQ(expected, actual);
}
}  // namespace

TEST(GlyphBitmap, MatchesPerPixelReferenceAcrossOrientationsPlanesAndClipping) {
  for (int orientation = 0; orientation < 4; ++orientation) {
    for (bool twoBit : {false, true}) {
      for (int mode = 0; mode < 3; ++mode) {
        for (bool state : {false, true}) {
          for (int width : {1, 3, 7, 16, 31}) {
            for (const auto [x, y] : {std::pair{-5, -3}, std::pair{0, 0}, std::pair{9, 11}, std::pair{29, 31}}) {
              for (const auto [origin, rows] :
                   {std::pair{0, 32}, std::pair{0, 7}, std::pair{7, 11}, std::pair{28, 4}}) {
                compare(orientation, twoBit, mode, state, width, 13, x, y, origin, rows, {-20, -20, 60, 60});
                compare(orientation, twoBit, mode, state, width, 13, x, y, origin, rows, {2, 3, 9, 11});
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
    compare(orientation, true, 0, true, 0, 0, 0, 0, 0, 32, {0, 0, 40, 32});
    compare(orientation, true, 0, true, 11, 9, 100, 100, 0, 32, {0, 0, 40, 32});
    compare(orientation, true, 0, true, 11, 9, 0, 0, 0, 32, {12, 10, 15, 20});
  }
}
