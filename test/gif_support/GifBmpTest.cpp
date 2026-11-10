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
