#include "Epub/converters/GifToFramebufferConverter.h"
#include "ImageTest.h"

namespace {
std::vector<uint8_t> makeGif() {
  // 25x19 canvas; first frame is a 21x13 local-palette rectangle at (2,3).
  std::vector<uint8_t> gif{'G',  'I', 'F', '8', '9', 'a', 25,   0,   19,  0,   0,   0,   0,   0x21, 0xf9,
                           4,    1,   0,   0,   2,   0,   0x2c, 2,   0,   3,   0,   21,  0,   13,   0,
                           0x81, 0,   0,   0,   104, 104, 104,  164, 164, 164, 255, 255, 255, 2};
  // Clear before every pixel so all LZW codes stay three bits wide.
  std::vector<uint8_t> data((21 * 13 * 6 + 3 + 7) / 8);
  size_t bit = 0;
  const auto emit = [&](int code) {
    for (int i = 0; i < 3; ++i, ++bit) data[bit / 8] |= ((code >> i) & 1) << (bit % 8);
  };
  for (int y = 0; y < 13; ++y) {
    for (int x = 0; x < 21; ++x) {
      emit(4);
      emit((x + y) % 4);
    }
  }
  emit(5);
  gif.push_back(static_cast<uint8_t>(data.size()));
  gif.insert(gif.end(), data.begin(), data.end());
  gif.push_back(0);
  // A second, black frame pixel at (0,0) must not appear in the output.
  gif.insert(gif.end(), {0x2c, 0, 0, 0, 0, 1, 0, 1, 0, 0x80, 0, 0, 0, 255, 255, 255, 2, 2, 0x44, 0x01, 0, 0x3b});
  return gif;
}

class GifFramebufferTest : public ImageTest {
 protected:
  int canvasGray(int sx, int sy) const {
    sx -= 2;
    sy -= 3;
    if (sx < 0 || sx >= 21 || sy < 0 || sy >= 13) return 255;
    const int index = (sx + sy) % 4;
    return index == 2 ? 255 : SHADES[index];
  }
};

TEST_F(GifFramebufferTest, ScalesClippedTransparentSubframeAndPreservesCacheOnScratchOom) {
  FileTest::files["image.gif"] = makeGif();
  renderer.orientation = GfxRenderer::PortraitInverted;
  renderer.originY = 11;
  renderer.rows = 17;
  config.maxWidth = 31;
  config.maxHeight = 23;
  const auto level = [&](int x, int y) {
    const int sx = x * WIDTH / config.maxWidth;
    const int sy = ((y + 1) * HEIGHT - 1) / config.maxHeight;
    return dither(canvasGray(sx, sy), x, y);
  };
  GifToFramebufferConverter gif;
  for (const size_t fail : {0u, 64u}) {
    SCOPED_TRACE(fail);
    AllocationTest::failures = 0;
    AllocationTest::failBytes = fail;  // Eight packed rows, eight bytes per row.
    resetFrame();
    ASSERT_TRUE(gif.decodeToFramebuffer("image.gif", renderer, config));
    EXPECT_EQ(AllocationTest::failures, fail ? 1u : 0u);
    EXPECT_EQ(frame, expectedFrame(level));
    expectCache(expectedCache(level));
  }
}

TEST_F(GifFramebufferTest, CacheWriteFailureKeepsCompletedRowsAndDropsCache) {
  FileTest::files["image.gif"] = makeGif();
  // Fail after the header and three row writes, leaving a partial pixel batch.
  FileTest::writesUntilFailure = 5;
  GifToFramebufferConverter gif;
  ASSERT_FALSE(gif.decodeToFramebuffer("image.gif", renderer, config));
  EXPECT_EQ(frame, expectedFrame([&](int x, int y) { return dither(canvasGray(x, y), x, y); }, 4));
  EXPECT_FALSE(FileTest::files.contains(config.cachePath));
}
}  // namespace
