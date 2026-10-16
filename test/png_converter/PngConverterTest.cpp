#include <PngToBmpConverter.h>
#include <gtest/gtest.h>

#include <vector>
namespace {
const std::vector<uint8_t> validPng = {137, 80,  78,  71,  13, 10, 26, 10,  0,   0,  0,   13, 73,  72,  68,  82, 0,
                                       0,   0,   1,   0,   0,  0,  1,  8,   0,   0,  0,   0,  58,  126, 155, 85, 0,
                                       0,   0,   10,  73,  68, 65, 84, 120, 156, 99, 104, 0,  0,   0,   130, 0,  129,
                                       119, 205, 114, 182, 0,  0,  0,  0,   73,  69, 78,  68, 174, 66,  96,  130};
struct Sink : Print {
  std::vector<uint8_t> bytes;
  size_t write(uint8_t b) override {
    bytes.push_back(b);
    return 1;
  }
};
}  // namespace
TEST(PngConverter, ValidGrayscaleProducesBitmap) {
  Storage.setFile("fixture", validPng);
  HalFile file;
  ASSERT_TRUE(Storage.openFileForRead("PNG", "fixture", file));
  Sink out;
  ASSERT_TRUE(PngToBmpConverter::pngFileTo1BitBmpStreamWithSize(file, out, 1, 1));
  ASSERT_GT(out.bytes.size(), 2u);
  EXPECT_EQ(out.bytes[0], 'B');
  EXPECT_EQ(out.bytes[1], 'M');
}
TEST(PngConverter, InvalidDepthPairsFailBeforeWritingOutput) {
  for (const auto pair : {std::pair{0, 0}, {0, 9}, {2, 1}, {3, 16}, {4, 4}, {6, 2}}) {
    auto png = validPng;
    png[24] = pair.second;
    png[25] = pair.first;
    Storage.setFile("fixture", png);
    HalFile file;
    ASSERT_TRUE(Storage.openFileForRead("PNG", "fixture", file));
    Sink out;
    EXPECT_FALSE(PngToBmpConverter::pngFileTo1BitBmpStreamWithSize(file, out, 1, 1));
    EXPECT_TRUE(out.bytes.empty());
  }
}
