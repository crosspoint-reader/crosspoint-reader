#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <vector>

#include "EpdFont.h"
#include "FontAlloc.h"
#include "FontDecompressor.h"
#include "builtinFonts/notosans_14_regular.h"
#include "builtinFonts/notosans_8_regular.h"
#include "builtinFonts/ubuntu_10_bold.h"
#include "builtinFonts/ubuntu_10_regular.h"
#include "builtinFonts/ubuntu_12_bold.h"
#include "builtinFonts/ubuntu_12_regular.h"

namespace {
bool failAllocation = false;
size_t liveAllocations = 0;
size_t largestAllocation = 0;

struct FontCase {
  const EpdFontData* data;
  uint32_t glyphCount;
  uint64_t bitmapHash;
};

// FNV-1a of the original uncompressed bitmaps, before enabling compression.
constexpr FontCase UI_FONTS[] = {
    {&notosans_8_regular, 1298, 0xa1edd1a981d66492ULL}, {&ubuntu_10_bold, 1313, 0xb24201ba64fc7bcaULL},
    {&ubuntu_10_regular, 1313, 0xe8cffa32a0a1e1a1ULL},  {&ubuntu_12_bold, 1313, 0x5c4b9b1083abff1cULL},
    {&ubuntu_12_regular, 1313, 0xa6086bf6b46a7ff6ULL},
};

class FontDecompressorTest : public testing::Test {
 protected:
  void SetUp() override {
    failAllocation = false;
    largestAllocation = 0;
    ASSERT_EQ(liveAllocations, 0u);
  }
  void TearDown() override {
    failAllocation = false;
    EXPECT_EQ(liveAllocations, 0u);
  }
};
}  // namespace

extern "C" void* fiFontMalloc(size_t size) {
  if (failAllocation) return nullptr;
  void* p = std::malloc(size);
  if (p) {
    ++liveAllocations;
    if (size > largestAllocation) largestAllocation = size;
  }
  return p;
}

extern "C" void fiFontFree(void* p) {
  if (p) --liveAllocations;
  std::free(p);
}

// Firmware discards uzlib's unused checksum wrapper; font groups are raw DEFLATE.
extern "C" uint32_t uzlib_adler32(const void*, unsigned int, uint32_t) {
  ADD_FAILURE() << "Font decompression requested a zlib checksum";
  return 0;
}
extern "C" uint32_t uzlib_crc32(const void*, unsigned int, uint32_t) {
  ADD_FAILURE() << "Font decompression requested a gzip checksum";
  return 0;
}

TEST_F(FontDecompressorTest, EveryUiGlyphMatchesOriginalPackedPixels) {
  FontDecompressor decoder;
  for (const auto& font : UI_FONTS) {
    uint64_t hash = 14695981039346656037ULL;
    for (uint32_t i = 0; i < font.glyphCount; ++i) {
      const auto& glyph = font.data->glyph[i];
      const auto* bits = decoder.getBitmap(font.data, &glyph, i);
      ASSERT_NE(bits, nullptr);
      for (uint16_t j = 0; j < glyph.dataLength; ++j) hash = (hash ^ bits[j]) * 1099511628211ULL;
    }
    EXPECT_EQ(hash, font.bitmapHash);
  }
  EXPECT_LE(largestAllocation, 2048u);
  EXPECT_EQ(liveAllocations, 1u);  // Shared hot group; 1-bit glyphs need no scratch buffer.
}

TEST_F(FontDecompressorTest, ReaderPrewarmMatchesHotGroup) {
  for (const EpdFontData* font : {&notosans_14_regular}) {
    FontDecompressor decoder;
    EpdFont lookup(font);
    // Include space and glyphs whose rows do not end on a byte boundary.
    const char* text = " !AMWag";
    std::vector<std::vector<uint8_t>> expected;
    for (const char* p = text; *p; ++p) {
      uint32_t index = lookup.getGlyph(static_cast<unsigned char>(*p)) - font->glyph;
      const auto& glyph = font->glyph[index];
      if (glyph.dataLength == 0) {
        expected.emplace_back();
      } else {
        const auto* bits = decoder.getBitmap(font, &glyph, index);
        ASSERT_NE(bits, nullptr);
        expected.emplace_back(bits, bits + glyph.dataLength);
      }
    }
    decoder.clearCache();
    ASSERT_EQ(decoder.prewarmCache(font, text), 0);
    const auto misses = decoder.getStats().cacheMisses;
    for (size_t i = 0; text[i]; ++i) {
      uint32_t index = lookup.getGlyph(static_cast<unsigned char>(text[i])) - font->glyph;
      const auto& glyph = font->glyph[index];
      const auto* bits = decoder.getBitmap(font, &glyph, index);
      ASSERT_NE(bits, nullptr);
      EXPECT_EQ(std::vector<uint8_t>(bits, bits + glyph.dataLength), expected[i]);
    }
    EXPECT_EQ(decoder.getStats().cacheMisses, misses);
  }
}

TEST_F(FontDecompressorTest, AllocationFailureCanRetryWithoutStaleGroupOrLeaks) {
  FontDecompressor decoder;
  const auto* font = &ubuntu_10_regular;
  EpdFont lookup(font);
  const auto* glyph = lookup.getGlyph('A');
  const auto index = static_cast<uint32_t>(glyph - font->glyph);
  failAllocation = true;
  EXPECT_EQ(decoder.getBitmap(font, glyph, index), nullptr);
  EXPECT_GT(decoder.prewarmCache(&notosans_14_regular, "A"), 0);
  EXPECT_EQ(liveAllocations, 0u);
  failAllocation = false;
  ASSERT_NE(decoder.getBitmap(font, glyph, index), nullptr);
  decoder.clearCache();
  EXPECT_EQ(liveAllocations, 0u);
  EXPECT_EQ(decoder.prewarmCache(font, "A"), 0);
}

TEST_F(FontDecompressorTest, UiPrewarmLeavesAllReaderStyleSlotsAvailable) {
  FontDecompressor decoder;
  for (const auto& font : UI_FONTS) EXPECT_EQ(decoder.prewarmCache(font.data, "Menu"), 0);
  EXPECT_EQ(liveAllocations, 0u);
  for (int i = 0; i < FontDecompressor::MAX_PAGE_SLOTS; ++i) {
    EXPECT_EQ(decoder.prewarmCache(&notosans_14_regular, "Reader"), 0);
  }
}
