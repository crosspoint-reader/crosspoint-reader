#include <Bitmap.h>
#include <BmpToBmpConverter.h>
#include <HalStorage.h>
#include <Print.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <tuple>
#include <vector>

#include "test/ScopedTempDir.h"

namespace {

class Output final : public Print {
 public:
  std::vector<uint8_t> bytes;
  size_t write(uint8_t value) override { return write(&value, 1); }
  size_t write(const uint8_t* data, size_t count) override {
    bytes.insert(bytes.end(), data, data + count);
    return count;
  }
};

uint32_t le32(const std::vector<uint8_t>& b, size_t at) {
  return uint32_t{b[at]} | uint32_t{b[at + 1]} << 8 | uint32_t{b[at + 2]} << 16 | uint32_t{b[at + 3]} << 24;
}

void put32(std::vector<uint8_t>& b, uint32_t v) {
  for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

// 24bpp or 1bpp BMP whose top (or left) half is black and the other half white.
std::vector<uint8_t> splitBmp(int width, int height, bool topDown, bool oneBit, bool leftRight = false) {
  const int bits = oneBit ? 1 : 24;
  const int rowBytes = (width * bits + 31) / 32 * 4;
  const uint32_t offset = 54 + (oneBit ? 8 : 0);
  std::vector<uint8_t> b = {'B', 'M'};
  put32(b, offset + rowBytes * height);
  put32(b, 0);
  put32(b, offset);
  put32(b, 40);
  put32(b, width);
  put32(b, topDown ? static_cast<uint32_t>(-height) : height);
  b.insert(b.end(), {1, 0, static_cast<uint8_t>(bits), 0});
  for (int i = 0; i < 6; ++i) put32(b, i == 4 && oneBit ? 2 : 0);
  if (oneBit) {
    put32(b, 0);
    put32(b, 0xFFFFFF);
  }
  for (int i = 0; i < height; ++i) {
    const int y = topDown ? i : height - 1 - i;
    if (!leftRight) {
      b.insert(b.end(), rowBytes, y < height / 2 ? 0 : 0xFF);
      continue;
    }
    for (int x = 0; x < rowBytes; ++x) b.push_back(x < rowBytes / 2 ? 0 : 0xFF);
  }
  return b;
}

class BmpToBmpTest : public ::testing::Test {
 protected:
  ScopedTempDir tempDir;
  const std::filesystem::path path = tempDir.path() / "input.bmp";
  HalFile file;
  Output convert(const std::vector<uint8_t>& bmp, bool crop) {
    {
      std::ofstream out(path, std::ios::binary);
      out.write(reinterpret_cast<const char*>(bmp.data()), bmp.size());
    }
    EXPECT_TRUE(file.open(path.c_str(), "rb"));
    Output out;
    EXPECT_TRUE(BmpToBmpConverter::bmpFileToBmpStream(file, out, crop, false));
    file.close();
    return out;
  }
  // Display stub is 64x40, so portrait output is at most 40x64. Returns level of pixel (x, y).
  static int level(const Output& out, int x, int y) {
    const unsigned bits = out.bytes[28];
    const uint32_t width = le32(out.bytes, 18);
    const auto rawHeight = static_cast<int32_t>(le32(out.bytes, 22));
    if (rawHeight > 0) y = rawHeight - 1 - y;
    const size_t row = le32(out.bytes, 10) + y * ((width * bits + 31) / 32 * 4);
    const uint8_t byte = out.bytes[row + x * bits / 8];
    return bits == 1 ? (byte >> (7 - x % 8)) & 1 : (byte >> (6 - (x % 4) * 2)) & 3;
  }
};

TEST_F(BmpToBmpTest, UpscalesSmallImageToFillScreen) {
  for (const bool topDown : {false, true}) {
    const Output out = convert(splitBmp(10, 16, topDown, false), true);
    EXPECT_EQ(le32(out.bytes, 18), 40u);
    EXPECT_EQ(le32(out.bytes, 22), topDown ? static_cast<uint32_t>(-64) : 64u);
    EXPECT_EQ(out.bytes[28], 2);
    EXPECT_EQ(out.bytes.size(), le32(out.bytes, 2));
    EXPECT_EQ(level(out, 20, 2), 0);
    EXPECT_EQ(level(out, 20, 61), 3);
  }
}

TEST_F(BmpToBmpTest, DownscalesAndFits) {
  const Output out = convert(splitBmp(160, 128, false, false), false);
  EXPECT_EQ(le32(out.bytes, 18), 40u);
  EXPECT_EQ(le32(out.bytes, 22), 32u);
  EXPECT_EQ(level(out, 5, 1), 0);
  EXPECT_EQ(level(out, 5, 30), 3);
}

TEST_F(BmpToBmpTest, CropsCenterWhileDownscaling) {
  for (const bool topDown : {false, true}) {
    const Output out = convert(splitBmp(320, 128, topDown, false, true), true);
    EXPECT_EQ(le32(out.bytes, 18), 40u);
    EXPECT_EQ(le32(out.bytes, 22), topDown ? static_cast<uint32_t>(-64) : 64u);
    EXPECT_EQ(level(out, 2, 32), 0);
    EXPECT_EQ(level(out, 37, 32), 3);
  }
}

// 4bpp BMP with the given grayscale palette and 4 vertical bands (indices 0..3 across width).
std::vector<uint8_t> fourBitBandedBmp(int width, int height, const std::vector<uint8_t>& paletteGray,
                                      uint32_t clrUsed) {
  const int rowBytes = (width * 4 + 31) / 32 * 4;
  const uint32_t offset = 54 + static_cast<uint32_t>(paletteGray.size()) * 4;
  std::vector<uint8_t> b = {'B', 'M'};
  put32(b, offset + rowBytes * height);
  put32(b, 0);
  put32(b, offset);
  put32(b, 40);
  put32(b, width);
  put32(b, height);
  b.insert(b.end(), {1, 0, 4, 0});
  for (int i = 0; i < 6; ++i) put32(b, i == 4 ? clrUsed : 0);
  for (uint8_t g : paletteGray) put32(b, uint32_t{g} | uint32_t{g} << 8 | uint32_t{g} << 16);
  for (int y = 0; y < height; ++y) {
    std::vector<uint8_t> row(rowBytes, 0);
    for (int x = 0; x < width; ++x) {
      const uint8_t idx = static_cast<uint8_t>(std::min(3, x * 4 / width));
      if (x & 1)
        row[x >> 1] |= idx;
      else
        row[x >> 1] |= static_cast<uint8_t>(idx << 4);
    }
    b.insert(b.end(), row.begin(), row.end());
  }
  return b;
}

TEST_F(BmpToBmpTest, OneBitSourceStaysOneBit) {
  const Output out = convert(splitBmp(20, 32, false, true), true);
  EXPECT_EQ(out.bytes[28], 1);
  EXPECT_EQ(le32(out.bytes, 18), 40u);
  EXPECT_EQ(level(out, 3, 2), 0);
  EXPECT_EQ(level(out, 3, 61), 1);
}

TEST_F(BmpToBmpTest, UnscaledNativeFourBitPreservesAllFourGrayLevels) {
  // 40x64 matches the portrait display stub (64x40 landscape); 60x64 crops horizontally with no scaling.
  const std::vector<uint8_t> native4 = {0, 85, 170, 255};
  for (const auto& [width, height, crop] :
       std::vector<std::tuple<int, int, bool>>{{40, 64, false}, {40, 48, false}, {60, 64, true}}) {
    const Output out = convert(fourBitBandedBmp(width, height, native4, 4), crop);
    EXPECT_EQ(out.bytes[28], 2);
    const int outW = static_cast<int>(le32(out.bytes, 18));
    const int outH = static_cast<int>(le32(out.bytes, 22));
    const int sourceX = (width - outW) / 2;
    for (int x = 0; x < outW; ++x) {
      const int expected = std::min(3, (sourceX + x) * 4 / width);
      EXPECT_EQ(level(out, x, outH / 2), expected) << "width=" << width << " x=" << x;
    }
  }
}

TEST_F(BmpToBmpTest, BitmapDetectsNativeAndNonNativePalettes) {
  auto parse = [&](const std::vector<uint8_t>& bmp) {
    {
      std::ofstream out(path, std::ios::binary);
      out.write(reinterpret_cast<const char*>(bmp.data()), bmp.size());
    }
    EXPECT_TRUE(file.open(path.c_str(), "rb"));
    Bitmap bitmap(file);
    EXPECT_EQ(bitmap.parseHeaders(), BmpReaderError::Ok);
    const bool native = bitmap.hasNativePalette();
    file.close();
    return native;
  };

  // 4-bit with explicit 4-color native palette and 16-entry zero-padded native palette.
  EXPECT_TRUE(parse(fourBitBandedBmp(40, 64, {0, 85, 170, 255}, 4)));
  EXPECT_TRUE(parse(fourBitBandedBmp(40, 64, {0, 85, 170, 255, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, 0)));

  // 4-bit with 16-shade grayscale palette is not native.
  std::vector<uint8_t> shades16(16);
  for (int i = 0; i < 16; ++i) shades16[i] = static_cast<uint8_t>(i * 17);
  EXPECT_FALSE(parse(fourBitBandedBmp(40, 64, shades16, 16)));

  // 24-bit BMP must not be treated as native even if biClrUsed is non-zero.
  std::vector<uint8_t> bmp24 = splitBmp(40, 64, false, false);
  bmp24[46] = 2;  // biClrUsed = 2; keep pixel offset shifted after an 8-byte palette
  bmp24.insert(bmp24.begin() + 54, 8, 0);
  const uint32_t newOff = le32(bmp24, 10) + 8;
  const uint32_t newSize = le32(bmp24, 2) + 8;
  for (int i = 0; i < 4; ++i) {
    bmp24[10 + i] = static_cast<uint8_t>(newOff >> (8 * i));
    bmp24[2 + i] = static_cast<uint8_t>(newSize >> (8 * i));
  }
  EXPECT_FALSE(parse(bmp24));
}

TEST_F(BmpToBmpTest, NonNativeFourBitStillDithersWithLegacyThresholds) {
  std::vector<uint8_t> shades16(16);
  for (int i = 0; i < 16; ++i) shades16[i] = static_cast<uint8_t>(i * 17);
  // Palette index 5 has luminance 85; with non-native 16-shade palette, legacy thresholds map 85 -> 2.
  auto bmp = fourBitBandedBmp(40, 64, shades16, 16);
  const uint32_t offset = le32(bmp, 10);
  std::fill(bmp.begin() + offset, bmp.end(), 0x55);  // every pixel = palette index 5 (lum 85)
  const Output out = convert(bmp, false);
  EXPECT_EQ(level(out, 0, 0), 2);
}

TEST_F(BmpToBmpTest, ScaledNativeFourBitStillScalesToTargetSize) {
  const Output out = convert(fourBitBandedBmp(20, 32, {0, 85, 170, 255}, 4), true);
  EXPECT_EQ(out.bytes[28], 2);
  EXPECT_EQ(le32(out.bytes, 18), 40u);
  EXPECT_EQ(le32(out.bytes, 22), 64u);
  EXPECT_EQ(level(out, 2, 32), 0);
  EXPECT_EQ(level(out, 37, 32), 3);
}

}  // namespace
