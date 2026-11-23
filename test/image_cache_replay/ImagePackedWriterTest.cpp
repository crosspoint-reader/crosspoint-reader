#include <Bitmap.h>
#include <ImagePackedWriter.h>

#include "Epub/converters/DirectPixelWriter.h"
#include "ImageTest.h"

namespace {
class ImagePackedWriterTest : public ImageTest {
 protected:
  static constexpr int BMP_HEIGHT = 13;

  static int levelAt(int x, int y) { return (x + 2 * y + x / 3 + y / 4) & 3; }

  static void appendLe32(Bytes& bytes, uint32_t value) {
    for (int shift : {0, 8, 16, 24}) bytes.push_back(static_cast<uint8_t>(value >> shift));
  }

  static Bytes makeBmp(int width, bool topDown, int completeRows = BMP_HEIGHT) {
    const int rowBytes = (width * 2 + 31) / 32 * 4;
    Bytes bytes{'B', 'M'};
    appendLe32(bytes, 70 + rowBytes * BMP_HEIGHT);
    appendLe32(bytes, 0);
    appendLe32(bytes, 70);
    appendLe32(bytes, 40);
    appendLe32(bytes, width);
    appendLe32(bytes, topDown ? -BMP_HEIGHT : BMP_HEIGHT);
    bytes.insert(bytes.end(), {1, 0, 2, 0});
    for (int i = 0; i < 6; ++i) appendLe32(bytes, i == 4 ? 4 : 0);
    for (uint8_t gray : {0, 85, 170, 255}) bytes.insert(bytes.end(), {gray, gray, gray, 0});
    for (int row = 0; row < completeRows; ++row) {
      const int y = topDown ? row : BMP_HEIGHT - 1 - row;
      Bytes packed(rowBytes);
      for (int x = 0; x < width; ++x) packed[x / 4] |= levelAt(x, y) << (6 - 2 * (x % 4));
      bytes.insert(bytes.end(), packed.begin(), packed.end());
    }
    if (completeRows < BMP_HEIGHT) bytes.insert(bytes.end(), rowBytes - 1, 0);  // Short final row.
    return bytes;
  }

  glyphBitmap::Target target() const {
    const bool inverted = renderer.orientation == GfxRenderer::PortraitInverted;
    return {renderer.target,
            renderer.physicalWidth,
            renderer.getDisplayWidthBytes(),
            renderer.originY,
            renderer.rows,
            {inverted ? renderer.physicalWidth - 1 : 0, inverted ? config.x : renderer.physicalHeight - 1 - config.x, 0,
             inverted ? 1 : -1, inverted ? -1 : 1, 0}};
  }

  glyphBitmap::Plane plane() const {
    switch (renderer.mode) {
      case GfxRenderer::BW:
        return glyphBitmap::Plane::BW;
      case GfxRenderer::GRAYSCALE_LSB:
        return glyphBitmap::Plane::GrayLSB;
      case GfxRenderer::GRAYSCALE_MSB:
        return glyphBitmap::Plane::GrayMSB;
    }
    return glyphBitmap::Plane::BW;
  }

  void checkBmp(bool topDown, bool transparent, int completeRows = BMP_HEIGHT, bool oom = false) {
    const int width = config.maxWidth;
    const int first = width > 7 ? 3 : 0;
    const int end = width > 7 ? width - 2 : width;
    const int top = config.y + 1, bottom = config.y + BMP_HEIGHT - 2;
    FileTest::files["writer.bmp"] = makeBmp(width, topDown, completeRows);
    HalFile file;
    ASSERT_TRUE(Storage.openFileForRead("BMP", "writer.bmp", file));
    Bitmap bitmap(file);
    ASSERT_EQ(bitmap.parseHeaders(), BmpReaderError::Ok);
    const auto expected = expectedFrame([&](int x, int y) {
      const int fileRow = topDown ? y : BMP_HEIGHT - 1 - y;
      const int level = levelAt(x, y);
      return fileRow >= completeRows || x < first || x >= end || config.y + y < top || config.y + y >= bottom ||
                     (transparent && level == 3)
                 ? -1
                 : level;
    });
    Bytes packed((width + 3) / 4), raw(bitmap.getRowBytes());
    AllocationTest::failBytes = oom ? packed.size() * 8 : 0;
    AllocationTest::failures = 0;
    {
      imagePacked::Writer writer(target(), width, first, end, top, bottom, plane(), renderer.absolute, transparent,
                                 true);
      bool hasColumns = false;
      for (int x = first; x < end; ++x) {
        const int physicalY = renderer.orientation == GfxRenderer::PortraitInverted
                                  ? config.x + x
                                  : renderer.physicalHeight - 1 - config.x - x;
        hasColumns |= physicalY >= renderer.originY && physicalY < renderer.originY + renderer.rows;
      }
      EXPECT_EQ(writer.isActive(), hasColumns && !oom);
      EXPECT_EQ(AllocationTest::failures, oom ? 1u : 0u);
      for (int row = 0; row < BMP_HEIGHT; ++row) {
        if (row == completeRows) {
          const auto before = packed;
          ASSERT_EQ(bitmap.readNextRow(packed.data(), raw.data()), BmpReaderError::ShortReadRow);
          EXPECT_EQ(packed, before);
          break;  // Destruction must publish the completed, still-pending rows.
        }
        ASSERT_EQ(bitmap.readNextRow(packed.data(), raw.data()), BmpReaderError::Ok);
        const int y = topDown ? row : BMP_HEIGHT - 1 - row;
        if (!writer.writeRow(packed.data(), config.y + y)) {
          // Inactive writers leave immediate scalar output to their caller.
          DirectPixelWriter pixels;
          pixels.init(renderer);
          pixels.beginRow(config.y + y);
          for (int x = first; x < end; ++x) {
            if (config.x + x < 0 || config.x + x >= renderer.getScreenWidth() || config.y + y < top ||
                config.y + y >= bottom)
              continue;
            const int level = (packed[x / 4] >> (6 - 2 * (x % 4))) & 3;
            if (!transparent || level != 3) pixels.writePixel(config.x + x, level);
          }
        }
        // Reuse both decoder buffers immediately, before the writer necessarily flushes.
        std::fill(packed.begin(), packed.end(), 0x69);
        std::fill(raw.begin(), raw.end(), 0x96);
      }
      if (completeRows == BMP_HEIGHT) {
        writer.flush();
        EXPECT_EQ(frame, expected);
        writer.flush();
        EXPECT_EQ(frame, expected);
      } else {
        EXPECT_EQ(frame, initialFrame());
        EXPECT_NE(expected, initialFrame());
      }
    }
    EXPECT_EQ(frame, expected);
    AllocationTest::failBytes = 0;
  }
};

TEST_F(ImagePackedWriterTest, BmpRowsMatchScalarAcrossOrdersPlanesTransparencyAndClipping) {
  config.maxHeight = BMP_HEIGHT;
  for (int width : {1, 7, 9, 17}) {
    config.maxWidth = width;
    for (const auto orientation : {GfxRenderer::Portrait, GfxRenderer::PortraitInverted}) {
      renderer.orientation = orientation;
      for (int x : {-2, 5}) {
        config.x = x;
        for (bool strip : {false, true}) {
          renderer.originY = strip ? 7 : 0;
          renderer.rows = strip ? 19 : renderer.physicalHeight;
          for (const auto mode : {GfxRenderer::BW, GfxRenderer::GRAYSCALE_LSB, GfxRenderer::GRAYSCALE_MSB}) {
            renderer.mode = mode;
            for (bool absolute : {false, true}) {
              renderer.absolute = absolute;
              for (bool topDown : {false, true}) {
                for (bool transparent : {false, true}) {
                  SCOPED_TRACE(::testing::Message()
                               << "width=" << width << " orientation=" << orientation << " x=" << x
                               << " strip=" << strip << " mode=" << mode << " absolute=" << absolute
                               << " topDown=" << topDown << " transparent=" << transparent);
                  resetFrame();
                  ASSERT_NO_FATAL_FAILURE(checkBmp(topDown, transparent));
                }
              }
            }
          }
        }
      }
    }
  }
}

TEST_F(ImagePackedWriterTest, ShortReadFlushesCompletedRowsOnDestruction) {
  config.maxWidth = 17;
  config.maxHeight = BMP_HEIGHT;
  for (const auto orientation : {GfxRenderer::Portrait, GfxRenderer::PortraitInverted}) {
    renderer.orientation = orientation;
    for (bool topDown : {false, true}) {
      resetFrame();
      ASSERT_NO_FATAL_FAILURE(checkBmp(topDown, false, 4));
    }
  }
}

TEST_F(ImagePackedWriterTest, ScratchOomLeavesRowsForScalarFallback) {
  config.maxWidth = 17;
  config.maxHeight = BMP_HEIGHT;
  for (bool topDown : {false, true}) {
    resetFrame();
    ASSERT_NO_FATAL_FAILURE(checkBmp(topDown, false, BMP_HEIGHT, true));
  }
}

TEST_F(ImagePackedWriterTest, PendingRowsAreInvisibleUntilFlushAndRepeatedFlushIsHarmless) {
  config.maxWidth = 9;
  config.maxHeight = 2;
  Bytes row((config.maxWidth + 3) / 4, 0x1b);
  const auto before = frame;
  {
    imagePacked::Writer writer(target(), config.maxWidth, 0, config.maxWidth, 0, renderer.getScreenHeight(), plane(),
                               renderer.absolute, false, true);
    ASSERT_TRUE(writer.isActive());
    ASSERT_TRUE(writer.writeRow(row.data(), config.y));
    EXPECT_EQ(frame, before);
    std::fill(row.begin(), row.end(), 0xff);
    writer.flush();
    EXPECT_EQ(frame, expectedFrame([](int x, int y) { return y == 0 ? x % 4 : -1; }));
    const auto flushed = frame;
    writer.flush();
    EXPECT_EQ(frame, flushed);
  }
  EXPECT_EQ(frame, expectedFrame([](int x, int y) { return y == 0 ? x % 4 : -1; }));
}
}  // namespace
