// The integer TrueType loader and the span packer, on fonts built in memory. At 1000 units per em and a size of
// 1000 in 26.6 (15.6 px), one font unit is exactly 1/64 px, so the outlines below are written in 26.6.
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "TtfBuilder.h"
#include "TtfRasterizer.h"
#include "TtfTestUtil.h"

using namespace ttf;
using namespace ttf_test;

namespace {

constexpr uint32_t kUnitPpem = 1000;  // one font unit = 1/64 px

ScratchPlan roomyPlan() {
  ScratchPlan plan;
  plan.points = 64;
  plan.contours = 16;
  plan.outputBytes = 1024;
  plan.poolBytes = kPoolBytesFlash;
  return plan;
}

class TtfRasterizerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    font.glyphs.resize(1);
    font.glyphs[0].contours = {rect(0, 0, 64, 64)};
  }

  // Builds the font, then loads and draws `glyph`; the picture is empty when the load fails.
  Status draw(const uint16_t glyph, const uint32_t ppem26_6 = kUnitPpem, const ScratchPlan& plan = roomyPlan()) {
    data = buildFont(font);
    bytes = FontBytes(data.data(), static_cast<uint32_t>(data.size()));
    if (parseFace(bytes, &face) != FaceStatus::Ok) return Status::Malformed;
    if (!rasterizer.allocated() || rasterizer.plan().points != plan.points ||
        rasterizer.plan().outputBytes != plan.outputBytes || rasterizer.plan().contours != plan.contours ||
        rasterizer.plan().maxPpem26_6 != plan.maxPpem26_6) {
      if (!rasterizer.allocate(plan)) return Status::Transient;
    }
    metrics = EpdGlyph{};
    const Status status = rasterizer.render(bytes, face, glyph, ppem26_6, &metrics);
    image = status == Status::Ok ? picture(metrics, rasterizer.output()) : std::string();
    return status;
  }

  uint16_t add(const Glyph& glyph) {
    font.glyphs.push_back(glyph);
    return static_cast<uint16_t>(font.glyphs.size() - 1);
  }

  Font font;
  Bytes data;
  FontBytes bytes;
  Face face;
  Rasterizer rasterizer;
  EpdGlyph metrics{};
  std::string image;
};

Glyph outline(std::vector<std::vector<Point>> contours, const uint16_t advance = 500) {
  Glyph glyph;
  glyph.contours = std::move(contours);
  glyph.advance = advance;
  return glyph;
}

Glyph compound(std::vector<Component> components) {
  Glyph glyph;
  glyph.components = std::move(components);
  return glyph;
}

Glyph raw(Bytes record) {
  Glyph glyph;
  glyph.raw = std::move(record);
  glyph.lsbIsXMin = false;
  return glyph;
}

// A simple-glyph header: contour count and a box.
Bytes header(const int16_t contours) {
  Bytes out;
  put16(out, static_cast<uint16_t>(contours));
  for (int i = 0; i < 4; ++i) put16(out, 0);
  return out;
}

}  // namespace

TEST_F(TtfRasterizerTest, ASquareFillsItsBox) {
  const uint16_t g = add(outline({rect(64, 0, 384, 320)}));  // x 1..6 px, y 0..5 px
  ASSERT_EQ(Status::Ok, draw(g));
  EXPECT_EQ(5, metrics.width);
  EXPECT_EQ(5, metrics.height);
  EXPECT_EQ(1, metrics.left);
  EXPECT_EQ(5, metrics.top);
  EXPECT_EQ(7, metrics.dataLength);
  EXPECT_EQ(0u, metrics.dataOffset);
  EXPECT_EQ("33333\n33333\n33333\n33333\n33333\n", image);
}

TEST_F(TtfRasterizerTest, RowsRunFromTheTop) {
  const uint16_t g = add(outline({rect(0, 0, 192, 64), rect(0, 64, 64, 192)}));  // an L
  ASSERT_EQ(Status::Ok, draw(g));
  EXPECT_EQ("3..\n3..\n333\n", image);
  EXPECT_EQ(3, metrics.top);
}

TEST_F(TtfRasterizerTest, CoverageQuantisesWithTheConvertersThresholds) {
  // A pixel 8, 20, 36 and 52 of 64 covered: coverage 32, 80, 144, 208 -> nibbles 2, 5, 9, 13.
  const int16_t widths[] = {8, 20, 36, 52};
  const char* expected[] = {".\n", "1\n", "2\n", "3\n"};
  for (int i = 0; i < 4; ++i) {
    font.glyphs.resize(1);
    const uint16_t g = add(outline({rect(0, 0, widths[i], 64)}));
    ASSERT_EQ(Status::Ok, draw(g)) << i;
    EXPECT_EQ(expected[i], image) << i;
  }
}

TEST_F(TtfRasterizerTest, AdvancesComeFromHmtx) {
  const uint16_t g = add(outline({rect(64, 0, 384, 320)}, 500));
  ASSERT_EQ(Status::Ok, draw(g, 640));  // 10 px: 500 units are 4.99998 px
  EXPECT_EQ(80, metrics.advanceX);
}

TEST_F(TtfRasterizerTest, AGlyphWithNoOutlineLoadsEmpty) {
  Glyph space;
  space.advance = 250;
  const uint16_t g = add(space);
  ASSERT_EQ(Status::Ok, draw(g));
  EXPECT_EQ(0, metrics.width);
  EXPECT_EQ(0, metrics.height);
  EXPECT_EQ(0, metrics.dataLength);
  EXPECT_EQ(63, metrics.advanceX);  // 250 units = 3.906 px = 62.5 in 12.4, rounded up
  uint8_t canary[4] = {0xEE, 0xEE, 0xEE, 0xEE};
  EXPECT_EQ(Status::Ok, rasterizer.rasterize(canary));
  EXPECT_EQ(0xEE, canary[0]);
}

TEST_F(TtfRasterizerTest, TheOutlineMovesSoTheLeftSideBearingStartsAtTheOrigin) {
  // FreeType places a glyph by its hmtx left side bearing, not by its outline: xMin - lsb is taken off.
  Glyph glyph = outline({rect(64, 0, 128, 64)});
  glyph.lsbIsXMin = false;
  glyph.lsb = 0;
  const uint16_t shifted = add(glyph);
  ASSERT_EQ(Status::Ok, draw(shifted));
  EXPECT_EQ(0, metrics.left);
  glyph.lsb = 64;
  const uint16_t inPlace = add(glyph);
  ASSERT_EQ(Status::Ok, draw(inPlace));
  EXPECT_EQ(1, metrics.left);
}

TEST_F(TtfRasterizerTest, ComponentsAreOffset) {
  const uint16_t dot = add(outline({rect(0, 0, 64, 64)}));
  const uint16_t g = add(compound({{dot, 128, 64}, {dot, 0, 0}}));
  ASSERT_EQ(Status::Ok, draw(g));
  EXPECT_EQ(0, metrics.left);
  EXPECT_EQ(2, metrics.top);
  EXPECT_EQ("..3\n3..\n", image);
}

TEST_F(TtfRasterizerTest, ComponentsAreScaled) {
  const uint16_t square = add(outline({rect(0, 0, 128, 128)}));  // 2 x 2 px
  const uint16_t half = add(compound({{square, 0, 0, {0x2000}}}));
  ASSERT_EQ(Status::Ok, draw(half));
  EXPECT_EQ("3\n", image);
  const uint16_t flat = add(compound({{square, 0, 0, {0x4000, 0x2000}}}));  // x 1.0, y 0.5
  ASSERT_EQ(Status::Ok, draw(flat));
  EXPECT_EQ("33\n", image);
}

TEST_F(TtfRasterizerTest, TheTwoByTwoTransformTakesItsEntriesInFileOrder) {
  // a, b, c, d in the file: x' = a*x + c*y, y' = b*x + d*y. This one turns a quarter left: (x, y) -> (-y, x).
  const uint16_t bar = add(outline({rect(0, 0, 192, 64)}));  // 3 x 1 px
  const uint16_t turned = add(compound({{bar, 0, 0, {0, 0x4000, -0x4000, 0}}}));
  ASSERT_EQ(Status::Ok, draw(turned));
  EXPECT_EQ(-1, metrics.left);
  EXPECT_EQ(3, metrics.top);
  EXPECT_EQ("3\n3\n3\n", image);
}

TEST_F(TtfRasterizerTest, UseMyMetricsTakesTheComponentsLeftSideBearing) {
  Glyph base = outline({rect(64, 0, 128, 64)});
  base.lsbIsXMin = false;
  base.lsb = 0;  // FreeType shifts this glyph 1 px left when it stands alone
  const uint16_t dot = add(base);
  const uint16_t plain = add(compound({{dot, 0, 0}}));
  ASSERT_EQ(Status::Ok, draw(plain));
  EXPECT_EQ(1, metrics.left);  // the compound's own lsb equals its xMin: no shift
  const uint16_t mine = add(compound({{dot, 0, 0, {}, 0x0200}}));
  ASSERT_EQ(Status::Ok, draw(mine));
  EXPECT_EQ(0, metrics.left);
}

TEST_F(TtfRasterizerTest, PointMatchedAndScaledOffsetComponentsAreUnsupported) {
  const uint16_t dot = add(outline({rect(0, 0, 64, 64)}));
  Bytes record = header(-1);
  put16(record, 0x0000);  // byte args that are point numbers: ARGS_ARE_XY_VALUES clear
  put16(record, dot);
  record.push_back(0);
  record.push_back(0);
  const uint16_t matched = add(raw(record));
  EXPECT_EQ(Status::Unsupported, draw(matched));
  const uint16_t scaledOffset = add(compound({{dot, 64, 0, {0x4000}, 0x0800}}));
  EXPECT_EQ(Status::Unsupported, draw(scaledOffset));
}

TEST_F(TtfRasterizerTest, ACompoundThatContainsItselfStopsAtTheDepthLimit) {
  const uint16_t self = static_cast<uint16_t>(font.glyphs.size());
  add(compound({{self, 0, 0}}));
  EXPECT_EQ(Status::Unsupported, draw(self));
}

TEST_F(TtfRasterizerTest, TooManyComponentsAreUnsupported) {
  const uint16_t empty = add(Glyph{});
  const uint16_t wide = add(compound(std::vector<Component>(200, Component{empty, 0, 0})));
  const uint16_t bomb = add(compound(std::vector<Component>(200, Component{wide, 0, 0})));  // 40,000 loads
  EXPECT_EQ(Status::Ok, draw(wide));
  EXPECT_EQ(Status::Unsupported, draw(bomb));
}

TEST_F(TtfRasterizerTest, OutlinesPastTheScratchCapsAreUnsupported) {
  ScratchPlan small = roomyPlan();
  small.points = 4;
  small.contours = 1;
  const uint16_t five = add(outline({{{0, 0}, {64, 0}, {64, 64}, {32, 96}, {0, 64}}}));
  EXPECT_EQ(Status::Unsupported, draw(five, kUnitPpem, small));
  const uint16_t two = add(outline({rect(0, 0, 32, 32), rect(64, 0, 96, 32)}));
  small.points = 8;
  EXPECT_EQ(Status::Unsupported, draw(two, kUnitPpem, small));
  small.contours = 2;
  EXPECT_EQ(Status::Ok, draw(two, kUnitPpem, small));
}

TEST_F(TtfRasterizerTest, AdvancesTooWideForAnEpdGlyphAreUnsupported) {
  // 65,535 units at 255 px per em is 16,711 px: past the 4,095.9 px a 12.4 advanceX can hold.
  const uint16_t wide = add(outline({rect(0, 0, 64, 64)}, 65535));
  EXPECT_EQ(Status::Unsupported, draw(wide, kMaxPpem26_6));
}

TEST_F(TtfRasterizerTest, GlyphsPlacedPastAnInt16AreUnsupported) {
  // On a 16-unit em at 14 pt one unit is 117 px: a glyph at the int16 limits lands about 59,700 px away.
  font.unitsPerEm = 16;
  const uint16_t below = add(outline({rect(0, -32768, 1, -32767)}));
  const uint16_t right = add(outline({rect(32766, 0, 32767, 1)}));
  EXPECT_EQ(Status::Unsupported, draw(below, ppemFromPoints(14)));  // top would wrap to +5,794
  EXPECT_EQ(Status::Unsupported, draw(right, ppemFromPoints(14)));  // left would wrap to -5,796
}

TEST_F(TtfRasterizerTest, BitmapsTooLargeAreUnsupported) {
  const uint16_t big = add(outline({rect(0, 0, 2000, 2000)}));
  EXPECT_EQ(Status::Unsupported, draw(big, kMaxPpem26_6));  // 510 px a side
  ScratchPlan tight = roomyPlan();
  tight.outputBytes = 6;
  const uint16_t square = add(outline({rect(0, 0, 320, 320)}));  // 5 x 5 px: 7 bytes
  EXPECT_EQ(Status::Unsupported, draw(square, kUnitPpem, tight));
  EXPECT_EQ(Status::Unsupported, draw(square, 0));
  EXPECT_EQ(Status::Unsupported, draw(square, kMaxPpem26_6 + 1));
}

TEST_F(TtfRasterizerTest, BrokenGlyphDataIsMalformed) {
  Bytes backwards = header(2);
  put16(backwards, 5);
  put16(backwards, 3);  // contour ends must increase
  Bytes longRun = header(1);
  put16(longRun, 2);                                  // 3 points
  put16(longRun, 0);                                  // no instructions
  longRun.push_back(0x3F);                            // on the curve, one-byte positive x and y, repeated...
  longRun.push_back(5);                               // ...5 more times: 6 flags for 3 points
  for (int i = 0; i < 6; ++i) longRun.push_back(10);  // the 3 points' coordinates are all there
  Bytes noCoordinates = header(1);
  put16(noCoordinates, 2);
  put16(noCoordinates, 0);
  for (int i = 0; i < 3; ++i) noCoordinates.push_back(0x01);  // word coordinates follow, but the record ends
  const Bytes cutHeader = {0x00, 0x01, 0x00};
  for (const Bytes& record : {backwards, longRun, noCoordinates, cutHeader}) {
    const uint16_t g = add(raw(record));
    EXPECT_EQ(Status::Malformed, draw(g)) << g;
  }
  EXPECT_EQ(Status::Malformed, draw(static_cast<uint16_t>(font.glyphs.size())));  // past numGlyphs
}

TEST_F(TtfRasterizerTest, ALocaEntryPastGlyfIsMalformed) {
  const uint16_t g = add(outline({rect(0, 0, 64, 64)}));
  data = buildFont(font);
  const size_t locaRecord = findRecord(data, "loca");
  const uint32_t loca = static_cast<uint32_t>(data[locaRecord + 8] << 24 | data[locaRecord + 9] << 16 |
                                              data[locaRecord + 10] << 8 | data[locaRecord + 11]);
  set16(data, loca + 2 * (g + 1), 0xFFFF);  // short loca: the glyph's end, in words
  bytes = FontBytes(data.data(), static_cast<uint32_t>(data.size()));
  ASSERT_EQ(FaceStatus::Ok, parseFace(bytes, &face));
  ASSERT_TRUE(rasterizer.allocate(roomyPlan()));
  EXPECT_EQ(Status::Malformed, rasterizer.load(bytes, face, g, kUnitPpem, &metrics));
}

TEST_F(TtfRasterizerTest, AFailedLoadDrawsNothing) {
  const uint16_t g = add(raw({0x00, 0x01, 0x00}));
  ASSERT_EQ(Status::Malformed, draw(g));
  uint8_t canary[4] = {0xEE, 0xEE, 0xEE, 0xEE};
  EXPECT_EQ(Status::Malformed, rasterizer.rasterize(canary));
  EXPECT_EQ(0xEE, canary[0]);
}

TEST_F(TtfRasterizerTest, WithoutAScratchBlockNothingIsWrongWithTheGlyph) {
  data = buildFont(font);
  bytes = FontBytes(data.data(), static_cast<uint32_t>(data.size()));
  ASSERT_EQ(FaceStatus::Ok, parseFace(bytes, &face));
  EXPECT_EQ(Status::Transient, rasterizer.load(bytes, face, 0, kUnitPpem, &metrics));
  EXPECT_FALSE(isPermanent(Status::Transient));
  EXPECT_TRUE(isPermanent(Status::Malformed));
  EXPECT_TRUE(isPermanent(Status::Unsupported));
}

TEST_F(TtfRasterizerTest, RasterizeWritesWhereItIsTold) {
  const uint16_t g = add(outline({rect(0, 0, 192, 64)}));
  data = buildFont(font);
  bytes = FontBytes(data.data(), static_cast<uint32_t>(data.size()));
  ASSERT_EQ(FaceStatus::Ok, parseFace(bytes, &face));
  ASSERT_TRUE(rasterizer.allocate(roomyPlan()));
  ASSERT_EQ(Status::Ok, rasterizer.load(bytes, face, g, kUnitPpem, &metrics));
  uint8_t dst[2] = {0xFF, 0xFF};  // a GlyphBatch slot starts erased: every byte must be written
  ASSERT_EQ(Status::Ok, rasterizer.rasterize(dst));
  EXPECT_EQ(0xFC, dst[0]);  // 3 3 3 and a padding pixel of 0
  EXPECT_EQ(0xFF, dst[1]);  // past dataLength: untouched
}

TEST(TtfScratch, ThePlanFollowsTheFontAndItsCaps) {
  Face face;
  face.unitsPerEm = 1000;
  face.maxPoints = 600;
  face.maxContours = 40;
  face.xMax = 1000;
  face.yMax = 1000;
  const ScratchPlan plan = planScratch(face, 1000, kPoolBytesFlash);
  EXPECT_EQ(kMaxScratchPoints, plan.points);
  EXPECT_EQ(40, plan.contours);
  EXPECT_EQ(81u, plan.outputBytes);  // 16 px and a pixel each way: 18 x 18 at 2 bits
  EXPECT_EQ(kPoolBytesFlash, plan.poolBytes);
  EXPECT_EQ(1000u, plan.maxPpem26_6);
  EXPECT_EQ(kMaxOutputBytes, planScratch(face, kMaxPpem26_6, kPoolBytesFlash).outputBytes);
}

TEST(TtfScratch, TheOutputRegionNeverTrustsAnEmptyHeadBox) {
  // FreeType does not trust head's box; a font that leaves it 0 must still fit em-sized glyphs.
  Face face;
  face.unitsPerEm = 1000;
  face.maxPoints = 64;
  face.maxContours = 8;
  EXPECT_EQ(81u, planScratch(face, 1000, kPoolBytesFlash).outputBytes);  // the em square: 16 px + 2
}

TEST_F(TtfRasterizerTest, AGlyphTooBigForThePlannedSizesIsTransient) {
  // The region was sized for kUnitPpem: twice that size is the caller's mistake, not the glyph's, so it
  // must not be marked missing for good (Unsupported is permanent, spec 6.1).
  ScratchPlan plan = roomyPlan();
  plan.outputBytes = 7;
  plan.maxPpem26_6 = kUnitPpem;
  const uint16_t square = add(outline({rect(0, 0, 320, 320)}));  // 5 x 5 px at kUnitPpem: 7 bytes
  EXPECT_EQ(Status::Ok, draw(square, kUnitPpem, plan));
  EXPECT_EQ(Status::Transient, draw(square, 2 * kUnitPpem, plan));  // 10 x 10: 25 bytes
}

TEST(TtfScratch, OneBlockServesEveryStyle) {
  const ScratchPlan a{100, 10, 500, kPoolBytesFlash};
  const ScratchPlan b{50, 20, 900, kPoolBytesSd};
  const ScratchPlan m = mergePlans(a, b);
  EXPECT_EQ(100, m.points);
  EXPECT_EQ(20, m.contours);
  EXPECT_EQ(900u, m.outputBytes);
  EXPECT_EQ(kPoolBytesSd, m.poolBytes);
}

TEST(TtfScratch, AllocationHoldsThePlanAndRefusesImpossibleOnes) {
  Rasterizer rasterizer;
  const ScratchPlan plan{512, 256, 4096, kPoolBytesSd};
  ASSERT_TRUE(rasterizer.allocate(plan));
  EXPECT_TRUE(rasterizer.allocated());
  // The pool, 512 points (a vector and a tag each), 256 contour ends, the output.
  EXPECT_GE(rasterizer.scratchBytes(), kPoolBytesSd + 512u * 9 + 256u * 2 + 4096);
  EXPECT_NE(nullptr, rasterizer.output());
  rasterizer.release();
  EXPECT_FALSE(rasterizer.allocated());
  EXPECT_EQ(nullptr, rasterizer.output());
  EXPECT_FALSE(rasterizer.allocate({512, 256, 4096, 1024}));  // pool under ftgrays' minimum
  EXPECT_FALSE(rasterizer.allocate({kMaxScratchPoints + 1, 1, 64, kPoolBytesFlash}));
  EXPECT_FALSE(rasterizer.allocate({8, 1, kMaxOutputBytes + 1, kPoolBytesFlash}));
}

TEST(TtfStatus, NamesForTheLog) {
  EXPECT_STREQ("ok", statusName(Status::Ok));
  EXPECT_STREQ("unsupported", statusName(Status::Unsupported));
  EXPECT_STREQ("malformed", statusName(Status::Malformed));
  EXPECT_STREQ("transient", statusName(Status::Transient));
}
