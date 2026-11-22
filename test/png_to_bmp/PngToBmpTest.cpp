#include <ImageOpsConfig.h>
#include <PngToBmpConverter.h>
#include <Print.h>
#include <gtest/gtest.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <vector>

namespace {
using Bytes = std::vector<uint8_t>;
constexpr int WIDTH = 129;
constexpr int HEIGHT = 2;
constexpr uint8_t GRAY = 0, RGB = 2, INDEXED = 3;

class Output final : public Print {
 public:
  Bytes bytes;
  size_t limit = SIZE_MAX;
  size_t write(uint8_t value) override { return write(&value, 1); }
  size_t write(const uint8_t* data, size_t count) override {
    count = std::min(count, limit - bytes.size());
    bytes.insert(bytes.end(), data, data + count);
    return count;
  }
};

void appendBe32(Bytes& bytes, uint32_t value) {
  for (int shift : {24, 16, 8, 0}) bytes.push_back(static_cast<uint8_t>(value >> shift));
}

void appendChunk(Bytes& png, const char (&type)[5], std::span<const uint8_t> data) {
  appendBe32(png, static_cast<uint32_t>(data.size()));
  const size_t start = png.size();
  png.insert(png.end(), type, type + 4);
  png.insert(png.end(), data.begin(), data.end());
  appendBe32(png, static_cast<uint32_t>(crc32(0, png.data() + start, 4 + data.size())));
}

// Each input row is packed MSB first, preceded by a PNG "None" filter byte.
Bytes scanlines(std::span<const uint8_t> samples, int depth, int samplesPerRow = WIDTH) {
  const size_t stride = 1 + (samplesPerRow * depth + 7) / 8;
  Bytes raw(stride * HEIGHT);
  for (int y = 0; y < HEIGHT; ++y) {
    for (int x = 0; x < samplesPerRow; ++x) {
      const int bit = x * depth;
      raw[y * stride + 1 + bit / 8] |= samples[y * samplesPerRow + x] << (8 - depth - bit % 8);
    }
  }
  return raw;
}

Bytes makePng(uint8_t depth, uint8_t color, const Bytes& raw, std::span<const uint8_t> palette = {}) {
  Bytes png{137, 'P', 'N', 'G', 13, 10, 26, 10}, header;
  appendBe32(header, WIDTH);
  appendBe32(header, HEIGHT);
  header.insert(header.end(), {depth, color, 0, 0, 0});
  appendChunk(png, "IHDR", header);
  if (!palette.empty()) appendChunk(png, "PLTE", palette);
  uLongf size = compressBound(raw.size());
  Bytes compressed(size);
  if (compress(compressed.data(), &size, raw.data(), raw.size()) != Z_OK) {
    ADD_FAILURE() << "Failed to compress test scanlines";
    return {};
  }
  compressed.resize(size);
  appendChunk(png, "IDAT", compressed);
  appendChunk(png, "IEND", {});
  return png;
}

uint32_t le32(const Bytes& bytes, size_t offset) {
  return uint32_t{bytes[offset]} | uint32_t{bytes[offset + 1]} << 8 | uint32_t{bytes[offset + 2]} << 16 |
         uint32_t{bytes[offset + 3]} << 24;
}

class PngToBmpTest : public ::testing::Test {
 protected:
  std::filesystem::path path = std::filesystem::temp_directory_path() /
                               ("crosspoint-png-to-bmp-" + std::to_string(CROSSPOINT_ACCELERATED_IMAGES) + "-" +
                                ::testing::UnitTest::GetInstance()->current_test_info()->name() + ".png");
  HalFile file;

  void TearDown() override {
    file.close();
    std::filesystem::remove(path);
  }

  void openPng(const Bytes& png) {
    file.close();
    {
      std::ofstream stream(path, std::ios::binary);
      ASSERT_TRUE(stream.write(reinterpret_cast<const char*>(png.data()), png.size()));
    }
    ASSERT_TRUE(file.open(path.c_str(), "rb"));
  }

  void convert(const Bytes& png, Output& output, bool thumbnail) {
    ASSERT_NO_FATAL_FAILURE(openPng(png));
    const uint32_t width = thumbnail ? WIDTH * 2 : WIDTH;
    const uint32_t height = thumbnail ? HEIGHT * 2 : HEIGHT;
    const unsigned depth = thumbnail ? 1 : 2;
    if (thumbnail) {
      ASSERT_TRUE(PngToBmpConverter::pngFileTo1BitBmpStreamWithSize(file, output, width, height));
    } else {
      ASSERT_TRUE(PngToBmpConverter::pngFileToBmpStream(file, output, false));
    }
    const Bytes& bmp = output.bytes;
    const uint32_t offset = 54 + 4 * (1u << depth);
    ASSERT_EQ(bmp.size(), offset + ((width * depth + 31) / 32) * 4 * height);
    EXPECT_EQ(bmp[0], 'B');
    EXPECT_EQ(bmp[1], 'M');
    EXPECT_EQ(le32(bmp, 2), bmp.size());
    EXPECT_EQ(le32(bmp, 10), offset);
    EXPECT_EQ(le32(bmp, 18), width);
    EXPECT_EQ(le32(bmp, 22), uint32_t{0} - height);  // Top-down BMP.
    EXPECT_EQ(bmp[28], depth);
  }

  void expectSameBmp(const Bytes& png, const Bytes& graySamples, bool thumbnail = false) {
    Output actual, expected;
    ASSERT_NO_FATAL_FAILURE(convert(png, actual, thumbnail));
    ASSERT_NO_FATAL_FAILURE(convert(makePng(8, GRAY, scanlines(graySamples, 8)), expected, thumbnail));
    EXPECT_EQ(actual.bytes, expected.bytes);
  }
};

TEST_F(PngToBmpTest, RgbMatchesKnownGrayscaleLuminance) {
  constexpr uint8_t colors[][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {0, 0, 0}, {255, 255, 255}};
  constexpr std::array<uint8_t, 5> luminance{76, 149, 28, 0, 255};
  Bytes rgb, gray;
  for (int i = 0; i < WIDTH * HEIGHT; ++i) {
    const size_t color = i % luminance.size();
    rgb.insert(rgb.end(), std::begin(colors[color]), std::end(colors[color]));
    gray.push_back(luminance[color]);
  }
  expectSameBmp(makePng(8, RGB, scanlines(rgb, 8, WIDTH * 3)), gray);
}

TEST_F(PngToBmpTest, ExpandsPackedGrayscaleForCoversAndThumbnails) {
  for (const int depth : {1, 2, 4}) {
    SCOPED_TRACE(depth);
    const int levels = 1 << depth;
    Bytes samples, gray;
    for (int i = 0; i < WIDTH * HEIGHT; ++i) {
      const int shade = i % levels;
      samples.push_back(shade);
      gray.push_back(shade * 255 / (levels - 1));
    }
    const auto png = makePng(depth, GRAY, scanlines(samples, depth));
    expectSameBmp(png, gray);
    if (depth == 2) expectSameBmp(png, gray, true);
  }
}

TEST_F(PngToBmpTest, ResolvesShortPaletteAndFallsBackToFirstEntry) {
  constexpr std::array<uint8_t, 9> palette{255, 255, 255, 0, 0, 255, 255, 0, 0};
  constexpr std::array<uint8_t, 4> indices{0, 1, 2, 15}, luminance{255, 28, 76, 255};
  Bytes samples, gray;
  for (int i = 0; i < WIDTH * HEIGHT; ++i) {
    samples.push_back(indices[i % indices.size()]);
    gray.push_back(luminance[i % luminance.size()]);
  }
  expectSameBmp(makePng(4, INDEXED, scanlines(samples, 4), palette), gray);
}

TEST_F(PngToBmpTest, RejectsUnsupportedDepthBeforeWritingBmp) {
  constexpr int depth = 3;
  constexpr std::array<uint8_t, 6> palette{0, 0, 0, 255, 255, 255};
  const Bytes raw((1 + (WIDTH * depth + 7) / 8) * HEIGHT);
  for (const auto color : {GRAY, INDEXED}) {
    SCOPED_TRACE(static_cast<int>(color));
    const std::span<const uint8_t> entries =
        color == INDEXED ? std::span<const uint8_t>{palette} : std::span<const uint8_t>{};
    ASSERT_NO_FATAL_FAILURE(openPng(makePng(depth, color, raw, entries)));
    Output output;
    EXPECT_FALSE(PngToBmpConverter::pngFileToBmpStream(file, output, false));
    EXPECT_TRUE(output.bytes.empty());
  }
}

TEST_F(PngToBmpTest, PropagatesSharedWriterHeaderAndRowFailures) {
  const auto png = makePng(8, GRAY, scanlines(Bytes(WIDTH * HEIGHT, 149), 8));
  constexpr size_t FIRST_ROW_END = 70 + ((WIDTH * 2 + 31) / 32) * 4;
  for (const size_t limit : {size_t{8}, FIRST_ROW_END}) {
    SCOPED_TRACE(limit);
    ASSERT_NO_FATAL_FAILURE(openPng(png));
    Output output;
    output.limit = limit;
    EXPECT_FALSE(PngToBmpConverter::pngFileToBmpStream(file, output, false));
    EXPECT_EQ(output.bytes.size(), limit);
  }
}
}  // namespace
