#include <AnimatedGIF.h>
#include <GifCommon.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {
// Original, uniform images encoded with Pillow; the first has a four-entry palette.
const char* SMALL_GIF = "474946383761010001008100000000000000000000000000002c000000000100010000080400010404003b";
const char* WIDE_GIF =
    "474946383761000c02008100000000000000000000000000002c00000000000c02000008800001081c48b0a0c18308132a5cc8b0a1c387"
    "10234a9c48b1a2c58b18336adcc8b1a3c78f20438a1c49b2a4c9932853aa5cc9b2a5cb973063ca9c49b3a6cd9b3873eadcc9b3a7cf"
    "9f40830a1d4ab4a8d1a348932a5dcab4a9d3a750a34a9d4ab5aad5ab58b36addcab5abd7af60c38a1d4bb6acd9b368d3aa5dcbb6addb"
    "930101003b";

std::vector<uint8_t> fromHex(const std::string& hex) {
  std::vector<uint8_t> bytes;
  for (size_t i = 0; i < hex.size(); i += 2)
    bytes.push_back(static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
  return bytes;
}

class GifDecoderTest : public ::testing::Test {
 protected:
  std::filesystem::path path;
  void SetUp() override {
    path = std::filesystem::temp_directory_path() /
           (std::string("crosspoint-gif-") + ::testing::UnitTest::GetInstance()->current_test_info()->name() + ".gif");
  }
  void TearDown() override { std::filesystem::remove(path); }
  bool inspect(const std::vector<uint8_t>& bytes, GifBasicInfo& info) {
    {
      std::ofstream stream(path, std::ios::binary);
      stream.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }
    HalFile file;
    EXPECT_TRUE(file.open(path.c_str(), "rb"));
    const bool result = GifCommon::readBasicInfo(file, info);
    EXPECT_EQ(file.position(), 0u);
    return result;
  }
};

TEST_F(GifDecoderTest, ValidatesCodeSizeAfterGlobalAndLocalPalettes) {
  for (const bool local : {false, true}) {
    auto bytes = fromHex(SMALL_GIF);
    size_t codeOffset = 35;
    if (local) {
      bytes[34] = 0x81;
      bytes.insert(bytes.begin() + 35, 12, 0);
      codeOffset += 12;
    }
    for (const uint8_t codeSize : {0, 1, 2, 7, 8, 9, 13, 255}) {
      bytes[codeOffset] = codeSize;
      GifBasicInfo info;
      EXPECT_EQ(inspect(bytes, info), codeSize >= 2 && codeSize <= 8);
    }
  }
}

TEST_F(GifDecoderTest, RejectsMissingCodeSizeAndTruncatedLocalPalette) {
  auto bytes = fromHex(SMALL_GIF);
  bytes.resize(35);
  GifBasicInfo info;
  EXPECT_FALSE(inspect(bytes, info));
  bytes[34] = 0x81;
  bytes.resize(44);
  EXPECT_FALSE(inspect(bytes, info));
}

struct DrawCount {
  int rows = 0;
  int pixels = 0;
};
void countRow(GIFDRAW* row) {
  auto* count = static_cast<DrawCount*>(row->pUser);
  ASSERT_NE(count, nullptr);
  ++count->rows;
  count->pixels += row->iWidth;
  for (int i = 0; i < row->iWidth; ++i) EXPECT_EQ(row->pPixels[i], 0);
}

TEST_F(GifDecoderTest, DecodesSmallFourColorAndMaximumWidthImages) {
  for (const char* fixture : {SMALL_GIF, WIDE_GIF}) {
    auto bytes = fromHex(fixture);
    AnimatedGIF decoder;
    decoder.begin(GIF_PALETTE_RGB888);
    ASSERT_TRUE(decoder.open(bytes.data(), bytes.size(), countRow));
    DrawCount count;
    decoder.playFrame(false, nullptr, &count);
    EXPECT_EQ(decoder.getLastError(), GIF_SUCCESS);
    EXPECT_EQ(count.rows, decoder.getCanvasHeight());
    EXPECT_EQ(count.pixels, decoder.getCanvasWidth() * decoder.getCanvasHeight());
    decoder.close();
  }
}

TEST_F(GifDecoderTest, DecodesLargeGlobalPalettesFromMemoryAndFiles) {
  for (const uint8_t paletteBits : {6, 7}) {
    auto bytes = fromHex(SMALL_GIF);
    bytes[10] = static_cast<uint8_t>(0x80 | paletteBits);
    bytes.insert(bytes.begin() + 25, (3u << (paletteBits + 1)) - 12, 0);
    GifBasicInfo info;
    ASSERT_TRUE(inspect(bytes, info));
    for (const bool fromFile : {false, true}) {
      AnimatedGIF decoder;
      decoder.begin(GIF_PALETTE_RGB888);
      ASSERT_TRUE(fromFile ? decoder.open(path.c_str(), countRow) : decoder.open(bytes.data(), bytes.size(), countRow));
      DrawCount count;
      decoder.playFrame(false, nullptr, &count);
      EXPECT_EQ(decoder.getLastError(), GIF_SUCCESS);
      EXPECT_EQ(count.rows, 1);
      EXPECT_EQ(count.pixels, 1);
      decoder.close();
    }
  }
}
}  // namespace
