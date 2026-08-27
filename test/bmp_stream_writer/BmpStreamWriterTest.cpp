#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include <Print.h>

#include "BmpStreamWriter.h"

int adjustPixel(const int gray) { return gray; }

namespace {

class VectorPrint final : public Print {
 public:
  size_t write(const uint8_t value) override {
    bytes.push_back(value);
    return 1;
  }

  std::vector<uint8_t> bytes;
};

uint32_t read32(const std::vector<uint8_t>& bytes, const size_t offset) {
  return bytes[offset] | static_cast<uint32_t>(bytes[offset + 1]) << 8 |
         static_cast<uint32_t>(bytes[offset + 2]) << 16 | static_cast<uint32_t>(bytes[offset + 3]) << 24;
}

TEST(BmpStreamWriter, WritesTwoBitTopDownBitmap) {
  VectorPrint output;
  BmpStreamWriter writer;
  ASSERT_TRUE(writer.begin(output, 2, 1, false));

  ASSERT_EQ(output.bytes.size(), 70);
  EXPECT_EQ(read32(output.bytes, 2), 74);
  EXPECT_EQ(read32(output.bytes, 10), 70);
  EXPECT_EQ(read32(output.bytes, 18), 2);
  EXPECT_EQ(read32(output.bytes, 22), UINT32_MAX);
  EXPECT_EQ(output.bytes[28], 2);
  EXPECT_EQ(output.bytes[58], 0x55);
  EXPECT_EQ(output.bytes[62], 0xAA);

  const uint8_t row[] = {0, 255};
  ASSERT_TRUE(writer.writeRow(row, 0));
  ASSERT_EQ(output.bytes.size(), 74);
  EXPECT_EQ(output.bytes[70], 0x30);
}

TEST(BmpStreamWriter, WritesOneBitTopDownBitmap) {
  VectorPrint output;
  BmpStreamWriter writer;
  ASSERT_TRUE(writer.begin(output, 2, 1, true));

  ASSERT_EQ(output.bytes.size(), 62);
  EXPECT_EQ(read32(output.bytes, 2), 66);
  EXPECT_EQ(read32(output.bytes, 10), 62);
  EXPECT_EQ(output.bytes[28], 1);

  const uint8_t row[] = {0, 255};
  ASSERT_TRUE(writer.writeRow(row, 0));
  ASSERT_EQ(output.bytes.size(), 66);
  EXPECT_EQ(output.bytes[62], 0x40);
}

TEST(BmpStreamWriter, AreaAveragesRowsBeforePacking) {
  VectorPrint output;
  BmpStreamWriter writer;
  ASSERT_TRUE(writer.begin(output, 2, 1, false));

  GrayRowScaler scaler;
  ASSERT_TRUE(scaler.begin(4, 2, writer));
  const uint8_t first[] = {0, 10, 20, 30};
  const uint8_t second[] = {40, 50, 60, 70};
  ASSERT_TRUE(scaler.writeRow(first, 0));
  EXPECT_EQ(output.bytes.size(), 70);
  ASSERT_TRUE(scaler.writeRow(second, 1));
  ASSERT_EQ(output.bytes.size(), 74);
  EXPECT_EQ(output.bytes[70], 0x10);
}

TEST(BmpStreamWriter, CalculatesFitAndCropDimensions) {
  int width;
  int height;
  ASSERT_TRUE(calculateBmpOutputSize(600, 900, 480, 800, false, width, height));
  EXPECT_EQ(width, 480);
  EXPECT_EQ(height, 720);
  ASSERT_TRUE(calculateBmpOutputSize(600, 900, 480, 800, true, width, height));
  EXPECT_EQ(width, 533);
  EXPECT_EQ(height, 800);
}

}  // namespace
