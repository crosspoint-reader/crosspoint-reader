#include <Bitmap.h>
#include <ImageOpsConfig.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#if defined(BMP_ROW_TEST_FULL) || CONFIG_IDF_TARGET_ESP32S3
static_assert(CROSSPOINT_ACCELERATED_IMAGES == 1);
#else
static_assert(CROSSPOINT_ACCELERATED_IMAGES == 0);
#endif

#if USE_SIMD_S3_PIE
namespace IndexedModel {
extern size_t pixels[3];
}
#endif

namespace {
using Palette = std::vector<uint8_t>;
const Palette NON_NATIVE_PALETTE{0, 17, 34, 51, 68, 85, 102, 119, 136, 153, 170, 187, 204, 221, 238, 255};

void put32(std::vector<uint8_t>& out, uint32_t value) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(value >> (i * 8)));
}

std::vector<uint8_t> image(int bits, int width, int height, bool topDown, const Palette& palette) {
  const int stride = (width * bits + 31) / 32 * 4;
  const unsigned offset = 54 + palette.size() * 4;
  std::vector<uint8_t> out{'B', 'M'};
  put32(out, offset + stride * height);
  put32(out, 0);
  put32(out, offset);
  put32(out, 40);
  put32(out, width);
  put32(out, topDown ? -height : height);
  out.insert(out.end(), {1, 0, static_cast<uint8_t>(bits), 0});
  put32(out, 0);
  put32(out, stride * height);
  put32(out, 0);
  put32(out, 0);
  put32(out, palette.size());
  put32(out, 0);
  for (auto gray : palette) out.insert(out.end(), {gray, gray, gray, 0});
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < stride; ++x) out.push_back(static_cast<uint8_t>(y + x * 73));
  }
  return out;
}

class BitmapPackedRowTest : public ::testing::Test {
 protected:
  std::filesystem::path path =
      std::filesystem::temp_directory_path() / (std::string("crosspoint-packed-bmp-") + BMP_ROW_TEST_VARIANT + "-" +
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

  void check(int bits, int width, int height, bool topDown, const Palette& palette, bool dithering) {
    const auto bytes = image(bits, width, height, topDown, palette);
    save(bytes);
    Bitmap bitmap(file, dithering);
    ASSERT_EQ(bitmap.parseHeaders(), BmpReaderError::Ok);
    const int outputBytes = (width + 3) / 4;
    const int offset = 54 + palette.size() * 4;
    std::vector<uint8_t> raw(bitmap.getRowBytes() + 16, 0xa5);
    std::vector<uint8_t> output(outputBytes + 32, 0xa5);
    for (int pass = 0; pass < 2; ++pass) {
      if (pass) ASSERT_EQ(bitmap.rewindToData(), BmpReaderError::Ok);
      for (int row = 0; row < height; ++row) {
        std::fill(output.begin(), output.end(), 0xa5);
        ASSERT_EQ(bitmap.readNextRow(output.data() + 16, raw.data()), BmpReaderError::Ok);
        std::vector<uint8_t> expected(outputBytes);
        for (int x = 0; x < width; ++x) {
          const uint8_t source = bytes[offset + row * bitmap.getRowBytes() + x * bits / 8];
          const unsigned index = (source >> (8 - bits - (x * bits % 8))) & ((1u << bits) - 1);
          // parseHeaders initializes unspecified palette entries to their index.
          const unsigned gray = index < palette.size() ? palette[index] : index;
          expected[x / 4] |= (adjustPixel(gray) >> 6) << (6 - (x % 4) * 2);
        }
        ASSERT_TRUE(std::equal(expected.begin(), expected.end(), output.begin() + 16));
        ASSERT_TRUE(std::all_of(output.begin(), output.begin() + 16, [](uint8_t v) { return v == 0xa5; }));
        ASSERT_TRUE(std::all_of(output.end() - 16, output.end(), [](uint8_t v) { return v == 0xa5; }));
        ASSERT_TRUE(std::all_of(raw.end() - 16, raw.end(), [](uint8_t v) { return v == 0xa5; }));
      }
      const auto before = output;
      EXPECT_EQ(bitmap.readNextRow(output.data() + 16, raw.data()), BmpReaderError::ShortReadRow);
      EXPECT_EQ(output, before);
    }
  }

  void checkProcessed(int bits, int width, bool topDown, bool dithering, bool originalThresholds, int phase) {
    constexpr int HEIGHT = 5;
    const auto& palette = NON_NATIVE_PALETTE;
    const auto bytes = image(bits, width, HEIGHT, topDown, palette);
    save(bytes);
    Bitmap bitmap(file, dithering, originalThresholds);
    ASSERT_EQ(bitmap.parseHeaders(), BmpReaderError::Ok);
    AtkinsonDitherer reference(width, originalThresholds);
    ASSERT_TRUE(reference.isValid());
    const int rowBytes = bitmap.getRowBytes(), outputBytes = (width + 3) / 4;
    const int rawPrefix = phase < 0 ? 0 : 16 + phase;
    const int outPrefix = phase < 0 ? 0 : 31 - phase;
    const int suffix = phase < 0 ? 0 : 16;
    auto raw = std::make_unique<uint8_t[]>(rawPrefix + rowBytes + suffix);
    auto output = std::make_unique<uint8_t[]>(outPrefix + outputBytes + suffix);
    auto* input = raw.get() + rawPrefix;
    auto* packed = output.get() + outPrefix;
    std::fill_n(raw.get(), rawPrefix + rowBytes + suffix, 0xa5);
    const size_t offset = 54 + palette.size() * 4;
    for (int pass = 0; pass < 2; ++pass) {
      if (pass) {
        ASSERT_EQ(bitmap.rewindToData(), BmpReaderError::Ok);
        reference.reset();
      }
      for (int row = 0; row < HEIGHT; ++row) {
        std::fill_n(output.get(), outPrefix + outputBytes + suffix, 0xa5);
        ASSERT_EQ(bitmap.readNextRow(packed, input), BmpReaderError::Ok);
        std::vector<uint8_t> expected(outputBytes);
        for (int x = 0; x < width; ++x) {
          unsigned index = 0;
          for (int bit = 0; bit < bits; ++bit) {
            const int position = x * bits + bit;
            index = index * 2 + ((bytes[offset + row * rowBytes + position / 8] & (128 >> (position % 8))) != 0);
          }
          const int gray = index < palette.size() ? palette[index] : index;
          const auto level = dithering ? reference.processPixel(adjustPixel(gray), x)
                                       : quantize(adjustPixel(gray), x, pass * HEIGHT + row);
          expected[x / 4] |= level << (6 - (x % 4) * 2);
        }
        if (dithering) reference.nextRow();
        ASSERT_TRUE(std::equal(expected.begin(), expected.end(), packed))
            << bits << "bpp width=" << width << " row=" << row << " phase=" << phase;
        EXPECT_TRUE(std::equal(input, input + rowBytes, bytes.data() + offset + row * rowBytes));
        EXPECT_TRUE(std::all_of(output.get(), packed, [](uint8_t v) { return v == 0xa5; }));
        EXPECT_TRUE(
            std::all_of(packed + outputBytes, packed + outputBytes + suffix, [](uint8_t v) { return v == 0xa5; }));
        EXPECT_TRUE(std::all_of(raw.get(), input, [](uint8_t v) { return v == 0xa5; }));
        EXPECT_TRUE(std::all_of(input + rowBytes, input + rowBytes + suffix, [](uint8_t v) { return v == 0xa5; }));
        EXPECT_EQ(file.position(), offset + (row + 1) * rowBytes);
      }
      std::fill_n(packed, outputBytes, 0xa5);
      EXPECT_EQ(bitmap.readNextRow(packed, input), BmpReaderError::ShortReadRow);
      EXPECT_TRUE(std::all_of(packed, packed + outputBytes, [](uint8_t v) { return v == 0xa5; }));
    }
  }
};

#ifdef BMP_ROW_TEST_FULL
TEST_F(BitmapPackedRowTest, EveryByteAndPaletteLevelMapping) {
  for (bool dithering : {false, true}) {
    for (int mapping = 0; mapping < 16; ++mapping) {
      check(1, 8, 256, false, {static_cast<uint8_t>((mapping & 3) * 85), static_cast<uint8_t>((mapping >> 2) * 85)},
            dithering);
    }
    for (int mapping = 0; mapping < 256; ++mapping) {
      Palette palette;
      for (int i = 0; i < 4; ++i) palette.push_back(((mapping >> (i * 2)) & 3) * 85);
      check(2, 4, 256, true, palette, dithering);
    }
  }
}

TEST_F(BitmapPackedRowTest, WidthsTailsPaddingRowOrderAndRewind) {
  for (int bits : {1, 2, 4}) {
    const std::vector<Palette> palettes =
        bits == 1
            ? std::vector<Palette>{{0, 255}, {255, 0}, {90, 180}, {255, 255}, {7}}
            : std::vector<Palette>{{0, 85, 170, 255}, {21, 90, 180, 244}, {255, 170, 85, 0}, {0, 0, 255, 255}, {85}};
    for (int width : {1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 32, 33, 127, 128, 129, 480, 800, 2048}) {
      for (bool topDown : {false, true}) {
        for (bool dithering : {false, true}) {
          for (const auto& palette : palettes) check(bits, width, 17, topDown, palette, dithering);
        }
      }
    }
  }
}

TEST_F(BitmapPackedRowTest, FourBitDitheringQuantizationChunksAndBounds) {
  for (int width :
       {1, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 64, 65, 127, 128, 129, 255, 256, 257, 480, 800, 2048}) {
    for (bool topDown : {false, true}) {
      for (bool dithering : {false, true}) {
        for (bool originalThresholds : {false, true}) {
          for (int phase = -1; phase < 16; ++phase)
            checkProcessed(4, width, topDown, dithering, originalThresholds, phase);
        }
      }
    }
  }
}

TEST_F(BitmapPackedRowTest, EightBitControl) {
  for (bool dithering : {false, true}) checkProcessed(8, 129, false, dithering, false, -1);
}

TEST_F(BitmapPackedRowTest, ShortReadDoesNotAdvanceDithering) {
  constexpr int width = 129, height = 3;
  const auto& palette = NON_NATIVE_PALETTE;
  auto truncated = image(4, width, height, true, palette);
  truncated.pop_back();
  save(truncated);
  Bitmap bitmap(file, true);
  ASSERT_EQ(bitmap.parseHeaders(), BmpReaderError::Ok);
  AtkinsonDitherer reference(width, false);
  ASSERT_TRUE(reference.isValid());
  std::vector<uint8_t> raw(bitmap.getRowBytes()), output((width + 3) / 4);
  for (int row = 0; row < height - 1; ++row) {
    if (row == 1) {
      const auto before = output;
      const auto position = file.position();
      ASSERT_TRUE(file.seek(position + bitmap.getRowBytes()));  // Try the truncated row, then resume the intact row.
      EXPECT_EQ(bitmap.readNextRow(output.data(), raw.data()), BmpReaderError::ShortReadRow);
      EXPECT_EQ(output, before);
      ASSERT_TRUE(file.seek(position));
    }
    ASSERT_EQ(bitmap.readNextRow(output.data(), raw.data()), BmpReaderError::Ok);
    std::vector<uint8_t> expected(output.size());
    for (int x = 0; x < width; ++x) {
      const unsigned index = (raw[x / 2] >> (x % 2 ? 0 : 4)) & 15;
      const auto level = reference.processPixel(adjustPixel(palette[index]), x);
      expected[x / 4] |= level << (6 - (x % 4) * 2);
    }
    reference.nextRow();
    EXPECT_EQ(output, expected);
  }
}

#else
TEST_F(BitmapPackedRowTest, NativeAndFallbackRowsWithTailsAndRewind) {
#if USE_SIMD_S3_PIE
  const auto before1 = IndexedModel::pixels[0], before2 = IndexedModel::pixels[1];
#endif
  check(1, 257, 3, false, {90, 180}, true);
  check(2, 257, 3, true, {0, 85, 170, 255}, true);
#if USE_SIMD_S3_PIE
  EXPECT_EQ(IndexedModel::pixels[0], before1);
  EXPECT_EQ(IndexedModel::pixels[1], before2);
  const auto before4 = IndexedModel::pixels[2];
#endif
  check(2, 257, 3, false, {255, 170, 85, 0}, true);
  checkProcessed(4, 129, true, true, false, 1);
#if USE_SIMD_S3_PIE
  EXPECT_GT(IndexedModel::pixels[1], before2);
  EXPECT_GT(IndexedModel::pixels[2], before4);
#endif
}
#endif
}  // namespace
