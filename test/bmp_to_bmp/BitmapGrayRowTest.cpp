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
namespace IndexedModel {
extern size_t pixels[3];
}
#endif

namespace {
using Color = std::array<uint8_t, 3>;  // R, G, B
using Palette = std::vector<Color>;

void put32(std::vector<uint8_t>& out, uint32_t value) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(value >> (8 * i)));
}

uint8_t sourceByte(int y, int byte) { return static_cast<uint8_t>(y * 41 + byte * 73 + byte / 2); }

unsigned indexAt(int bits, int y, int x) {
  unsigned index = 0;
  for (int bit = 0; bit < bits; ++bit) {
    const int position = x * bits + bit;
    index = index * 2 + ((sourceByte(y, position / 8) & (128 >> (position % 8))) != 0);
  }
  return index;
}

uint8_t luminance(const Color& color) {
  return static_cast<uint8_t>((77u * color[0] + 150u * color[1] + 29u * color[2]) / 256);
}

Palette makePalette(int bits, bool shortPalette) {
  Palette palette;
  const int count = shortPalette ? 1 : 1 << bits;
  palette.reserve(count);
  for (int i = 0; i < count; ++i)
    palette.push_back(
        {static_cast<uint8_t>(255 - i * 37), static_cast<uint8_t>(i * 73 + 9), static_cast<uint8_t>(i * 19 + 141)});
  return palette;
}

std::vector<uint8_t> image(int bits, int width, int height, bool topDown, const Palette& palette) {
  const int stride = (width * bits + 31) / 32 * 4;
  const unsigned offset = 54 + palette.size() * 4;
  std::vector<uint8_t> bytes{'B', 'M'};
  put32(bytes, offset + stride * height);
  put32(bytes, 0);
  put32(bytes, offset);
  put32(bytes, 40);
  put32(bytes, width);
  put32(bytes, topDown ? -height : height);
  bytes.insert(bytes.end(), {1, 0, static_cast<uint8_t>(bits), 0});
  put32(bytes, 0);
  put32(bytes, stride * height);
  put32(bytes, 0);
  put32(bytes, 0);
  put32(bytes, palette.size());
  put32(bytes, 0);
  for (const auto& color : palette) bytes.insert(bytes.end(), {color[2], color[1], color[0], 0});
  for (int row = 0; row < height; ++row) {
    const int y = topDown ? row : height - 1 - row;
    for (int byte = 0; byte < stride; ++byte) bytes.push_back(sourceByte(y, byte));
  }
  return bytes;
}

class BitmapGrayRowTest : public ::testing::Test {
 protected:
  std::filesystem::path path =
      std::filesystem::temp_directory_path() / (std::string("crosspoint-gray-bmp-") + BMP_ROW_TEST_VARIANT + "-" +
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

  void check(int bits, int width, bool topDown, const Palette& palette, int height = 3) {
    save(image(bits, width, height, topDown, palette));
    Bitmap bitmap(file, true);
    ASSERT_EQ(bitmap.parseHeaders(), BmpReaderError::Ok);
    for (int phase = 0; phase < 16; ++phase) {
      std::vector<uint8_t> raw(bitmap.getRowBytes() + 48, 0xa5);
      std::vector<uint8_t> gray(width + 48, 0xa5);
      auto* input = raw.data() + 16 + phase;
      auto* output = gray.data() + 16 + (15 - phase);
      // Read in reverse display order, including repeated random-access reads.
      for (int y = height - 1; y >= 0; --y) {
        std::fill(gray.begin(), gray.end(), 0xa5);
        ASSERT_EQ(bitmap.readRowGray(y, output, input), BmpReaderError::Ok);
        const auto preserved = raw;
        for (int x = 0; x < width; ++x) {
          const unsigned index = indexAt(bits, y, x);
          // Unspecified palette entries retain their index luminance.
          const unsigned expected = index < palette.size() ? luminance(palette[index]) : index;
          ASSERT_EQ(output[x], expected) << bits << "bpp width=" << width << " x=" << x << " y=" << y;
        }
        EXPECT_TRUE(std::all_of(gray.data(), output, [](uint8_t v) { return v == 0xa5; }));
        EXPECT_TRUE(std::all_of(output + width, gray.data() + gray.size(), [](uint8_t v) { return v == 0xa5; }));
        EXPECT_TRUE(std::all_of(raw.data(), input, [](uint8_t v) { return v == 0xa5; }));
        EXPECT_TRUE(
            std::all_of(input + bitmap.getRowBytes(), raw.data() + raw.size(), [](uint8_t v) { return v == 0xa5; }));
        for (int byte = 0; byte < bitmap.getRowBytes(); ++byte) EXPECT_EQ(input[byte], sourceByte(y, byte));
        const size_t fileRow = topDown ? y : height - 1 - y;
        EXPECT_EQ(file.position(), 54 + palette.size() * 4 + (fileRow + 1) * bitmap.getRowBytes());
        ASSERT_EQ(bitmap.readRowGray(y, output, input), BmpReaderError::Ok);
        EXPECT_EQ(raw, preserved);
      }
    }
  }
};

#ifdef BMP_ROW_TEST_FULL
TEST_F(BitmapGrayRowTest, IndexedWidthsPalettesAlignmentAndRowOrder) {
  for (int bits : {1, 2, 4, 8}) {
    for (int width : {1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 64, 65, 127, 128, 129, 480, 800, 2048}) {
      for (bool topDown : {false, true}) {
        for (bool shortPalette : {false, true}) check(bits, width, topDown, makePalette(bits, shortPalette));
      }
    }
  }
}

TEST_F(BitmapGrayRowTest, EveryPackedByte) {
  for (int bits : {1, 2, 4}) check(bits, 128, true, makePalette(bits, false), 256);
}

TEST_F(BitmapGrayRowTest, ExactSizeBuffers) {
  for (int bits : {1, 2, 4, 8}) {
    for (int width : {1, 7, 33, 65, 129, 2048}) {
      const Palette palette = makePalette(bits, false);
      save(image(bits, width, 1, true, palette));
      Bitmap bitmap(file);
      ASSERT_EQ(bitmap.parseHeaders(), BmpReaderError::Ok);
      auto raw = std::make_unique<uint8_t[]>(bitmap.getRowBytes());
      auto gray = std::make_unique<uint8_t[]>(width);
      ASSERT_EQ(bitmap.readRowGray(0, gray.get(), raw.get()), BmpReaderError::Ok);
      for (int x = 0; x < width; ++x) EXPECT_EQ(gray[x], luminance(palette[indexAt(bits, 0, x)]));
    }
  }
}

TEST_F(BitmapGrayRowTest, FailuresLeaveGrayUntouched) {
  for (int bits : {1, 2, 4, 8}) {
    auto bytes = image(bits, 129, 1, true, makePalette(bits, false));
    bytes.pop_back();
    save(bytes);
    Bitmap bitmap(file);
    ASSERT_EQ(bitmap.parseHeaders(), BmpReaderError::Ok);
    std::vector<uint8_t> raw(bitmap.getRowBytes()), gray(129, 0xa5);
    const auto before = gray;
    EXPECT_EQ(bitmap.readRowGray(0, gray.data(), raw.data()), BmpReaderError::ShortReadRow);
    EXPECT_EQ(gray, before);
    file.close();
    EXPECT_EQ(bitmap.readRowGray(0, gray.data(), raw.data()), BmpReaderError::SeekPixelDataFailed);
    EXPECT_EQ(gray, before);
  }
}

TEST_F(BitmapGrayRowTest, LeavesSequentialDitherStateAlone) {
  constexpr int bits = 4, width = 129, height = 3;
  const Palette palette = makePalette(bits, false);
  save(image(bits, width, height, true, palette));
  Bitmap bitmap(file, true);
  ASSERT_EQ(bitmap.parseHeaders(), BmpReaderError::Ok);
  AtkinsonDitherer reference(width, false);
  ASSERT_TRUE(reference.isValid());
  std::vector<uint8_t> raw(bitmap.getRowBytes()), gray(width), packed((width + 3) / 4);
  for (int y = 0; y < height; ++y) {
    const size_t position = file.position();
    ASSERT_EQ(bitmap.readRowGray(height - 1 - y, gray.data(), raw.data()), BmpReaderError::Ok);
    ASSERT_TRUE(file.seek(position));
    ASSERT_EQ(bitmap.readNextRow(packed.data(), raw.data()), BmpReaderError::Ok);
    std::vector<uint8_t> expected(packed.size());
    for (int x = 0; x < width; ++x) {
      const auto level = reference.processPixel(adjustPixel(luminance(palette[indexAt(bits, y, x)])), x);
      expected[x / 4] |= level << (6 - (x % 4) * 2);
    }
    reference.nextRow();
    EXPECT_EQ(packed, expected);
  }
}

#else
TEST_F(BitmapGrayRowTest, ReadsIndexedRowsWithColoredPalettesAndTails) {
  for (int i = 0; i < 4; ++i) {
    const int bits = 1 << i;
#if USE_SIMD_S3_PIE
    const size_t before = i < 3 ? IndexedModel::pixels[i] : 0;
#endif
    check(bits, 129, false, makePalette(bits, bits == 8));
#if USE_SIMD_S3_PIE
    if (i < 3) EXPECT_GT(IndexedModel::pixels[i], before) << bits << "bpp";
#endif
  }
}
#endif
}  // namespace
