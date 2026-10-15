// The integer loader against FreeType itself, unhinted (spec 9.3), over every glyph of every bundled TTF: every
// glyph must come out byte for byte. FreeType renders outlines flagged OVERLAP 4x4 oversampled, which the
// engine does not (spec 11.6), so the reference clears that flag; the oversampled difference is measured on
// its own against the spec's 3% budget.
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "FreeTypeReference.h"
#include "TtfBuilder.h"
#include "TtfRasterizer.h"
#include "TtfTestUtil.h"
#include "ttf_raster_golden.h"

using namespace ttf;
using namespace ttf_test;

namespace {

constexpr uint32_t kPoints = 14;

int pixelAt(const uint8_t* bits, const int p) { return (bits[p >> 2] >> (6 - 2 * (p & 3))) & 3; }

class TtfVsFreeType : public ::testing::TestWithParam<std::string> {
 protected:
  void SetUp() override {
    data = readFont(GetParam());
    ASSERT_FALSE(data.empty());
    bytes = FontBytes(data.data(), static_cast<uint32_t>(data.size()));
    ASSERT_EQ(FaceStatus::Ok, parseFace(bytes, &face));
  }

  void compareEveryGlyph(uint32_t points);

  std::vector<uint8_t> data;
  FontBytes bytes;
  Face face;
};

std::string testName(const ::testing::TestParamInfo<std::string>& info) {
  std::string name = info.param.substr(info.param.find('/') + 1);
  for (char& c : name) {
    if (c == '-' || c == '.') c = '_';
  }
  return name;
}

}  // namespace

TEST_P(TtfVsFreeType, EveryGlyphMatchesFreeType) {
  // Small print, the body size, and a large heading.
  for (const uint32_t points : {8u, kPoints, 40u}) {
    SCOPED_TRACE(points);
    compareEveryGlyph(points);
  }
}

void TtfVsFreeType::compareEveryGlyph(const uint32_t points) {
  const FreeTypeReference reference(data, points);
  ASSERT_TRUE(reference.ok());
  ASSERT_EQ(reference.numGlyphs(), face.numGlyphs);
  const uint32_t ppem = ppemFromPoints(points);
  Rasterizer rasterizer;
  ASSERT_TRUE(rasterizer.allocate(planScratch(face, ppem, kPoolBytesFlash)));

  uint32_t glyphs = 0, tooLarge = 0, loadFailures = 0, advanceDiffs = 0, boxDiffs = 0, exactDiffs = 0;
  uint32_t overlapGlyphs = 0;
  for (uint32_t g = 0; g < face.numGlyphs; ++g) {
    const ReferenceGlyph expected = reference.glyph(g);
    if (!expected.loaded) continue;
    ++glyphs;
    EpdGlyph metrics{};
    const Status status = rasterizer.render(bytes, face, static_cast<uint16_t>(g), ppem, &metrics);
    // A bitmap over 255 px a side cannot be an EpdGlyph, and one over the output region is past its cap
    // (spec 5.3): both are Unsupported by design. Ubuntu's wordmark at 40 pt is 288 px wide.
    if (expected.width > 255 || expected.height > 255 ||
        glyphDataBytes(expected.width, expected.height, true) > rasterizer.plan().outputBytes) {
      EXPECT_EQ(Status::Unsupported, status) << "glyph " << g;
      ++tooLarge;
      continue;
    }
    if (status != Status::Ok) {
      ++loadFailures;
      ADD_FAILURE() << "glyph " << g << ": " << statusName(status);
      continue;
    }
    if (metrics.advanceX != static_cast<uint16_t>((expected.linearAdvance + 2048) >> 12)) ++advanceDiffs;
    if (metrics.width != expected.width || metrics.height != expected.height || metrics.left != expected.left ||
        metrics.top != expected.top) {
      ++boxDiffs;
      continue;
    }
    overlapGlyphs += expected.overlap;
    const uint8_t* ours = rasterizer.output();
    // An empty bitmap's data() may be null, and memcmp with a null pointer is undefined even for 0 bytes
    // (glibc declares it nonnull: UBSan on CI's Linux GCC).
    if (!expected.bitmap.empty() && std::memcmp(ours, expected.bitmap.data(), expected.bitmap.size()) != 0 &&
        ++exactDiffs <= 3) {
      ADD_FAILURE() << "glyph " << g << ", ours then FreeType's:\n"
                    << picture(metrics, ours) << "---\n"
                    << picture(metrics, expected.bitmap.data());
    }
  }
  std::printf(
      "%s @%u pt: %u glyphs (%u too large, %u flagged overlap), %u load failures, %u advance diffs, %u box "
      "diffs, %u inexact\n",
      GetParam().c_str(), points, glyphs, tooLarge, overlapGlyphs, loadFailures, advanceDiffs, boxDiffs, exactDiffs);
  EXPECT_GT(glyphs, 1000u);
  EXPECT_EQ(0u, loadFailures);
  EXPECT_EQ(0u, advanceDiffs);
  EXPECT_EQ(0u, boxDiffs);  // spec 9.3 allows 5; scaling exactly as FreeType does leaves none
  EXPECT_EQ(0u, exactDiffs);
}

TEST_P(TtfVsFreeType, OversampledOverlapsStayWithinThreePercent) {
  // Spec 9.3's pixel budget, against FreeType as it really renders outlines flagged OVERLAP: 4x4 oversampled.
  const FreeTypeReference reference(data, kPoints);
  ASSERT_TRUE(reference.ok());
  const uint32_t ppem = ppemFromPoints(kPoints);
  Rasterizer rasterizer;
  ASSERT_TRUE(rasterizer.allocate(planScratch(face, ppem, kPoolBytesFlash)));
  uint32_t overlapGlyphs = 0;
  uint64_t pixels = 0, differ = 0;
  for (uint32_t g = 0; g < face.numGlyphs; ++g) {
    const ReferenceGlyph expected = reference.glyph(g, true);
    if (!expected.loaded || !expected.overlap) continue;
    EpdGlyph metrics{};
    ASSERT_EQ(Status::Ok, rasterizer.render(bytes, face, static_cast<uint16_t>(g), ppem, &metrics)) << g;
    ASSERT_EQ(expected.width, metrics.width) << g;
    ASSERT_EQ(expected.height, metrics.height) << g;
    ++overlapGlyphs;
    for (int p = 0; p < expected.width * expected.height; ++p) {
      ++pixels;
      differ += pixelAt(rasterizer.output(), p) != pixelAt(expected.bitmap.data(), p);
    }
  }
  std::printf("%s @%u pt: %u overlap glyphs, %.2f%% of their pixels differ from FreeType's oversampled ones\n",
              GetParam().c_str(), kPoints, overlapGlyphs,
              pixels ? 100.0 * static_cast<double>(differ) / static_cast<double>(pixels) : 0.0);
  EXPECT_LE(differ * 100, pixels * 3);
}

TEST_P(TtfVsFreeType, TheCmapMatchesFreeType) {
  const FreeTypeReference reference(data, kPoints);
  ASSERT_TRUE(reference.ok());
  uint32_t mapped = 0, diffs = 0;
  for (uint32_t cp = 0; cp < 0x30000; ++cp) {
    const uint32_t expected = reference.glyphIndex(cp);
    mapped += expected != 0;
    if (glyphIndex(bytes, face, cp) != expected && ++diffs <= 5) ADD_FAILURE() << "U+" << std::hex << cp;
  }
  EXPECT_GT(mapped, 200u);
  EXPECT_EQ(0u, diffs);
  EXPECT_FALSE(bytes.outOfRange());
}

TEST_P(TtfVsFreeType, LineMetricsMatchTheConverterAtEverySize) {
  for (const uint32_t points : {8u, 12u, 14u, 18u, 24u, 40u}) {
    const FreeTypeReference reference(data, points);
    ASSERT_TRUE(reference.ok());
    const LineMetrics m = lineMetrics(face, ppemFromPoints(points));
    // fontconvert_sdcard.py: advanceY = ceil(height / 64), ascender = ceil, descender = floor.
    EXPECT_EQ(reference.height() / 64, m.advanceY) << points;
    EXPECT_EQ(reference.ascender() / 64, m.ascender) << points;
    EXPECT_EQ(reference.descender() / 64, m.descender) << points;
  }
}

INSTANTIATE_TEST_SUITE_P(BundledFonts, TtfVsFreeType, ::testing::ValuesIn(bundledTtfs()), testName);

TEST(TtfPoolSize, ThePoolNeverChangesTheOutput) {
  // Spec 2: the pool size is a speed dial only. Every glyph of a text face, with three pools.
  const std::vector<uint8_t> data = readFont("Bookerly/Bookerly-Regular.ttf");
  FontBytes bytes(data.data(), static_cast<uint32_t>(data.size()));
  Face face;
  ASSERT_EQ(FaceStatus::Ok, parseFace(bytes, &face));
  const uint32_t ppem = ppemFromPoints(kPoints);
  Rasterizer small, flash, sd;
  ASSERT_TRUE(small.allocate(planScratch(face, ppem, 2048)));
  ASSERT_TRUE(flash.allocate(planScratch(face, ppem, kPoolBytesFlash)));
  ASSERT_TRUE(sd.allocate(planScratch(face, ppem, kPoolBytesSd)));
  for (uint16_t g = 0; g < face.numGlyphs; ++g) {
    EpdGlyph a{}, b{}, c{};
    ASSERT_EQ(Status::Ok, small.render(bytes, face, g, ppem, &a));
    ASSERT_EQ(Status::Ok, flash.render(bytes, face, g, ppem, &b));
    ASSERT_EQ(Status::Ok, sd.render(bytes, face, g, ppem, &c));
    ASSERT_EQ(0, std::memcmp(&a, &b, sizeof a)) << g;
    ASSERT_EQ(0, std::memcmp(&a, &c, sizeof a)) << g;
    ASSERT_EQ(0, std::memcmp(small.output(), flash.output(), a.dataLength)) << g;
    ASSERT_EQ(0, std::memcmp(small.output(), sd.output(), a.dataLength)) << g;
  }
}

TEST(TtfGolden, TheDeviceBenchExpectsThisOutput) {
  // bench/ttf_raster_main.cpp computes the same hash on the device, over the same glyphs.
  const std::vector<uint8_t> data = readFont("Bookerly/Bookerly-Regular.ttf");
  FontBytes bytes(data.data(), static_cast<uint32_t>(data.size()));
  Face face;
  ASSERT_EQ(FaceStatus::Ok, parseFace(bytes, &face));
  Rasterizer rasterizer;
  ASSERT_TRUE(rasterizer.allocate(planScratch(face, ppemFromPoints(40), kPoolBytesFlash)));
  uint32_t hash = ttf_golden::kSeed;
  for (uint16_t g = 0; g < face.numGlyphs; ++g) {
    EpdGlyph metrics{};
    if (rasterizer.render(bytes, face, g, ppemFromPoints(kPoints), &metrics) != Status::Ok) continue;
    hash = ttf_golden::hashGlyph(hash, g, metrics, rasterizer.output());
  }
  EXPECT_EQ(ttf_golden::kBookerlyRegular14pt, hash) << std::hex << hash;
}

TEST(TtfVsFreeTypeSynthetic, CompoundsMatchFreeTypeBitForBit) {
  // The loader paths no bundled font reaches (Review Focus 2): rotated, scaled and mirrored components,
  // USE_MY_METRICS taking a component's left side bearing, and that two levels deep; quadratic curves throughout.
  Font font;
  font.glyphs.resize(8);
  font.glyphs[0].contours = {rect(50, 0, 450, 700)};
  font.glyphs[1].contours = {// a bowl and its counter
                             {{250, 0},
                              {450, 0, false},
                              {450, 250},
                              {450, 500, false},
                              {250, 500},
                              {50, 500, false},
                              {50, 250},
                              {50, 0, false}},
                             {{250, 100},
                              {150, 100, false},
                              {150, 250},
                              {150, 400, false},
                              {250, 400},
                              {350, 400, false},
                              {350, 250},
                              {350, 100, false}}};
  font.glyphs[2].contours = {rect(80, 0, 160, 700)};  // a stem whose lsb is not its xMin
  font.glyphs[2].lsbIsXMin = false;
  font.glyphs[2].lsb = 20;
  font.glyphs[2].advance = 300;
  font.glyphs[3].components = {{1, 300, 0, {0x3800, 0x0C00, -0x0C00, 0x3800}}, {2, -40, 80, {0x4000, 0x5000}}};
  font.glyphs[4].components = {{2, 10, 0, {}, 0x0200}, {1, 200, 100}};  // USE_MY_METRICS on the stem
  font.glyphs[5].components = {{3, 0, 0}, {4, 600, -50, {}, 0x0200}};   // ...and through a compound
  font.glyphs[6].components = {{1, 500, 0, {-0x4000, 0x4000}}};         // mirrored
  font.glyphs[7].components = {{2, 0, 0, {0x2666}}};                    // scaled to 0.6
  font.cmap = {{'A', 1}};
  const Bytes data = buildFont(font);
  FontBytes bytes(data.data(), static_cast<uint32_t>(data.size()));
  Face face;
  ASSERT_EQ(FaceStatus::Ok, parseFace(bytes, &face));
  Rasterizer rasterizer;
  ASSERT_TRUE(rasterizer.allocate({64, 16, kMaxOutputBytes, kPoolBytesFlash}));
  for (const uint32_t points : {8u, kPoints, 40u}) {
    const FreeTypeReference reference(data, points);
    ASSERT_TRUE(reference.ok());
    for (uint16_t g = 1; g < 8; ++g) {
      const ReferenceGlyph expected = reference.glyph(g);
      ASSERT_TRUE(expected.loaded) << g;
      EpdGlyph metrics{};
      ASSERT_EQ(Status::Ok, rasterizer.render(bytes, face, g, ppemFromPoints(points), &metrics)) << g;
      EXPECT_EQ(static_cast<uint16_t>((expected.linearAdvance + 2048) >> 12), metrics.advanceX) << g << " @" << points;
      ASSERT_EQ(expected.width, metrics.width) << g << " @" << points;
      ASSERT_EQ(expected.height, metrics.height) << g << " @" << points;
      EXPECT_EQ(expected.left, metrics.left) << g << " @" << points;
      EXPECT_EQ(expected.top, metrics.top) << g << " @" << points;
      EXPECT_EQ(0, std::memcmp(rasterizer.output(), expected.bitmap.data(), expected.bitmap.size()))
          << g << " @" << points << ", ours then FreeType's:\n"
          << picture(metrics, rasterizer.output()) << "---\n"
          << picture(metrics, expected.bitmap.data());
    }
  }
}
