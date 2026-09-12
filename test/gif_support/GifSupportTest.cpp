#include <GifImageLayout.h>
#include <GifLimitedWriter.h>
#include <gtest/gtest.h>

namespace {
class CountingOutput : public Print {
 public:
  size_t total = 0;
  size_t write(uint8_t) override { ++total; return 1; }
  size_t write(const uint8_t*, size_t size) override { total += size; return size; }
};

TEST(GifSupport, StopsAnExtractionAtTheByteLimit) {
  CountingOutput output;
  GifLimitedWriter writer(output, 10);
  const uint8_t bytes[8] = {};
  EXPECT_EQ(writer.write(bytes, 8), 8u);
  EXPECT_EQ(writer.write(bytes, 8), 0u);
  EXPECT_EQ(writer.write(bytes, 2), 2u);
  EXPECT_EQ(writer.write(0), 0u);
  EXPECT_EQ(output.total, 10u);
}

TEST(GifSupport, TallAndWideCoversStayWithinTheirTarget) {
  for (const auto& dimensions : {std::pair{1, 3072}, std::pair{3072, 1}, std::pair{640, 480}}) {
    for (const auto& target : {std::pair{480, 800}, std::pair{800, 480}}) {
      GifCommon::ImageLayout layout;
      ASSERT_TRUE(GifCommon::calculateLayout(dimensions.first, dimensions.second, target.first, target.second, true,
                                            layout));
      EXPECT_EQ(layout.width, target.first);
      EXPECT_EQ(layout.height, target.second);
      EXPECT_GE(layout.sourceX, 0);
      EXPECT_GE(layout.sourceY, 0);
      EXPECT_LE(layout.sourceX + layout.sourceWidth, dimensions.first);
      EXPECT_LE(layout.sourceY + layout.sourceHeight, dimensions.second);
    }
  }
}

TEST(GifSupport, FitRetainsTheWholeSource) {
  GifCommon::ImageLayout layout;
  ASSERT_TRUE(GifCommon::calculateLayout(640, 480, 240, 400, false, layout));
  EXPECT_EQ(layout.width, 240);
  EXPECT_EQ(layout.height, 180);
  EXPECT_EQ(layout.sourceWidth, 640);
  EXPECT_EQ(layout.sourceHeight, 480);
}

TEST(GifSupport, RejectsInvalidOrOversizedTargets) {
  GifCommon::ImageLayout layout;
  EXPECT_FALSE(GifCommon::calculateLayout(1, 1, 0, 800, true, layout));
  EXPECT_FALSE(GifCommon::calculateLayout(1, 1, 480, -1, true, layout));
  EXPECT_FALSE(GifCommon::calculateLayout(1, 1, 480, 32767, true, layout));
}
}  // namespace
