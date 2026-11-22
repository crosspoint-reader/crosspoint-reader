#pragma once

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Memory.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

#include "Epub/blocks/ImageBlock.h"
#include "Epub/converters/ImageToFramebufferDecoder.h"

class ImageTest : public ::testing::Test {
 protected:
  using Bytes = std::vector<uint8_t>;
  static constexpr int WIDTH = 25;
  static constexpr int HEIGHT = 19;
  static constexpr int GUARD = 16;
  static constexpr int SHADES[] = {0, 104, 164, 255};
  static constexpr int BAYER[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

  GfxRenderer renderer;
  RenderConfig config{};
  Bytes frame;

  void SetUp() override {
    renderer.physicalWidth = 64;
    renderer.physicalHeight = renderer.rows = 48;
    renderer.mode = GfxRenderer::GRAYSCALE_MSB;
    renderer.absolute = true;
    config.x = 5;
    config.y = 3;
    config.maxWidth = WIDTH;
    config.maxHeight = HEIGHT;
    config.useExactDimensions = true;
    config.cachePath = "image.pxc";
    resetFrame();
  }

  void TearDown() override {
    ImageBlock::releaseRenderCache();
    ImageBlock::clearRenderFailures();
    FileTest::files.clear();
    FileTest::readOpens = 0;
    FileTest::writesUntilFailure = -1;
    AllocationTest::failBytes = 0;
    AllocationTest::failures = 0;
  }

  Bytes initialFrame() const { return Bytes(renderer.rows * renderer.getDisplayWidthBytes() + 2 * GUARD, 0xa5); }

  void resetFrame() {
    frame = initialFrame();
    renderer.target = frame.data() + GUARD;
  }

  // Independent pixel reference, including logical screen and physical strip clipping.
  template <typename Level>
  Bytes expectedFrame(Level levelAt, int rows = -1) const {
    auto result = initialFrame();
    for (int y = 0; y < (rows < 0 ? config.maxHeight : rows); ++y) {
      for (int x = 0; x < config.maxWidth; ++x) {
        const int lx = config.x + x, ly = config.y + y;
        if (lx < 0 || ly < 0 || lx >= renderer.getScreenWidth() || ly >= renderer.getScreenHeight()) continue;
        int px, py;
        switch (renderer.orientation) {
          case GfxRenderer::Portrait:
            px = ly;
            py = renderer.physicalHeight - 1 - lx;
            break;
          case GfxRenderer::PortraitInverted:
            px = renderer.physicalWidth - 1 - ly;
            py = lx;
            break;
          case GfxRenderer::LandscapeClockwise:
            px = renderer.physicalWidth - 1 - lx;
            py = renderer.physicalHeight - 1 - ly;
            break;
          case GfxRenderer::LandscapeCounterClockwise:
            px = lx;
            py = ly;
            break;
        }
        py -= renderer.originY;
        if (py < 0 || py >= renderer.rows) continue;
        const int level = levelAt(x, y);
        if (level < 0) continue;  // Transparent pixel.
        auto& byte = result[GUARD + py * renderer.getDisplayWidthBytes() + px / 8];
        const auto mask = 0x80 >> (px % 8);
        bool white;
        if (renderer.mode == GfxRenderer::BW) {
          if (level == 3) continue;
          white = false;
        } else if (renderer.absolute) {
          white = (level & (renderer.mode == GfxRenderer::GRAYSCALE_MSB ? 2 : 1)) != 0;
        } else {
          if (level != 1 && (level != 2 || renderer.mode != GfxRenderer::GRAYSCALE_MSB)) continue;
          white = true;
        }
        if (white)
          byte |= mask;
        else
          byte &= ~mask;
      }
    }
    return result;
  }

  template <typename Level>
  Bytes expectedCache(Level levelAt) const {
    const int width = config.maxWidth, height = config.maxHeight, stride = (width + 3) / 4;
    Bytes result(4 + stride * height);
    result[0] = width & 0xff;
    result[1] = width >> 8;
    result[2] = height & 0xff;
    result[3] = height >> 8;
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        result[4 + y * stride + x / 4] |= levelAt(x, y) << (6 - 2 * (x % 4));
      }
    }
    return result;
  }

  int dither(int gray, int x, int y) const {
    const int adjusted = gray + (BAYER[(config.y + y) % 4][(config.x + x) % 4] - 8) * 5;
    return std::clamp(adjusted, 0, 255) / 64;
  }

  void expectCache(const Bytes& expected) const {
    const auto it = FileTest::files.find(config.cachePath);
    ASSERT_NE(it, FileTest::files.end());
    EXPECT_EQ(it->second, expected);
  }
};
