// The integer loader against FreeType itself, unhinted (spec 9.3), over every glyph of every bundled TTF.
// FreeType renders glyphs whose outlines carry an OVERLAP flag 4x4 oversampled, which the engine does not
// (spec 11.6): those are held to the spec's pixel budget. Every other glyph must come out byte for byte.
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "FreeTypeReference.h"
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
  uint64_t overlapPixels = 0, overlapPixelDiffs = 0;
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
    const uint8_t* ours = rasterizer.output();
    if (!expected.overlap) {
      if (std::memcmp(ours, expected.bitmap.data(), expected.bitmap.size()) != 0 && ++exactDiffs <= 3) {
        ADD_FAILURE() << "glyph " << g << ", ours then FreeType's:\n"
                      << picture(metrics, ours) << "---\n"
                      << picture(metrics, expected.bitmap.data());
      }
      continue;
    }
    ++overlapGlyphs;
    for (int p = 0; p < expected.width * expected.height; ++p) {
      ++overlapPixels;
      overlapPixelDiffs += pixelAt(ours, p) != pixelAt(expected.bitmap.data(), p);
    }
  }
  std::printf(
      "%s @%u pt: %u glyphs (%u too large), %u load failures, %u advance diffs, %u box diffs, %u inexact; %u overlap "
      "glyphs, "
      "%.2f%% of their pixels differ\n",
      GetParam().c_str(), points, glyphs, tooLarge, loadFailures, advanceDiffs, boxDiffs, exactDiffs, overlapGlyphs,
      overlapPixels ? 100.0 * static_cast<double>(overlapPixelDiffs) / static_cast<double>(overlapPixels) : 0.0);
  EXPECT_GT(glyphs, 1000u);
  EXPECT_EQ(0u, loadFailures);
  EXPECT_EQ(0u, advanceDiffs);
  EXPECT_EQ(0u, boxDiffs);  // spec 9.3 allows 5; scaling exactly as FreeType does leaves none
  EXPECT_EQ(0u, exactDiffs);
  EXPECT_LE(overlapPixelDiffs * 100, overlapPixels * 3);  // spec 9.3: at most 3% of pixels
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
