#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <string_view>
#include <vector>

#include "EpdFontData.h"
#include "FontCacheManager.h"
#include "FontDecompressor.h"
#include "GlyphStreamCodec.h"
#include "GlyphStreamFixtures.generated.h"

namespace {

bool countHeapAllocations = false;
size_t heapAllocationCount = 0;

}  // namespace

void* operator new(const size_t size) {
  if (countHeapAllocations) heapAllocationCount++;
  if (void* allocation = std::malloc(size)) return allocation;
  throw std::bad_alloc();
}

void operator delete(void* allocation) noexcept { std::free(allocation); }

void operator delete(void* allocation, size_t) noexcept { std::free(allocation); }

namespace {

namespace fixtures = glyph_stream_fixtures;

std::vector<uint8_t> fromHex(const std::string_view text) {
  std::vector<uint8_t> bytes;
  bytes.reserve(text.size() / 2);
  auto nibble = [](const char value) -> uint8_t {
    return value <= '9' ? static_cast<uint8_t>(value - '0') : static_cast<uint8_t>(value - 'a' + 10);
  };
  for (size_t index = 0; index < text.size(); index += 2) {
    bytes.push_back(static_cast<uint8_t>((nibble(text[index]) << 4) | nibble(text[index + 1])));
  }
  return bytes;
}

EpdFontData makeFont(const std::vector<uint8_t>& bitmap, const EpdGlyph* glyphs, const EpdUnicodeInterval* intervals,
                     const uint32_t intervalCount, const bool is2Bit) {
  EpdFontData font{};
  font.bitmap = bitmap.data();
  font.glyph = glyphs;
  font.intervals = intervals;
  font.intervalCount = intervalCount;
  font.is2Bit = is2Bit;
  font.bitmapFormat = EPD_BITMAP_FORMAT_GLYPH_STREAM_V1;
  return font;
}

class GlyphStreamCodecTest : public testing::Test {
 protected:
  std::array<uint8_t, GlyphStreamCodec::SCRATCH_PLANE_SIZE> scratchA{};
  std::array<uint8_t, GlyphStreamCodec::SCRATCH_PLANE_SIZE> scratchB{};
  std::array<uint8_t, GlyphStreamCodec::SCRATCH_PLANE_SIZE> output{};
};

TEST_F(GlyphStreamCodecTest, EmptyGlyphNeedsNoStream) {
  const std::vector<uint8_t> bitmap{0};
  const EpdGlyph glyphs[] = {{0, 0, 0, 0, 0, 0, 0}};
  const EpdUnicodeInterval intervals[] = {{0x20, 0x20, 0}};
  const EpdFontData font = makeFont(bitmap, glyphs, intervals, 1, false);

  EXPECT_TRUE(GlyphStreamCodec::decode(&font, 0, true, scratchA.data(), scratchB.data(), output.data(), 0));
}

TEST_F(GlyphStreamCodecTest, RawOneBitGlyphRoundtrips) {
  const std::vector<uint8_t> bitmap = fromHex("4000");
  const EpdGlyph glyphs[] = {{1, 1, 0, 0, 0, 2, 0}};
  const EpdUnicodeInterval intervals[] = {{0x41, 0x41, 0}};
  const EpdFontData font = makeFont(bitmap, glyphs, intervals, 1, false);

  ASSERT_TRUE(GlyphStreamCodec::decode(&font, 0, true, scratchA.data(), scratchB.data(), output.data(), 1));
  EXPECT_EQ(0, output[0]);
}

TEST_F(GlyphStreamCodecTest, PythonDepthTwoTwoBitFixtureIsBitExact) {
  const std::vector<uint8_t> bitmap(fixtures::kDepthTwoTwoBitBitmap.begin(), fixtures::kDepthTwoTwoBitBitmap.end());
  const EpdFontData font =
      makeFont(bitmap, fixtures::kDepthTwoTwoBitGlyphs.data(), fixtures::kDepthTwoTwoBitIntervals.data(), 1, true);

  for (uint32_t glyphIndex = 0; glyphIndex < fixtures::kDepthTwoTwoBitGlyphs.size(); ++glyphIndex) {
    const size_t expectedOffset = fixtures::kDepthTwoTwoBitExpectedOffsets[glyphIndex];
    const size_t expectedLength = fixtures::kDepthTwoTwoBitExpectedLengths[glyphIndex];
    ASSERT_TRUE(GlyphStreamCodec::decode(&font, glyphIndex, true, scratchA.data(), scratchB.data(), output.data(),
                                         output.size()));
    EXPECT_EQ(std::vector<uint8_t>(fixtures::kDepthTwoTwoBitExpectedBitmap.begin() + expectedOffset,
                                   fixtures::kDepthTwoTwoBitExpectedBitmap.begin() + expectedOffset + expectedLength),
              std::vector<uint8_t>(output.begin(), output.begin() + expectedLength));
  }
}

TEST_F(GlyphStreamCodecTest, MaximumDimensionsCodedFixtureRoundtrips) {
  const std::vector<uint8_t> bitmap(fixtures::kMaximumDimensionsOneBitBitmap.begin(),
                                    fixtures::kMaximumDimensionsOneBitBitmap.end());
  const EpdFontData font = makeFont(bitmap, fixtures::kMaximumDimensionsOneBitGlyphs.data(),
                                    fixtures::kMaximumDimensionsOneBitIntervals.data(), 1, false);

  ASSERT_TRUE(GlyphStreamCodec::decode(&font, 0, true, scratchA.data(), scratchB.data(), output.data(),
                                       fixtures::kMaximumDimensionsOneBitExpectedBitmap.size()));
  EXPECT_EQ(
      std::vector<uint8_t>(fixtures::kMaximumDimensionsOneBitExpectedBitmap.begin(),
                           fixtures::kMaximumDimensionsOneBitExpectedBitmap.end()),
      std::vector<uint8_t>(output.begin(), output.begin() + fixtures::kMaximumDimensionsOneBitExpectedBitmap.size()));
}

TEST_F(GlyphStreamCodecTest, FontDecompressorDecodesGlyphStreamOnDemand) {
  const std::vector<uint8_t> bitmap(fixtures::kMaximumDimensionsOneBitBitmap.begin(),
                                    fixtures::kMaximumDimensionsOneBitBitmap.end());
  const EpdFontData font = makeFont(bitmap, fixtures::kMaximumDimensionsOneBitGlyphs.data(),
                                    fixtures::kMaximumDimensionsOneBitIntervals.data(), 1, false);
  FontDecompressor decompressor;

  ASSERT_TRUE(decompressor.init());
  const uint8_t* decoded = decompressor.getBitmap(&font, &fixtures::kMaximumDimensionsOneBitGlyphs[0], 0);

  ASSERT_NE(nullptr, decoded);
  EXPECT_EQ(std::vector<uint8_t>(fixtures::kMaximumDimensionsOneBitExpectedBitmap.begin(),
                                 fixtures::kMaximumDimensionsOneBitExpectedBitmap.end()),
            std::vector<uint8_t>(decoded, decoded + fixtures::kMaximumDimensionsOneBitExpectedBitmap.size()));
}

TEST_F(GlyphStreamCodecTest, FontDecompressorPrewarmsGlyphStreamPixels) {
  const std::vector<uint8_t> bitmap = fromHex("40004080");
  const EpdGlyph glyphs[] = {
      {1, 1, 0, 0, 0, 2, 0},
      {1, 1, 0, 0, 0, 2, 2},
  };
  const EpdUnicodeInterval intervals[] = {{0x41, 0x42, 0}};
  const EpdFontData font = makeFont(bitmap, glyphs, intervals, 1, false);
  FontDecompressor decompressor;

  ASSERT_TRUE(decompressor.init());
  ASSERT_EQ(0, decompressor.prewarmCache(&font, "AB"));
  const uint8_t* first = decompressor.getBitmap(&font, &glyphs[0], 0);
  const uint8_t* second = decompressor.getBitmap(&font, &glyphs[1], 1);

  ASSERT_NE(nullptr, first);
  ASSERT_NE(nullptr, second);
  EXPECT_EQ(0x00, first[0]);
  EXPECT_EQ(0x80, second[0]);
}

TEST_F(GlyphStreamCodecTest, OneBitGlyphStreamUsesSharedDecompressorRouting) {
  EpdFontData rawFont{};
  EXPECT_FALSE(epdFontUsesDecompressor(&rawFont));

  rawFont.bitmapFormat = EPD_BITMAP_FORMAT_GLYPH_STREAM_V1;
  EXPECT_TRUE(epdFontUsesDecompressor(&rawFont));

  EpdFontGroup legacyGroup{};
  rawFont.bitmapFormat = EPD_BITMAP_FORMAT_LEGACY;
  rawFont.groups = &legacyGroup;
  rawFont.groupCount = 1;
  EXPECT_TRUE(epdFontUsesDecompressor(&rawFont));
}

TEST_F(GlyphStreamCodecTest, FontCacheManagerPrewarmsOneBitGlyphStreamButSkipsLegacyRaw) {
  const std::vector<uint8_t> streamBitmap = fromHex("4000");
  const EpdGlyph streamGlyphs[] = {{1, 1, 0, 0, 0, 2, 0}};
  const EpdUnicodeInterval intervals[] = {{0x41, 0x41, 0}};
  const EpdFontData streamFont = makeFont(streamBitmap, streamGlyphs, intervals, 1, false);
  const EpdFont streamRegular(&streamFont);
  const std::map<int, EpdFontFamily> streamFonts{{7, EpdFontFamily(&streamRegular)}};
  const std::map<int, SdCardFont*> noSdFonts;
  FontDecompressor streamDecompressor;
  FontCacheManager streamManager(streamFonts, noSdFonts);

  ASSERT_TRUE(streamDecompressor.init());
  streamManager.setFontDecompressor(&streamDecompressor);
  streamManager.prewarmCache(7, "A", 0x01);

  EXPECT_EQ(1U, streamDecompressor.getStats().pageBufferBytes);
  const uint8_t* decoded = streamDecompressor.getBitmap(&streamFont, &streamGlyphs[0], 0);
  ASSERT_NE(nullptr, decoded);
  EXPECT_EQ(0x00, decoded[0]);
  EXPECT_EQ(1U, streamDecompressor.getStats().cacheHits);

  const std::vector<uint8_t> rawBitmap{0x80};
  EpdGlyph rawGlyphs[] = {{1, 1, 0, 0, 0, 1, 0}};
  EpdFontData rawFont = makeFont(rawBitmap, rawGlyphs, intervals, 1, false);
  rawFont.bitmapFormat = EPD_BITMAP_FORMAT_LEGACY;
  const EpdFont rawRegular(&rawFont);
  const std::map<int, EpdFontFamily> rawFonts{{8, EpdFontFamily(&rawRegular)}};
  FontDecompressor rawDecompressor;
  FontCacheManager rawManager(rawFonts, noSdFonts);

  ASSERT_TRUE(rawDecompressor.init());
  rawManager.setFontDecompressor(&rawDecompressor);
  rawManager.prewarmCache(8, "A", 0x01);

  EXPECT_EQ(0U, rawDecompressor.getStats().pageBufferBytes);
  EXPECT_EQ(rawBitmap.data(), rawDecompressor.getBitmap(&rawFont, &rawGlyphs[0], 0));
}

TEST_F(GlyphStreamCodecTest, PrewarmScopeRoutesTextToRecordedStyle) {
  const std::vector<uint8_t> regularBitmap = fromHex("40004080");
  const std::vector<uint8_t> boldBitmap = fromHex("40004080");
  const EpdGlyph glyphs[] = {
      {1, 1, 0, 0, 0, 2, 0},
      {1, 1, 0, 0, 0, 2, 2},
  };
  const EpdUnicodeInterval intervals[] = {{0x41, 0x42, 0}};
  const EpdFontData regularFont = makeFont(regularBitmap, glyphs, intervals, 1, false);
  const EpdFontData boldFont = makeFont(boldBitmap, glyphs, intervals, 1, false);
  const EpdFont regular(&regularFont);
  const EpdFont bold(&boldFont);
  const std::map<int, EpdFontFamily> fonts{{7, EpdFontFamily(&regular, &bold)}};
  const std::map<int, SdCardFont*> noSdFonts;
  FontDecompressor decompressor;
  FontCacheManager manager(fonts, noSdFonts);

  ASSERT_TRUE(decompressor.init());
  manager.setFontDecompressor(&decompressor);
  auto scope = manager.createPrewarmScope();
  manager.recordText("A", 7, EpdFontFamily::REGULAR);
  manager.recordText("B", 7, EpdFontFamily::BOLD);
  manager.recordText("B", 7, EpdFontFamily::REGULAR);
  scope.endScanAndPrewarm();

  EXPECT_EQ(3U, decompressor.getStats().pageBufferBytes);
  ASSERT_NE(nullptr, decompressor.getBitmap(&regularFont, &glyphs[0], 0));
  ASSERT_NE(nullptr, decompressor.getBitmap(&regularFont, &glyphs[1], 1));
  ASSERT_NE(nullptr, decompressor.getBitmap(&boldFont, &glyphs[1], 1));
  EXPECT_EQ(3U, decompressor.getStats().cacheHits);
}

TEST_F(GlyphStreamCodecTest, PrewarmScopeMergesStylesThatResolveToTheSameFont) {
  const std::vector<uint8_t> bitmap = fromHex("40004080");
  const EpdGlyph glyphs[] = {
      {1, 1, 0, 0, 0, 2, 0},
      {1, 1, 0, 0, 0, 2, 2},
  };
  const EpdUnicodeInterval intervals[] = {{0x41, 0x42, 0}};
  const EpdFontData fontData = makeFont(bitmap, glyphs, intervals, 1, false);
  const EpdFont regular(&fontData);
  const std::map<int, EpdFontFamily> fonts{{7, EpdFontFamily(&regular)}};
  const std::map<int, SdCardFont*> noSdFonts;
  FontDecompressor decompressor;
  FontCacheManager manager(fonts, noSdFonts);

  ASSERT_TRUE(decompressor.init());
  manager.setFontDecompressor(&decompressor);
  auto scope = manager.createPrewarmScope();
  manager.recordText("A", 7, EpdFontFamily::REGULAR);
  manager.recordText("B", 7, EpdFontFamily::BOLD);
  scope.endScanAndPrewarm();

  EXPECT_EQ(2U, decompressor.getStats().pageBufferBytes);
  ASSERT_NE(nullptr, decompressor.getBitmap(&fontData, &glyphs[0], 0));
  ASSERT_NE(nullptr, decompressor.getBitmap(&fontData, &glyphs[1], 1));
  EXPECT_EQ(2U, decompressor.getStats().cacheHits);
}

TEST_F(GlyphStreamCodecTest, PrewarmScanDoesNotAllocateHeapMemory) {
  const std::map<int, EpdFontFamily> noFonts;
  const std::map<int, SdCardFont*> noSdFonts;
  FontCacheManager manager(noFonts, noSdFonts);

  heapAllocationCount = 0;
  countHeapAllocations = true;
  auto scope = manager.createPrewarmScope();
  manager.recordText("Repeated text: \xC3\xA9 \xE4\xB8\xAD \xF0\x9F\x98\x80", 7, EpdFontFamily::REGULAR);
  countHeapAllocations = false;

  EXPECT_EQ(0U, heapAllocationCount);
  scope.endScanAndPrewarm();
}

TEST_F(GlyphStreamCodecTest, PrewarmScopePreservesUniqueMultibyteCodepoints) {
  const std::vector<uint8_t> bitmap = fromHex("400040004000");
  const EpdGlyph glyphs[] = {
      {1, 1, 0, 0, 0, 2, 0},
      {1, 1, 0, 0, 0, 2, 2},
      {1, 1, 0, 0, 0, 2, 4},
  };
  const EpdUnicodeInterval intervals[] = {
      {0x00E9, 0x00E9, 0},
      {0x4E2D, 0x4E2D, 1},
      {0x1F600, 0x1F600, 2},
  };
  const EpdFontData fontData = makeFont(bitmap, glyphs, intervals, 3, false);
  const EpdFont regular(&fontData);
  const std::map<int, EpdFontFamily> fonts{{7, EpdFontFamily(&regular)}};
  const std::map<int, SdCardFont*> noSdFonts;
  FontDecompressor decompressor;
  FontCacheManager manager(fonts, noSdFonts);

  ASSERT_TRUE(decompressor.init());
  manager.setFontDecompressor(&decompressor);
  auto scope = manager.createPrewarmScope();
  manager.recordText("\xC3\xA9\xE4\xB8\xAD\xF0\x9F\x98\x80\xC3\xA9", 7, EpdFontFamily::REGULAR);
  scope.endScanAndPrewarm();

  EXPECT_EQ(3U, decompressor.getStats().pageBufferBytes);
  ASSERT_NE(nullptr, decompressor.getBitmap(&fontData, &glyphs[0], 0));
  ASSERT_NE(nullptr, decompressor.getBitmap(&fontData, &glyphs[1], 1));
  ASSERT_NE(nullptr, decompressor.getBitmap(&fontData, &glyphs[2], 2));
  EXPECT_EQ(3U, decompressor.getStats().cacheHits);
}

TEST_F(GlyphStreamCodecTest, CodedPayloadTruncationZeroFillsSafely) {
  const std::vector<uint8_t> bitmap(fixtures::kMaximumDimensionsOneBitBitmap.begin(),
                                    fixtures::kMaximumDimensionsOneBitBitmap.end());
  EpdGlyph glyphs[] = {fixtures::kMaximumDimensionsOneBitGlyphs[0]};
  glyphs[0].dataLength = 1;
  const EpdFontData font = makeFont(bitmap, glyphs, fixtures::kMaximumDimensionsOneBitIntervals.data(), 1, false);

  for (uint16_t length = 1; length <= bitmap.size(); ++length) {
    glyphs[0].dataLength = length;
    EXPECT_TRUE(GlyphStreamCodec::decode(&font, 0, true, scratchA.data(), scratchB.data(), output.data(), 363));
  }
}

TEST_F(GlyphStreamCodecTest, RejectsReservedHeaderBits) {
  const std::vector<uint8_t> bitmap = fromHex("10");
  const EpdGlyph glyphs[] = {{1, 1, 0, 0, 0, 1, 0}};
  const EpdUnicodeInterval intervals[] = {{0x41, 0x41, 0}};
  const EpdFontData font = makeFont(bitmap, glyphs, intervals, 1, false);

  EXPECT_FALSE(GlyphStreamCodec::decode(&font, 0, true, scratchA.data(), scratchB.data(), output.data(), 1));
}

TEST_F(GlyphStreamCodecTest, RejectsUndersizedPackedOutput) {
  const std::vector<uint8_t> bitmap(fixtures::kMaximumDimensionsOneBitBitmap.begin(),
                                    fixtures::kMaximumDimensionsOneBitBitmap.end());
  const EpdFontData font = makeFont(bitmap, fixtures::kMaximumDimensionsOneBitGlyphs.data(),
                                    fixtures::kMaximumDimensionsOneBitIntervals.data(), 1, false);

  EXPECT_FALSE(GlyphStreamCodec::decode(&font, 0, true, scratchA.data(), scratchB.data(), output.data(),
                                        fixtures::kMaximumDimensionsOneBitExpectedBitmap.size() - 1));
}

TEST(GlyphStreamMetadataTest, ValueInitializedLegacyFontKeepsFormatZero) {
  const EpdFontData legacy{};

  EXPECT_EQ(EPD_BITMAP_FORMAT_LEGACY, legacy.bitmapFormat);
}

TEST_F(GlyphStreamCodecTest, RejectsTruncatedReferenceHeader) {
  const std::vector<uint8_t> bitmap = fromHex("8700");
  const EpdGlyph glyphs[] = {{1, 1, 0, 0, 0, 2, 0}};
  const EpdUnicodeInterval intervals[] = {{0x41, 0x41, 0}};
  const EpdFontData font = makeFont(bitmap, glyphs, intervals, 1, false);

  EXPECT_FALSE(GlyphStreamCodec::decode(&font, 0, true, scratchA.data(), scratchB.data(), output.data(), 1));
}

TEST_F(GlyphStreamCodecTest, RejectsTruncatedRawPayload) {
  const std::vector<uint8_t> bitmap = fromHex("40");
  const EpdGlyph glyphs[] = {{1, 1, 0, 0, 0, 1, 0}};
  const EpdUnicodeInterval intervals[] = {{0x41, 0x41, 0}};
  const EpdFontData font = makeFont(bitmap, glyphs, intervals, 1, false);

  EXPECT_FALSE(GlyphStreamCodec::decode(&font, 0, true, scratchA.data(), scratchB.data(), output.data(), 1));
}

TEST_F(GlyphStreamCodecTest, RejectsBaseIndexThatIsNotEarlier) {
  const std::vector<uint8_t> bitmap = fromHex("4000870100");
  const EpdGlyph glyphs[] = {
      {1, 1, 0, 0, 0, 2, 0},
      {1, 1, 0, 0, 0, 3, 2},
  };
  const EpdUnicodeInterval intervals[] = {{0x41, 0x42, 0}};
  const EpdFontData font = makeFont(bitmap, glyphs, intervals, 1, false);

  EXPECT_FALSE(GlyphStreamCodec::decode(&font, 1, true, scratchA.data(), scratchB.data(), output.data(), 1));
}

TEST_F(GlyphStreamCodecTest, RejectsWidthMismatchedBase) {
  const std::vector<uint8_t> bitmap = fromHex("4000870000");
  const EpdGlyph glyphs[] = {
      {2, 1, 0, 0, 0, 2, 0},
      {1, 1, 0, 0, 0, 3, 2},
  };
  const EpdUnicodeInterval intervals[] = {{0x41, 0x42, 0}};
  const EpdFontData font = makeFont(bitmap, glyphs, intervals, 1, false);

  EXPECT_FALSE(GlyphStreamCodec::decode(&font, 1, true, scratchA.data(), scratchB.data(), output.data(), 1));
}

TEST_F(GlyphStreamCodecTest, RejectsReferenceDepthThree) {
  const std::vector<uint8_t> bitmap = fromHex("4000870000870100870200");
  const EpdGlyph glyphs[] = {
      {1, 1, 0, 0, 0, 2, 0},
      {1, 1, 0, 0, 0, 3, 2},
      {1, 1, 0, 0, 0, 3, 5},
      {1, 1, 0, 0, 0, 3, 8},
  };
  const EpdUnicodeInterval intervals[] = {{0x41, 0x44, 0}};
  const EpdFontData font = makeFont(bitmap, glyphs, intervals, 1, false);

  EXPECT_FALSE(GlyphStreamCodec::decode(&font, 3, true, scratchA.data(), scratchB.data(), output.data(), 1));
}

TEST_F(GlyphStreamCodecTest, RejectsDimensionsBeyondScratchPlanes) {
  const std::vector<uint8_t> bitmap = fromHex("40ffffffffffffffff");
  const EpdGlyph glyphs[] = {{64, 1, 0, 0, 0, 9, 0}};
  const EpdUnicodeInterval intervals[] = {{0x41, 0x41, 0}};
  const EpdFontData font = makeFont(bitmap, glyphs, intervals, 1, false);

  EXPECT_FALSE(GlyphStreamCodec::decode(&font, 0, true, scratchA.data(), scratchB.data(), output.data(), 8));
}

}  // namespace
