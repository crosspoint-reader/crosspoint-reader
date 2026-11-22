#include <zlib.h>

#include <array>
#include <iterator>

#include "Epub/converters/JpegToFramebufferConverter.h"
#include "Epub/converters/PngToFramebufferConverter.h"
#include "ImageTest.h"
#include "ScalingJpeg.h"

bool decodeScalarPng(const std::string& path, GfxRenderer& renderer, const RenderConfig& config);
bool decodeScalarJpeg(const std::string& path, GfxRenderer& renderer, const RenderConfig& config);

namespace {
using Bytes = std::vector<uint8_t>;

std::array<uint8_t, 3> rgbPixel(int x, int y) {
  return {static_cast<uint8_t>(x * 17 + y * 31), static_cast<uint8_t>(x * 43 + y * 7),
          static_cast<uint8_t>(x * 11 + y * 53)};
}

void appendBe32(Bytes& bytes, uint32_t value) {
  for (int shift : {24, 16, 8, 0}) bytes.push_back(static_cast<uint8_t>(value >> shift));
}

void appendChunk(Bytes& png, const char* type, const Bytes& data) {
  appendBe32(png, data.size());
  const size_t start = png.size();
  png.insert(png.end(), type, type + 4);
  png.insert(png.end(), data.begin(), data.end());
  appendBe32(png, crc32(0, png.data() + start, 4 + data.size()));
}

Bytes makeRgbPng(int width, int height) {
  Bytes png{137, 'P', 'N', 'G', 13, 10, 26, 10}, header, raw;
  appendBe32(header, width);
  appendBe32(header, height);
  header.insert(header.end(), {8, 2, 0, 0, 0});
  appendChunk(png, "IHDR", header);
  for (int y = 0; y < height; ++y) {
    raw.push_back(0);  // Filter None.
    for (int x = 0; x < width; ++x) {
      const auto pixel = rgbPixel(x, y);
      raw.insert(raw.end(), pixel.begin(), pixel.end());
    }
  }
  uLongf size = compressBound(raw.size());
  Bytes compressed(size);
  if (compress(compressed.data(), &size, raw.data(), raw.size()) != Z_OK) {
    ADD_FAILURE() << "Failed to compress PNG fixture";
    return {};
  }
  compressed.resize(size);
  appendChunk(png, "IDAT", compressed);
  appendChunk(png, "IEND", {});
  return png;
}

struct ScaleCase {
  int width, height;
  float cropX, cropY;
  bool clipScreen;
};

constexpr ScaleCase CASES[] = {
    {25, 19, 0, 0, false},          // 1:1.
    {17, 11, 0, 0, false},          // Fine downscale.
    {41, 29, 0, 0, false},          // Upscale both axes.
    {23, 25, 0.48f, 0.32f, false},  // Cropped upscale with output narrower than the source buffer.
    {9, 31, 0.32f, 0.42f, false},   // Mixed-axis scaling.
    {7, 5, 0.24f, 0.21f, false},    // JPEG coarse scaling plus fine scaling.
    {6, 4, 0, 0, false},            // JPEG 1/4 coarse scale.
    {3, 2, 0, 0, false},            // JPEG 1/8 coarse scale.
    {41, 29, 0.24f, 0.21f, true},   // Right/bottom screen clipping and cache zero-fill.
};

class ImageScalingTest : public ImageTest {
 protected:
  void compare(bool jpeg, bool failRows = false) {
    const char* path = jpeg ? "scale.jpg" : "scale.png";
    AllocationTest::failBytes = 0;
    FileTest::files.erase(config.cachePath);
    resetFrame();
    ASSERT_TRUE(jpeg ? decodeScalarJpeg(path, renderer, config) : decodeScalarPng(path, renderer, config));
    const auto scalarFrame = frame;
    if (renderer.rows == renderer.physicalHeight && config.maxWidth == WIDTH && config.maxHeight == HEIGHT) {
      EXPECT_NE(scalarFrame, initialFrame());
    }
    const bool cached = FileTest::files.contains(config.cachePath);
    ASSERT_TRUE(jpeg || cached);
    const auto scalarCache = cached ? FileTest::files.at(config.cachePath) : Bytes{};
    const size_t stride = (config.maxWidth + 3) / 4;
    if (cached) {
      ASSERT_EQ(scalarCache.size(), 4 + stride * config.maxHeight);
      const auto cachedLevel = [&](int x, int y) {
        return (scalarCache[4 + y * stride + x / 4] >> (6 - 2 * (x % 4))) & 3;
      };
      // Legacy cropped JPEG callbacks can also paint outside the image/cache rectangle.
      if (!jpeg || (config.sourceCropX == 0 && config.sourceCropY == 0)) {
        EXPECT_EQ(scalarFrame, expectedFrame(cachedLevel));
      }
    }

    if (!jpeg) {
      const int left = static_cast<int>(WIDTH * config.sourceCropX / 2);
      const int top = static_cast<int>(HEIGHT * config.sourceCropY / 2);
      const int visibleWidth = WIDTH - 2 * left, visibleHeight = HEIGHT - 2 * top;
      const auto level = [&](int x, int y) {
        const int sx = left + x * visibleWidth / config.maxWidth;
        // PNG keeps the first source row of each downscale bucket.
        const int sy =
            top + (config.maxHeight > visibleHeight ? ((y + 1) * visibleHeight - 1) / config.maxHeight
                                                    : (y * visibleHeight + config.maxHeight - 1) / config.maxHeight);
        const auto pixel = rgbPixel(sx, sy);
        return dither((77 * pixel[0] + 150 * pixel[1] + 29 * pixel[2]) / 256, x, y);
      };
      EXPECT_EQ(scalarFrame, expectedFrame(level));
      EXPECT_EQ(scalarCache, expectedCache([&](int x, int y) {
                  return config.x + x < renderer.getScreenWidth() && config.y + y < renderer.getScreenHeight()
                             ? level(x, y)
                             : 0;
                }));
    }

    const int visibleWidth = std::min(config.maxWidth, renderer.getScreenWidth() - config.x);
    AllocationTest::failBytes = failRows ? 8 * ((visibleWidth + 15) & ~15) + 15 : 0;
    AllocationTest::failures = 0;
    FileTest::files.erase(config.cachePath);
    resetFrame();
    PngToFramebufferConverter png;
    JpegToFramebufferConverter jpg;
    ASSERT_TRUE(jpeg ? jpg.decodeToFramebuffer(path, renderer, config)
                     : png.decodeToFramebuffer(path, renderer, config));
    EXPECT_EQ(AllocationTest::failures, failRows ? 1u : 0u);
    EXPECT_EQ(frame, scalarFrame);
    ASSERT_EQ(FileTest::files.contains(config.cachePath), cached);
    if (cached) expectCache(scalarCache);
    AllocationTest::failBytes = 0;
  }

  void matrix(bool jpeg) {
    FileTest::files[jpeg ? "scale.jpg" : "scale.png"] =
        jpeg ? Bytes(std::begin(SCALING_JPEG), std::end(SCALING_JPEG)) : makeRgbPng(WIDTH, HEIGHT);
    for (const auto& dimensions : CASES) {
      config.maxWidth = dimensions.width;
      config.maxHeight = dimensions.height;
      config.sourceCropX = dimensions.cropX;
      config.sourceCropY = dimensions.cropY;
      for (const auto orientation : {GfxRenderer::Portrait, GfxRenderer::PortraitInverted,
                                     GfxRenderer::LandscapeClockwise, GfxRenderer::LandscapeCounterClockwise}) {
        renderer.orientation = orientation;
        config.x = dimensions.clipScreen ? renderer.getScreenWidth() - 10 : 5;
        config.y = dimensions.clipScreen ? renderer.getScreenHeight() - 9 : 3;
        for (const auto mode : {GfxRenderer::BW, GfxRenderer::GRAYSCALE_LSB, GfxRenderer::GRAYSCALE_MSB}) {
          renderer.mode = mode;
          for (const bool absolute : {false, true}) {
            renderer.absolute = absolute;
            for (const bool strip : {false, true}) {
              renderer.originY = strip ? 9 : 0;
              renderer.rows = strip ? 23 : renderer.physicalHeight;
              SCOPED_TRACE(::testing::Message()
                           << dimensions.width << "x" << dimensions.height << " crop=" << dimensions.cropX << ","
                           << dimensions.cropY << " orientation=" << orientation << " mode=" << mode
                           << " absolute=" << absolute << " strip=" << strip << " clipped=" << dimensions.clipScreen);
              ASSERT_NO_FATAL_FAILURE(compare(jpeg));
            }
          }
        }
      }
    }
  }
};

TEST_F(ImageScalingTest, RgbPngMatchesIndependentSamplingAndDisabledConverter) { matrix(false); }

TEST_F(ImageScalingTest, NonuniformJpegMatchesDisabledConverterAcrossScaleBranches) { matrix(true); }

TEST_F(ImageScalingTest, ScaledPortraitScratchOomMatchesDisabledConverters) {
  FileTest::files["scale.png"] = makeRgbPng(WIDTH, HEIGHT);
  FileTest::files["scale.jpg"] = {std::begin(SCALING_JPEG), std::end(SCALING_JPEG)};
  config.maxWidth = 23;
  config.maxHeight = 25;
  config.sourceCropX = 0.48f;
  config.sourceCropY = 0.32f;
  for (const auto orientation : {GfxRenderer::Portrait, GfxRenderer::PortraitInverted}) {
    renderer.orientation = orientation;
    ASSERT_NO_FATAL_FAILURE(compare(false, true));
    ASSERT_NO_FATAL_FAILURE(compare(true, true));
  }
}
}  // namespace
