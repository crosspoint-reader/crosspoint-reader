#include <gtest/gtest.h>

#include <array>
#include <vector>

#include "lib/GfxRenderer/BitmapHelpers.h"
#include "lib/GfxRenderer/ImagePackedBand.h"

namespace {

TEST(AbsoluteGrayscale, PackedBandMatchesClippedScalarDraw) {
  using glyphBitmap::Plane;
  using imagePacked::WhitePixels;
  constexpr int STRIDE = 3;
  for (int width = 1; width <= 17; ++width) {
    std::vector<uint8_t> packed[8];
    imagePacked::BandRows rows{};
    for (int lane : {0, 2, 5, 6, 7}) {
      packed[lane].resize((width + 3) / 4);
      rows[lane] = packed[lane].data();
      for (int x = 0; x < width; ++x) {
        const int level = (x + lane) % 4;
        packed[lane][x / 4] |= level << (6 - 2 * (x % 4));
      }
    }
    for (int first : {0, width / 2}) {
      for (int end : {width, width - 1}) {
        if (first >= end) continue;
        for (int step : {STRIDE, -STRIDE}) {
          const auto index = [&](int x) { return (step > 0 ? x : width - 1 - x) * STRIDE + 1; };
          for (const auto plane : {Plane::BW, Plane::GrayLSB, Plane::GrayMSB}) {
            for (bool absolute : {false, true}) {
              for (const auto white : {WhitePixels::Draw, WhitePixels::KeepBackground}) {
                std::array<uint8_t, 17 * STRIDE> frame;
                for (size_t i = 0; i < frame.size(); ++i) frame[i] = i % 2 ? 0x5a : 0xa5;
                auto expected = frame;
                for (int x = first; x < end; ++x) {
                  for (int lane = 0; lane < 8; ++lane) {
                    if (!rows[lane]) continue;
                    const int level = (x + lane) % 4;
                    if (level == 3 && white == WhitePixels::KeepBackground) continue;
                    const auto pixel = plane == Plane::BW ? GrayPlanePixel{level != 3, true}
                                                          : grayPlanePixel(level, plane == Plane::GrayMSB, absolute);
                    if (!pixel.write) continue;
                    const uint8_t bit = 0x80 >> lane;
                    if (pixel.black)
                      expected[index(x)] &= ~bit;
                    else
                      expected[index(x)] |= bit;
                  }
                }
                imagePacked::drawBand(rows, {width, first, end}, {&frame[index(first)], step, plane, absolute}, white);
                ASSERT_EQ(frame, expected) << "width=" << width << " first=" << first << " end=" << end
                                           << " step=" << step << " plane=" << static_cast<int>(plane)
                                           << " absolute=" << absolute << " white=" << static_cast<int>(white);
              }
            }
          }
        }
      }
    }
  }
}

}  // namespace
