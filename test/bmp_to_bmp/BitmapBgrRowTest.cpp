#include <Bitmap.h>
#include <ImageOpsConfig.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#if USE_SIMD_S3_PIE
namespace Rgb24Model {
extern size_t pixels, bgrPixels;
}
#endif

namespace {
using Color = std::array<uint8_t, 3>;  // R, G, B

Color pixel(int x, int y) {
  static constexpr Color PRIMARIES[] = {{255, 0, 0}, {0, 255, 0},     {0, 0, 255},
                                        {0, 0, 0},   {255, 255, 255}, {127, 127, 127}};
  const int index = (x + y) % 11;
  if (index < 6) return PRIMARIES[index];
  return {static_cast<uint8_t>(x * 37 + y * 71), static_cast<uint8_t>(x * 19 + y * 29),
          static_cast<uint8_t>(x * 83 + y * 17)};
}

uint8_t luminance(Color color) {
  return static_cast<uint8_t>((77u * color[0] + 150u * color[1] + 29u * color[2]) / 256);
}

void put32(std::vector<uint8_t>& out, uint32_t value) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(value >> (8 * i)));
}

std::vector<uint8_t> image(int bits, int width, int height, bool topDown) {
  const int stride = (width * bits + 31) / 32 * 4;
  std::vector<uint8_t> bytes{'B', 'M'};
  put32(bytes, 54 + stride * height);
  put32(bytes, 0);
  put32(bytes, 54);
  put32(bytes, 40);
  put32(bytes, width);
  put32(bytes, topDown ? -height : height);
  bytes.insert(bytes.end(), {1, 0, static_cast<uint8_t>(bits), 0});
  put32(bytes, 0);
  put32(bytes, stride * height);
  for (int i = 0; i < 4; ++i) put32(bytes, 0);
  for (int row = 0; row < height; ++row) {
    const int y = topDown ? row : height - 1 - row;
    for (int x = 0; x < width; ++x) {
      const auto color = pixel(x, y);
      bytes.insert(bytes.end(), {color[2], color[1], color[0]});
      if (bits == 32) bytes.push_back(static_cast<uint8_t>(x * 53 + y * 31));
    }
    bytes.insert(bytes.end(), stride - width * (bits / 8), 0xc7);
  }
  return bytes;
}

class BitmapBgrRowTest : public ::testing::Test {
 protected:
  std::filesystem::path path =
      std::filesystem::temp_directory_path() / (std::string("crosspoint-bgr-bmp-") + BMP_ROW_TEST_VARIANT + "-" +
                                                ::testing::UnitTest::GetInstance()->current_test_info()->name());
  HalFile file;

  void save(const std::vector<uint8_t>& bytes) {
    file.close();
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    out.close();
    ASSERT_TRUE(file.open(path.c_str(), "rb"));
  }

  void TearDown() override {
    file.close();
    std::filesystem::remove(path);
  }

  void check(int bits, int width, bool topDown, bool dithering, bool originalThresholds, int phase) {
    constexpr int HEIGHT = 5;
    const auto bytes = image(bits, width, HEIGHT, topDown);
    save(bytes);
    Bitmap bitmap(file, dithering, originalThresholds);
    ASSERT_EQ(bitmap.parseHeaders(), BmpReaderError::Ok);
    AtkinsonDitherer reference(width, originalThresholds);
    ASSERT_TRUE(reference.isValid());
    const int rowBytes = bitmap.getRowBytes(), outputBytes = (width + 3) / 4;
    const int rawPrefix = phase < 0 ? 0 : 16 + phase;
    const int grayPrefix = phase < 0 ? 0 : 16 + (phase + 7) % 16;
    const int outPrefix = phase < 0 ? 0 : 31 - phase;
    const int suffix = phase < 0 ? 0 : 16;
    auto raw = std::make_unique<uint8_t[]>(rawPrefix + rowBytes + suffix);
    auto gray = std::make_unique<uint8_t[]>(grayPrefix + width + suffix);
    auto packed = std::make_unique<uint8_t[]>(outPrefix + outputBytes + suffix);
    auto* input = raw.get() + rawPrefix;
    auto* outputGray = gray.get() + grayPrefix;
    auto* outputPacked = packed.get() + outPrefix;
    std::fill_n(raw.get(), rawPrefix + rowBytes + suffix, 0xa5);
#if USE_SIMD_S3_PIE
    const size_t beforeRgb = Rgb24Model::pixels, beforeBgr = Rgb24Model::bgrPixels;
#endif
    for (int row = 0; row < HEIGHT; ++row) {
      const int grayY = (row + 2) % HEIGHT;
      const size_t position = file.position();
      std::fill_n(gray.get(), grayPrefix + width + suffix, 0xa5);
      ASSERT_EQ(bitmap.readRowGray(grayY, outputGray, input), BmpReaderError::Ok);
      for (int x = 0; x < width; ++x) ASSERT_EQ(outputGray[x], luminance(pixel(x, grayY)));
      const int fileRow = topDown ? grayY : HEIGHT - 1 - grayY;
      EXPECT_TRUE(std::equal(input, input + rowBytes, bytes.data() + 54 + fileRow * rowBytes));
      EXPECT_EQ(file.position(), 54u + (fileRow + 1) * rowBytes);
      EXPECT_TRUE(std::all_of(gray.get(), outputGray, [](uint8_t v) { return v == 0xa5; }));
      EXPECT_TRUE(std::all_of(outputGray + width, outputGray + width + suffix, [](uint8_t v) { return v == 0xa5; }));
      ASSERT_TRUE(file.seek(position));
      std::fill_n(packed.get(), outPrefix + outputBytes + suffix, 0xa5);
      ASSERT_EQ(bitmap.readNextRow(outputPacked, input), BmpReaderError::Ok);
      std::vector<uint8_t> expected(outputBytes);
      const int y = topDown ? row : HEIGHT - 1 - row;
      for (int x = 0; x < width; ++x) {
        const int grayValue = adjustPixel(luminance(pixel(x, y)));
        const auto level = dithering ? reference.processPixel(grayValue, x) : quantize(grayValue, x, row);
        expected[x / 4] |= level << (6 - (x % 4) * 2);
      }
      if (dithering) reference.nextRow();
      ASSERT_TRUE(std::equal(expected.begin(), expected.end(), outputPacked))
          << bits << "bpp width=" << width << " row=" << row << " phase=" << phase;
      EXPECT_TRUE(std::equal(input, input + rowBytes, bytes.data() + 54 + row * rowBytes));
      EXPECT_TRUE(std::all_of(packed.get(), outputPacked, [](uint8_t v) { return v == 0xa5; }));
      EXPECT_TRUE(std::all_of(outputPacked + outputBytes, outputPacked + outputBytes + suffix,
                              [](uint8_t v) { return v == 0xa5; }));
      EXPECT_TRUE(std::all_of(raw.get(), input, [](uint8_t v) { return v == 0xa5; }));
      EXPECT_TRUE(std::all_of(input + rowBytes, input + rowBytes + suffix, [](uint8_t v) { return v == 0xa5; }));
      EXPECT_EQ(file.position(), 54u + (row + 1) * rowBytes);
    }
#if USE_SIMD_S3_PIE
    EXPECT_EQ(Rgb24Model::bgrPixels - beforeBgr, bits == 24 ? 2 * HEIGHT * static_cast<size_t>(width & ~7) : 0);
    EXPECT_EQ(Rgb24Model::pixels, beforeRgb);
#endif
  }
};

#ifdef BMP_ROW_TEST_FULL
TEST_F(BitmapBgrRowTest, GrayAndPackedRowsOrderDitheringBoundsAndAlpha) {
  for (int bits : {24, 32}) {
    for (int width :
         {1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 64, 65, 127, 128, 129, 255, 256, 257, 480, 800, 2048}) {
      for (bool topDown : {false, true}) {
        for (bool dithering : {false, true}) {
          for (bool originalThresholds : {false, true}) {
            for (int phase = -1; phase < 16; ++phase) check(bits, width, topDown, dithering, originalThresholds, phase);
          }
        }
      }
    }
  }
}

TEST_F(BitmapBgrRowTest, FailuresLeaveOutputUntouched) {
  for (int bits : {24, 32}) {
    auto bytes = image(bits, 129, 1, true);
    bytes.pop_back();
    save(bytes);
    Bitmap bitmap(file, true);
    ASSERT_EQ(bitmap.parseHeaders(), BmpReaderError::Ok);
    std::vector<uint8_t> raw(bitmap.getRowBytes()), gray(129, 0xa5), packed((129 + 3) / 4, 0xa5);
    const auto beforeGray = gray, beforePacked = packed;
    EXPECT_EQ(bitmap.readRowGray(0, gray.data(), raw.data()), BmpReaderError::ShortReadRow);
    EXPECT_EQ(gray, beforeGray);
    ASSERT_EQ(bitmap.rewindToData(), BmpReaderError::Ok);
    EXPECT_EQ(bitmap.readNextRow(packed.data(), raw.data()), BmpReaderError::ShortReadRow);
    EXPECT_EQ(packed, beforePacked);
    file.close();
    EXPECT_EQ(bitmap.readRowGray(0, gray.data(), raw.data()), BmpReaderError::SeekPixelDataFailed);
    EXPECT_EQ(gray, beforeGray);
  }
}
#else
TEST_F(BitmapBgrRowTest, ColorRowsWithPaddingAndTail) {
  check(24, 129, false, true, true, 1);
  check(32, 9, true, false, false, -1);
}
#endif
}  // namespace
