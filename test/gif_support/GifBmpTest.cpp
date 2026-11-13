#include <GifToBmpConverter.h>
#include <Print.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <vector>

namespace {
// Original Pillow-encoded gray pattern, with palette entry 3 transparent.
std::vector<uint8_t> image() {
  const std::string hex =
      "47494638396108000400810000000000555555aaaaaaffffff21f90401000003002c000000000800040000081500010410304020c181"
      "051102206830614286030302003b";
  std::vector<uint8_t> bytes;
  for (size_t i = 0; i < hex.size(); i += 2)
    bytes.push_back(static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
  return bytes;
}

class Output final : public Print {
 public:
  std::vector<uint8_t> bytes;
  size_t limit = SIZE_MAX;
  size_t write(uint8_t value) override { return write(&value, 1); }
  size_t write(const uint8_t* data, size_t count) override {
    count = std::min(count, limit - bytes.size());
    bytes.insert(bytes.end(), data, data + count);
    return count;
  }
};

uint32_t le32(const std::vector<uint8_t>& bytes, size_t at) {
  return uint32_t{bytes[at]} | uint32_t{bytes[at + 1]} << 8 | uint32_t{bytes[at + 2]} << 16 |
         uint32_t{bytes[at + 3]} << 24;
}

class GifBmpTest : public ::testing::Test {
 protected:
  std::filesystem::path path;
  HalFile file;
  void SetUp() override {
    path = std::filesystem::temp_directory_path() /
           (std::string("crosspoint-gif-bmp-") + ::testing::UnitTest::GetInstance()->current_test_info()->name());
    open(image());
  }
  void TearDown() override {
    file.close();
    std::filesystem::remove(path);
  }
  void open(const std::vector<uint8_t>& bytes) {
    file.close();
    {
      std::ofstream out(path, std::ios::binary);
      out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }
    ASSERT_TRUE(file.open(path.c_str(), "rb"));
  }
  void checkBmp(const Output& out, uint32_t width, uint32_t height, unsigned bits) {
    ASSERT_GE(out.bytes.size(), 62u);
    EXPECT_EQ(out.bytes[0], 'B');
    EXPECT_EQ(out.bytes[1], 'M');
    EXPECT_EQ(le32(out.bytes, 2), out.bytes.size());
    EXPECT_EQ(le32(out.bytes, 10), 54 + 4 * (1u << bits));
    EXPECT_EQ(le32(out.bytes, 18), width);
    EXPECT_EQ(le32(out.bytes, 22), uint32_t{0} - height);
    EXPECT_EQ(out.bytes[28], bits);
    EXPECT_EQ(le32(out.bytes, 34), (width * bits + 31) / 32 * 4 * height);
    EXPECT_EQ(out.bytes.size(), le32(out.bytes, 10) + le32(out.bytes, 34));
    EXPECT_TRUE(file.isOpen());
  }
};

TEST_F(GifBmpTest, WritesCroppedAndFitCoverHeaders) {
  for (const bool thresholds : {false, true}) {
    for (const bool crop : {false, true}) {
      Output out;
      ASSERT_TRUE(GifToBmpConverter::gifFileToBmpStream(file, out, crop, thresholds));
      checkBmp(out, 40, crop ? 64 : 20, 2);
    }
  }
}

TEST_F(GifBmpTest, WritesNativeAndScaledThumbnails) {
  for (const auto [width, height] : {std::pair{8, 4}, {4, 4}, {32, 16}}) {
    Output out;
    ASSERT_TRUE(GifToBmpConverter::gifFileTo1BitBmpStreamWithSize(file, out, width, height));
    checkBmp(out, width, width / 2, 1);
  }
}

TEST_F(GifBmpTest, PreservesPixelsAcrossScalingCroppingAndCanvasGaps) {
  // Reference FNV-1a hashes of complete BMPs, including headers and packed pixels.
  constexpr uint64_t expected[2][8] = {
      {7685477406190704776ULL, 14498139752244550009ULL, 4189858372662219127ULL, 8170268485886828064ULL,
       8350232636354198542ULL, 5481201112441086324ULL, 14417264059166155133ULL, 18054588502944234550ULL},
      {5710073575351826936ULL, 15064723361281102937ULL, 9649742591039960104ULL, 15714721179729549903ULL,
       10037843795648086482ULL, 13216124269044540203ULL, 6678199215268177629ULL, 5090147471856905439ULL}};
  for (const bool offsetFrame : {false, true}) {
    auto bytes = image();
    if (offsetFrame) {
      bytes[6] = 12;
      bytes[8] = 10;
      bytes[34] = 2;
      bytes[36] = 3;
    }
    open(bytes);
    for (int mode = 0; mode < 8; ++mode) {
      Output out;
      if (mode < 4) {
        ASSERT_TRUE(GifToBmpConverter::gifFileToBmpStream(file, out, mode & 1, mode & 2));
      } else {
        constexpr std::pair<int, int> targets[] = {{8, 4}, {4, 4}, {32, 16}, {3, 5}};
        const auto [width, height] = targets[mode - 4];
        ASSERT_TRUE(GifToBmpConverter::gifFileTo1BitBmpStreamWithSize(file, out, width, height));
      }
      uint64_t hash = 14695981039346656037ULL;
      for (const uint8_t byte : out.bytes) hash = (hash ^ byte) * 1099511628211ULL;
      EXPECT_EQ(hash, expected[offsetFrame][mode]) << "offsetFrame=" << offsetFrame << ", mode=" << mode;
    }
  }
}

TEST_F(GifBmpTest, RejectsEveryShortHeaderWriteAndShortRows) {
  for (const bool oneBit : {false, true}) {
    for (size_t limit = 0; limit <= (oneBit ? 62u : 70u); ++limit) {
      Output out;
      out.limit = limit;
      EXPECT_FALSE(oneBit ? GifToBmpConverter::gifFileTo1BitBmpStreamWithSize(file, out, 8, 4)
                          : GifToBmpConverter::gifFileToBmpStream(file, out));
      EXPECT_EQ(out.bytes.size(), limit);
      EXPECT_TRUE(file.isOpen());
    }
  }
}

TEST_F(GifBmpTest, RejectsInterlacingAndInvalidCodeSize) {
  for (const bool interlaced : {false, true}) {
    auto bytes = image();
    if (interlaced)
      bytes[42] |= 0x40;
    else
      bytes[43] = 13;
    open(bytes);
    Output out;
    EXPECT_FALSE(GifToBmpConverter::gifFileToBmpStream(file, out));
    EXPECT_TRUE(out.bytes.empty());
  }
}
}  // namespace
