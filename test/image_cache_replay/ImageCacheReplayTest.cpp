#include <iterator>
#include <utility>

#include "Epub/converters/JpegToFramebufferConverter.h"
#include "Epub/converters/PngToFramebufferConverter.h"
#include "ImageTest.h"

namespace {
// 25x19 indexed PNG. Palette [0, 104, 164, 255], index (x+y)%4, filter None.
constexpr uint8_t INDEXED_PNG[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00,
    0x19, 0x00, 0x00, 0x00, 0x13, 0x02, 0x03, 0x00, 0x00, 0x00, 0x18, 0x1c, 0x4e, 0x96, 0x00, 0x00, 0x00, 0x0c, 0x50,
    0x4c, 0x54, 0x45, 0x00, 0x00, 0x00, 0x68, 0x68, 0x68, 0xa4, 0xa4, 0xa4, 0xff, 0xff, 0xff, 0xac, 0x98, 0x6f, 0x48,
    0x00, 0x00, 0x00, 0x1e, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0x90, 0x06, 0x03, 0x06, 0x86, 0x1c, 0x30, 0x70,
    0x60, 0xd8, 0x08, 0x06, 0x0d, 0x0c, 0xc7, 0xc0, 0xe0, 0x00, 0xc3, 0x00, 0xc9, 0x03, 0x00, 0xc6, 0xc7, 0x3d, 0xe1,
    0xf7, 0xf9, 0xfa, 0x00, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
};

// RGB PNG: five rows of red, green, blue, and white at (x+y)%4, then an invalid DEFLATE block in the second IDAT.
constexpr uint8_t PARTIAL_PNG[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00,
    0x00, 0x19, 0x00, 0x00, 0x00, 0x13, 0x08, 0x02, 0x00, 0x00, 0x00, 0xea, 0x10, 0x31, 0x52, 0x00, 0x00, 0x00,
    0x23, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x62, 0xf8, 0xcf, 0xc0, 0xc0, 0x00, 0xc6, 0x20, 0x40, 0x21, 0x9b,
    0x6a, 0x06, 0x41, 0x0c, 0xa3, 0x92, 0x41, 0xff, 0x19, 0xa8, 0x65, 0x10, 0x88, 0x49, 0xc5, 0xf0, 0x02, 0x00,
    0x00, 0x00, 0xff, 0xff, 0x96, 0x6c, 0x6e, 0xeb, 0x00, 0x00, 0x00, 0x01, 0x49, 0x44, 0x41, 0x54, 0x07, 0xb6,
    0x5c, 0xe8, 0x4b, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
};

// cjpeg -grayscale -quality 100 -optimize; one constant shade per 8-pixel column.
constexpr uint8_t GRAY_JPEG[] = {
    0xff, 0xd8, 0xff, 0xe0, 0x00, 0x10, 0x4a, 0x46, 0x49, 0x46, 0x00, 0x01, 0x01, 0x00, 0x00, 0x01, 0x00, 0x01,
    0x00, 0x00, 0xff, 0xdb, 0x00, 0x43, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0xff,
    0xc0, 0x00, 0x0b, 0x08, 0x00, 0x13, 0x00, 0x19, 0x01, 0x01, 0x11, 0x00, 0xff, 0xc4, 0x00, 0x16, 0x00, 0x01,
    0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0a, 0x09, 0x0b,
    0xff, 0xc4, 0x00, 0x14, 0x10, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xff, 0xda, 0x00, 0x08, 0x01, 0x01, 0x00, 0x00, 0x3f, 0x00, 0xcf, 0xfc, 0xd0, 0x17,
    0x80, 0xb6, 0x18, 0x03, 0x9a, 0x02, 0xf0, 0x16, 0xc3, 0x00, 0x73, 0x40, 0x5e, 0x02, 0xd8, 0x7f, 0xff, 0xd9,
};

using ImageCacheReplayTest = ImageTest;

TEST_F(ImageCacheReplayTest, IndexedPngPixelsAndCacheSurvivePaletteAndRowScratchOom) {
  FileTest::files["image.png"] = {std::begin(INDEXED_PNG), std::end(INDEXED_PNG)};
  PngToFramebufferConverter png;
  const auto level = [&](int x, int y) { return dither(SHADES[(x * WIDTH / config.maxWidth + y) % 4], x, y); };
  for (const auto [width, fail] : {std::pair{WIDTH, 0u}, {23, 0u}, {WIDTH, 1536u}, {WIDTH, 8 * 32 + 15u}}) {
    SCOPED_TRACE(::testing::Message() << "width=" << width << " failBytes=" << fail);
    config.maxWidth = width;
    AllocationTest::failures = 0;
    AllocationTest::failBytes = fail;
    resetFrame();
    ASSERT_TRUE(png.decodeToFramebuffer("image.png", renderer, config));
    EXPECT_EQ(AllocationTest::failures, fail ? 1u : 0u);
    EXPECT_EQ(frame, expectedFrame(level));
    expectCache(expectedCache(level));
  }
}

TEST_F(ImageCacheReplayTest, IndexedPngCanFlattenOrPreserveTransparency) {
  constexpr int ALPHA[] = {255, 0, 64, 255};
  constexpr uint8_t TRNS[] = {0, 0, 0, 4, 't', 'R', 'N', 'S', 255, 0, 64, 255, 0xb0, 0xed, 0xc0, 0xe3};
  auto& input = FileTest::files["image.png"];
  input.assign(std::begin(INDEXED_PNG), std::end(INDEXED_PNG));
  input.insert(input.begin() + 57, std::begin(TRNS), std::end(TRNS));  // Between PLTE and IDAT.
  PngToFramebufferConverter png;
  for (const bool preserve : {false, true}) {
    SCOPED_TRACE(preserve);
    config.preserveAlpha = preserve;
    const auto level = [&](int x, int y) {
      const int i = (x + y) % 4;
      if (!preserve) return dither((SHADES[i] * ALPHA[i] + 255 * (255 - ALPHA[i])) / 255, x, y);
      const int threshold = 16 * BAYER[(config.y + y) % 4][(config.x + x) % 4];
      return ALPHA[i] > threshold ? dither(SHADES[i], x, y) : -1;
    };
    resetFrame();
    FileTest::files.erase(config.cachePath);
    ASSERT_TRUE(png.decodeToFramebuffer("image.png", renderer, config));
    EXPECT_EQ(frame, expectedFrame(level));
    if (preserve)
      EXPECT_FALSE(FileTest::files.contains(config.cachePath));
    else
      expectCache(expectedCache(level));
  }
}

TEST_F(ImageCacheReplayTest, PartialPngFlushesCompletedRowsAndDropsCache) {
  FileTest::files["image.png"] = {std::begin(PARTIAL_PNG), std::end(PARTIAL_PNG)};
  config.y = 1;  // The five completed rows do not fill the first output batch.
  PngToFramebufferConverter png;
  ASSERT_FALSE(png.decodeToFramebuffer("image.png", renderer, config));
  constexpr int GRAY[] = {76, 149, 28, 255};
  EXPECT_EQ(frame, expectedFrame([&](int x, int y) { return dither(GRAY[(x + y) % 4], x, y); }, 5));
  EXPECT_FALSE(FileTest::files.contains(config.cachePath));
}

TEST_F(ImageCacheReplayTest, JpegPixelsAndCacheSurviveRowScratchOomAndCacheWriteFailure) {
  Bytes jpegBytes(std::begin(GRAY_JPEG), std::end(GRAY_JPEG));
  // JPEGDEC rejects files shorter than 256 bytes. Add a 96-byte JPEG comment.
  jpegBytes.insert(jpegBytes.begin() + 2, {0xff, 0xfe, 0, 98});
  jpegBytes.insert(jpegBytes.begin() + 6, 96, 0);
  FileTest::files["image.jpg"] = std::move(jpegBytes);
  JpegToFramebufferConverter jpeg;
  const auto level = [&](int x, int y) { return dither(SHADES[x / 8], x, y); };
  for (const auto [fail, writes] : {std::pair{0u, -1}, {8 * 32 + 15u, -1}, {0u, 5}}) {
    SCOPED_TRACE(::testing::Message() << "failBytes=" << fail << " writesUntilFailure=" << writes);
    AllocationTest::failures = 0;
    AllocationTest::failBytes = fail;
    FileTest::writesUntilFailure = writes;
    resetFrame();
    bool cacheWritten = false;
    ASSERT_TRUE(jpeg.decodeToFramebuffer("image.jpg", renderer, config, cacheWritten));
    EXPECT_EQ(frame, expectedFrame(level));
    EXPECT_EQ(AllocationTest::failures, fail ? 1u : 0u);
    EXPECT_EQ(cacheWritten, writes == -1);
    if (writes == -1)
      expectCache(expectedCache(level));
    else
      EXPECT_FALSE(FileTest::files.contains(config.cachePath));
  }
}

TEST_F(ImageCacheReplayTest, ReplaysSplitCacheRowFromRamAndAfterSecondChunkOom) {
  // 49-byte rows straddle the 16 KiB RAM chunk; clipping leaves only a strip of the image.
  config.maxWidth = 193;
  config.maxHeight = 337;
  renderer.physicalWidth = 352;
  renderer.physicalHeight = 208;
  renderer.originY = 63;
  renderer.rows = 83;
  const auto level = [](int x, int y) { return (x ^ (2 * (x / 4)) ^ (y / 3) ^ (y / 8)) & 3; };
  const auto cache = expectedCache(level);
  FileTest::files[config.cachePath] = cache;
  ImageBlock block("image.png", "", config.maxWidth, config.maxHeight);
  const auto expected = expectedFrame(level);

  resetFrame();
  block.render(renderer, config.x, config.y);
  EXPECT_EQ(frame, expected);
  EXPECT_EQ(FileTest::readOpens, 1u);

  resetFrame();
  block.render(renderer, config.x, config.y);
  EXPECT_EQ(frame, expected);
  EXPECT_EQ(FileTest::readOpens, 1u);

  ImageBlock::releaseRenderCache();
  AllocationTest::failBytes = cache.size() - 4 - 16384;
  resetFrame();
  block.render(renderer, config.x, config.y);
  EXPECT_EQ(AllocationTest::failures, 1u);
  EXPECT_EQ(frame, expected);
  EXPECT_EQ(FileTest::readOpens, 2u);
  EXPECT_EQ(renderer.placeholders, 0u);
  expectCache(cache);
}
}  // namespace
