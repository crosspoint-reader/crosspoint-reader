#include <FontPsram.h>
#include <HalMemory.h>
#include <HalStorage.h>
#include <SdCardFont.h>
#include <Utf8.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <string>

namespace {
size_t failNextArraySize = 0;
size_t nothrowArrayAllocations = 0;
}  // namespace

// Keep array allocation and deletion paired, including arrays allocated by the test framework.
void* operator new[](size_t size) {
  void* allocation = std::malloc(size == 0 ? 1 : size);
  if (!allocation) {
    std::fputs("Unexpected host OOM in SdCardFontTest\n", stderr);
    std::exit(EXIT_FAILURE);
  }
  return allocation;
}

void* operator new[](size_t size, const std::nothrow_t&) noexcept {
  ++nothrowArrayAllocations;
  if (failNextArraySize != 0 && size == failNextArraySize) {
    failNextArraySize = 0;
    return nullptr;
  }
  return std::malloc(size == 0 ? 1 : size);
}

void operator delete[](void* allocation) noexcept { std::free(allocation); }
void operator delete[](void* allocation, size_t) noexcept { std::free(allocation); }
void operator delete[](void* allocation, const std::nothrow_t&) noexcept { std::free(allocation); }

namespace {
constexpr uint32_t FIRST = 0xAC00;
constexpr uint32_t GLYPHS = 513;
constexpr uint16_t BITMAP_BYTES = 128;
constexpr size_t GLYPH_OFFSET = 64 + 24;

void put16(size_t at, uint16_t value) {
  sdFontTestFile[at] = value;
  sdFontTestFile[at + 1] = value >> 8;
}
void put32(size_t at, uint32_t value) {
  put16(at, value);
  put16(at + 2, value >> 16);
}

// Offset of a makeFont glyph record field.
size_t glyphField(uint32_t index, size_t field) { return GLYPH_OFFSET + index * sizeof(EpdGlyph) + field; }

void setGlyphBitmapLength(uint32_t index, uint16_t bytes) {
  sdFontTestFile[glyphField(index, offsetof(EpdGlyph, width))] = 8;
  sdFontTestFile[glyphField(index, offsetof(EpdGlyph, height))] = bytes;
  put16(glyphField(index, offsetof(EpdGlyph, dataLength)), bytes);
}

// `glyphs` Hangul glyphs from FIRST, the last of them U+FFFD.
void makeFont(uint32_t glyphs = GLYPHS) {
  const size_t BITMAP_OFFSET = GLYPH_OFFSET + glyphs * sizeof(EpdGlyph);
  sdFontTestFile.assign(BITMAP_OFFSET + glyphs * BITMAP_BYTES, 0);
  std::memcpy(sdFontTestFile.data(), "CPFONT\0\0", 8);
  put16(8, CPFONT_VERSION);
  sdFontTestFile[12] = 1;
  put32(36, 2);
  put32(40, glyphs);
  sdFontTestFile[44] = 32;
  put16(45, 32);
  put32(56, 64);
  put32(64, FIRST);
  put32(68, FIRST + glyphs - 2);
  put32(76, 0xFFFD);
  put32(80, 0xFFFD);
  put32(84, glyphs - 1);
  for (uint32_t i = 0; i < glyphs; ++i) {
    EpdGlyph glyph{};
    glyph.width = 32;
    glyph.height = 32;
    glyph.advanceX = 32 << 4;
    glyph.top = 32;
    glyph.dataLength = BITMAP_BYTES;
    // Store bitmaps in reverse glyph order to exercise sorted reads on rebuild.
    glyph.dataOffset = (glyphs - 1 - i) * BITMAP_BYTES;
    std::memcpy(sdFontTestFile.data() + GLYPH_OFFSET + i * sizeof(glyph), &glyph, sizeof(glyph));
    std::memset(sdFontTestFile.data() + BITMAP_OFFSET + glyph.dataOffset, i % 251, BITMAP_BYTES);
  }
}

// Six Latin glyphs 'A'..'F' plus U+FFFD, with left classes for 'A' and 'C',
// right classes for 'B' and 'D', and a class matrix whose top-left 2×2 corner
// holds their pairs. `extraEntries` appends entries for U+0100 onwards to both
// class tables, so the tables span several read blocks. They use `extraClass`,
// or classes 3..classCount in turn when it is 0. A `classCount` of 0 writes no
// kern data. `ligature` adds one pair, E+F -> A.
void makeKerningFont(uint16_t extraEntries = 0, uint8_t classCount = 2, uint8_t extraClass = 2, bool ligature = false) {
  constexpr uint32_t KERN_GLYPHS = 7;
  constexpr size_t GLYPH_OFFSET = 64 + 24;
  constexpr size_t KERN_OFFSET = GLYPH_OFFSET + KERN_GLYPHS * sizeof(EpdGlyph);
  constexpr uint8_t LEFT[][2] = {{'A', 1}, {'C', 2}};
  constexpr uint8_t RIGHT[][2] = {{'B', 1}, {'D', 2}};
  constexpr int8_t MATRIX[2][2] = {{-3, 0}, {4, -5}};
  const uint16_t entries = classCount ? 2 + extraEntries : 0;
  const size_t matrixBytes = static_cast<size_t>(classCount) * classCount;
  const size_t ligatureOffset = KERN_OFFSET + entries * 3 * 2 + matrixBytes;
  const size_t bitmapOffset = ligatureOffset + (ligature ? 8 : 0);
  sdFontTestFile.assign(bitmapOffset + KERN_GLYPHS * BITMAP_BYTES, 0);
  std::memcpy(sdFontTestFile.data(), "CPFONT\0\0", 8);
  put16(8, CPFONT_VERSION);
  sdFontTestFile[12] = 1;
  put32(36, 2);
  put32(40, KERN_GLYPHS);
  sdFontTestFile[44] = 32;
  put16(45, 32);
  put16(49, entries);  // left class entries
  put16(51, entries);  // right class entries
  sdFontTestFile[53] = classCount;
  sdFontTestFile[54] = classCount;
  sdFontTestFile[55] = ligature ? 1 : 0;
  put32(56, 64);
  put32(64, 'A');
  put32(68, 'F');
  put32(76, 0xFFFD);
  put32(80, 0xFFFD);
  put32(84, KERN_GLYPHS - 1);
  for (uint32_t i = 0; i < KERN_GLYPHS; ++i) {
    EpdGlyph glyph{};
    glyph.width = 32;
    glyph.height = 32;
    glyph.advanceX = 32 << 4;
    glyph.top = 32;
    glyph.dataLength = BITMAP_BYTES;
    glyph.dataOffset = i * BITMAP_BYTES;
    std::memcpy(sdFontTestFile.data() + GLYPH_OFFSET + i * sizeof(glyph), &glyph, sizeof(glyph));
  }
  size_t at = KERN_OFFSET;
  for (const auto* table : {LEFT, RIGHT}) {
    if (classCount == 0) break;
    for (size_t i = 0; i < 2; ++i, at += 3) {
      put16(at, table[i][0]);
      sdFontTestFile[at + 2] = table[i][1];
    }
    for (uint16_t i = 0; i < extraEntries; ++i, at += 3) {
      put16(at, 0x100 + i);
      sdFontTestFile[at + 2] = extraClass ? extraClass : 3 + i % (classCount - 2);
    }
  }
  for (size_t row = 0; classCount > 0 && row < 2; ++row) {
    std::memcpy(sdFontTestFile.data() + at + row * classCount, MATRIX[row], sizeof(MATRIX[row]));
  }
  if (ligature) {
    put32(ligatureOffset, 'E' << 16 | 'F');
    put32(ligatureOffset + 4, 'A');
  }
}

std::string latinPage(const char* ascii, uint32_t first, uint32_t count) {
  std::string text = ascii;
  for (uint32_t cp = first; cp < first + count; ++cp) {
    text.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    text.push_back(static_cast<char>(0x80 | (cp & 63)));
  }
  return text;
}

std::string page(uint32_t first, uint32_t count) {
  std::string text;
  text.reserve(count * 3);
  for (uint32_t cp = first; cp < first + count; ++cp) {
    text.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    text.push_back(static_cast<char>(0x80 | ((cp >> 6) & 63)));
    text.push_back(static_cast<char>(0x80 | (cp & 63)));
  }
  return text;
}

// Checks each glyph's length and bitmap bytes against the makeFont fixture.
void expectPageBitmaps(SdCardFont& font, uint32_t first, uint32_t count) {
  const auto* data = font.getEpdFont()->data;
  ASSERT_EQ(0U, data->intervalCount);
  ASSERT_NE(nullptr, data->vectorBitmapHandler);
  for (uint32_t cp = first; cp < first + count; ++cp) {
    const EpdGlyph* glyph = font.getEpdFont()->getGlyph(cp);
    ASSERT_NE(nullptr, glyph) << cp;
    const size_t length = glyphField(cp - FIRST, offsetof(EpdGlyph, dataLength));
    ASSERT_EQ(sdFontTestFile[length] | sdFontTestFile[length + 1] << 8, glyph->dataLength) << cp;
    const uint8_t* bitmap = data->vectorBitmapHandler(data->glyphMissCtx, glyph);
    if (glyph->dataLength == 0) {
      ASSERT_EQ(nullptr, bitmap) << cp;
      continue;
    }
    ASSERT_NE(nullptr, bitmap) << cp;
    const uint8_t fill = (cp - FIRST) % 251;
    ASSERT_TRUE(std::all_of(bitmap, bitmap + glyph->dataLength, [&](uint8_t b) { return b == fill; })) << cp;
  }
}
// A cache boundary as FontCacheManager::clearCache() drives it: the shared pool first, then the font.
void cacheBoundary(SdGlyphPool& pool, SdCardFont& font) {
  pool.clearCache();
  font.clearCache();
}
}  // namespace

TEST(SdCardFontTest, HitsReadNothingAndPrewarmReadsOnlyMissingGlyphs) {
  makeFont();
  SdGlyphPool pool;
  SdCardFont font(&pool);
  ASSERT_TRUE(font.load("fixture"));
  ASSERT_EQ(0, font.prewarm(page(FIRST, 100).c_str(), 1, false, false, false));
  cacheBoundary(pool, font);
  font.resetStats();
  ASSERT_EQ(0, font.prewarm(page(FIRST + 5, 100).c_str(), 1, false, false, false));
  EXPECT_EQ(5U, font.getStats().uniqueGlyphs);
  EXPECT_EQ(5U * BITMAP_BYTES, font.getStats().bitmapBytes);
  sdFontTestReads = 0;
  expectPageBitmaps(font, FIRST + 5, 100);
  EXPECT_EQ(0U, sdFontTestReads);
  EXPECT_TRUE(font.getEpdFont()->hasCodepoint(FIRST + 400));
  EXPECT_FALSE(font.getEpdFont()->hasCodepoint(0x100));
}

TEST(SdCardFontTest, CompletePagesEvictTheOldestUnpinnedChunks) {
  makeFont();
  SdGlyphPool pool;
  SdCardFont font(&pool);
  ASSERT_TRUE(font.load("fixture"));
  ASSERT_EQ(0, font.prewarm(page(FIRST, 26).c_str(), 1, false, false, false));
  cacheBoundary(pool, font);
  ASSERT_EQ(0, font.prewarm(page(FIRST + 26, 26).c_str(), 1, false, false, false));
  sdFontTestReads = 0;
  expectPageBitmaps(font, FIRST + 26, 26);
  EXPECT_EQ(0U, sdFontTestReads);
  expectPageBitmaps(font, FIRST, 1);
  EXPECT_GT(sdFontTestReads, 0U);
}

TEST(SdCardFontTest, PinnedTailDoesNotStallPrewarmAndRepeatedPagesReadNothing) {
  makeFont();
  SdGlyphPool pool;
  SdCardFont font(&pool);
  ASSERT_TRUE(font.load("fixture"));
  ASSERT_EQ(0, font.prewarm(page(FIRST, 100).c_str(), 1, false, false, false));
  for (uint32_t offset : {100U, 200U, 300U, 400U, 0U}) {
    const auto text = page(FIRST, 1) + page(FIRST + offset, 100);
    cacheBoundary(pool, font);
    ASSERT_EQ(0, font.prewarm(text.c_str(), 1, false, false, false));
    cacheBoundary(pool, font);
    cacheBoundary(pool, font);
    sdFontTestReads = 0;
    ASSERT_EQ(0, font.prewarm(text.c_str(), 1, false, false, false));
    expectPageBitmaps(font, FIRST + offset, 100);
    EXPECT_EQ(0U, sdFontTestReads);
  }
}

TEST(SdCardFontTest, GenerationPinsKeepPointersStableAcrossDrawTimeMisses) {
  makeFont();
  SdGlyphPool pool;
  SdCardFont font(&pool);
  ASSERT_TRUE(font.load("fixture"));
  ASSERT_EQ(0, font.prewarm(page(FIRST, 20).c_str(), 1, false, false, false));
  const EpdGlyph* pinned = font.getEpdFont()->getGlyph(FIRST);
  const auto* data = font.getEpdFont()->data;
  const uint8_t* bitmap = data->vectorBitmapHandler(data->glyphMissCtx, pinned);
  for (uint32_t i = 20; i < 300; ++i) expectPageBitmaps(font, FIRST + i, 1);
  EXPECT_EQ(pinned, font.getEpdFont()->getGlyph(FIRST));
  EXPECT_EQ(bitmap, data->vectorBitmapHandler(data->glyphMissCtx, pinned));
  EXPECT_EQ(0, bitmap[0]);
  EXPECT_GT(font.getStats().blockedInsertions, 0U);
}

TEST(SdCardFontTest, AllPinnedPrewarmReportsMissesAndFallbackSurvivesHits) {
  makeFont();
  SdGlyphPool pool;
  SdCardFont font(&pool);
  ASSERT_TRUE(font.load("fixture"));
  ASSERT_EQ(0, font.prewarm(page(FIRST, 20).c_str(), 1, false, false));
  pool.beginScope();
  EXPECT_GT(font.prewarm(page(FIRST, 300).c_str(), 1, false, false), 0);
  const auto* glyph = font.getEpdFont()->getGlyph(FIRST + 300);
  const auto* data = font.getEpdFont()->data;
  const auto* bitmap = data->vectorBitmapHandler(data->glyphMissCtx, glyph);
  expectPageBitmaps(font, FIRST, 1);
  EXPECT_EQ(glyph->advanceX, font.getEpdFont()->getGlyph(FIRST + 300)->advanceX);
  EXPECT_EQ(300 % 251, bitmap[0]);
  EXPECT_GT(font.getStats().overflowHits, 0U);
  expectPageBitmaps(font, FIRST + 301, 1);
}

TEST(SdCardFontTest, MetricMissesAllocateOnlyWhenDrawn) {
  makeFont();
  SdGlyphPool pool;
  SdCardFont font(&pool);
  ASSERT_TRUE(font.load("fixture"));
  const auto allocations = sdFontPsramAllocations;
  for (uint32_t i = 0; i < GLYPHS; ++i) {
    const auto* glyph = font.getEpdFont()->getGlyph(FIRST + i);
    ASSERT_NE(nullptr, glyph);
    EXPECT_EQ(BITMAP_BYTES, glyph->dataLength);
  }
  EXPECT_EQ(allocations, sdFontPsramAllocations);
  EXPECT_EQ(0U, font.getStats().bitmapBytes);
  const auto* glyph = font.getEpdFont()->getGlyph(FIRST + 300);
  const auto* data = font.getEpdFont()->data;
  const auto* bitmap = data->vectorBitmapHandler(data->glyphMissCtx, glyph);
  ASSERT_NE(nullptr, bitmap);
  EXPECT_EQ(300 % 251, bitmap[0]);
  const auto drawnAllocations = sdFontPsramAllocations;
  sdFontTestReads = 0;
  EXPECT_EQ(bitmap, data->vectorBitmapHandler(data->glyphMissCtx, glyph));
  EXPECT_EQ(drawnAllocations, sdFontPsramAllocations);
  EXPECT_EQ(0U, sdFontTestReads);
}

TEST(SdCardFontTest, IncrementalUiPrewarmsRetainEarlierStrings) {
  makeFont();
  SdGlyphPool pool;
  SdCardFont font(&pool);
  ASSERT_TRUE(font.load("fixture"));
  for (uint32_t i = 0; i < 20; i += 5) ASSERT_EQ(0, font.prewarm(page(FIRST + i, 5).c_str(), 1, false, false));
  sdFontTestReads = 0;
  expectPageBitmaps(font, FIRST, 20);
  EXPECT_EQ(0U, sdFontTestReads);
}

TEST(SdCardFontTest, MetadataPrewarmUpgradesBitmapsWithoutMovingMetrics) {
  makeFont();
  SdGlyphPool pool;
  SdCardFont font(&pool);
  ASSERT_TRUE(font.load("fixture"));
  ASSERT_EQ(0, font.prewarm(page(FIRST, 20).c_str(), 1, true, false));
  EXPECT_EQ(0U, font.getStats().bitmapBytes);
  font.resetStats();
  ASSERT_EQ(0, font.prewarm(page(FIRST, 20).c_str(), 1, false, false));
  EXPECT_EQ(0U, font.getStats().uniqueGlyphs);
  EXPECT_EQ(21U * BITMAP_BYTES, font.getStats().bitmapBytes);
  expectPageBitmaps(font, FIRST, 20);
}

TEST(SdCardFontTest, ScratchIndexAndChunkAllocationFailuresRecoverWithoutLeaks) {
  makeFont();
  for (int failAfter : {0, 1, 2}) {  // scratch, index, first chunk
    const size_t before = sdFontPsramAllocations - sdFontPsramFrees;
    {
      SdGlyphPool pool;
      SdCardFont font(&pool);
      ASSERT_TRUE(font.load("fixture"));
      sdFontPsramFailAfter = failAfter;
      const int result = font.prewarm(page(FIRST, 20).c_str(), 1, false, false, false);
      EXPECT_EQ(failAfter == 0 ? -1 : 1, result);
      EXPECT_EQ(-1, sdFontPsramFailAfter);
      expectPageBitmaps(font, FIRST, 20);
    }
    EXPECT_EQ(before, sdFontPsramAllocations - sdFontPsramFrees);
  }
}

TEST(SdCardFontTest, PoolAllocationFailurePreservesAdvanceMetricsAndRecovers) {
  makeFont();
  SdGlyphPool pool;
  SdCardFont font(&pool);
  ASSERT_TRUE(font.load("fixture"));
  const auto text = page(FIRST, 30);  // with U+FFFD, fills the first 32-entry scratch
  ASSERT_EQ(0, font.buildAdvanceTable(text.c_str(), 1));
  ASSERT_TRUE(font.hasAdvanceTable());
  sdFontPsramFailAfter = 2;  // scratch and index succeed, then the first chunk fails
  ASSERT_EQ(1, font.prewarm(text.c_str(), 1, false, false, false));
  EXPECT_EQ(-1, sdFontPsramFailAfter);
  EXPECT_TRUE(font.hasAdvanceTable());
  ASSERT_EQ(0, font.prewarm(text.c_str(), 1, false, false, false));
  sdFontTestReads = 0;
  expectPageBitmaps(font, FIRST, 30);
  EXPECT_EQ(0U, sdFontTestReads);
}

TEST(SdCardFontTest, ConvergedChunksDoNotAllocateAcrossReadingTurns) {
  makeFont();
  SdGlyphPool pool;
  SdCardFont font(&pool);
  ASSERT_TRUE(font.load("fixture"));
  ASSERT_EQ(0, font.prewarm(page(FIRST, 100).c_str(), 1, false, false, false));
  // Fill all nine chunks once, so subsequent varied page sets cannot require growth.
  ASSERT_GT(font.prewarm(page(FIRST, 300).c_str(), 1, false, false, false), 0);
  cacheBoundary(pool, font);
  const size_t allocations = nothrowArrayAllocations;
  const size_t frees = sdFontPsramFrees;
  for (uint32_t turn = 0; turn < 12; ++turn) {
    cacheBoundary(pool, font);
    const uint32_t first = FIRST + (turn * 25) % 400;
    ASSERT_EQ(0, font.prewarm(page(first, 100).c_str(), 1, false, false, false));
    expectPageBitmaps(font, first, 100);
  }
  EXPECT_EQ(allocations, nothrowArrayAllocations);
  EXPECT_EQ(frees, sdFontPsramFrees);
}

TEST(SdCardFontTest, ExplicitReleaseAndPressureFreeArenaButKeepCoverage) {
  makeFont();
  const size_t before = sdFontPsramAllocations - sdFontPsramFrees;
  SdGlyphPool pool;
  SdCardFont font(&pool);
  ASSERT_TRUE(font.load("fixture"));
  const size_t loaded = sdFontPsramAllocations - sdFontPsramFrees;
  for (bool pressure : {false, true}) {
    ASSERT_EQ(0, font.prewarm(page(FIRST, 100).c_str(), 1, false, false, false));
    if (pressure) {
      ESP.freeHeap = 30 * 1024;
      cacheBoundary(pool, font);
      ESP.freeHeap = 200 * 1024;
    } else {
      font.releaseResidentCaches();
    }
    EXPECT_EQ(loaded, sdFontPsramAllocations - sdFontPsramFrees);
    EXPECT_TRUE(font.getEpdFont()->hasCodepoint(FIRST));
    expectPageBitmaps(font, FIRST, 1);
    font.releaseResidentCaches();
  }
  EXPECT_GT(loaded, before);
}

TEST(SdCardFontTest, PagesKernWithTheFontsClassMatrix) {
  makeKerningFont();
  SdGlyphPool pool;
  SdCardFont font(&pool);
  ASSERT_TRUE(font.load("fixture"));
  ASSERT_EQ(0, font.prewarm("ABCDEF", 1, false, true, false));
  const EpdFont* epd = font.getEpdFont();
  EXPECT_EQ(-3, epd->getKerning('A', 'B'));
  EXPECT_EQ(0, epd->getKerning('A', 'D'));
  EXPECT_EQ(4, epd->getKerning('C', 'B'));
  EXPECT_EQ(-5, epd->getKerning('C', 'D'));
  EXPECT_EQ(0, epd->getKerning('B', 'D'));
  EXPECT_EQ(0, epd->getKerning('E', 'B'));
}

TEST(SdCardFontTest, KernRequestsServedFromAKernFreeArenaStillKern) {
  makeKerningFont();
  SdGlyphPool pool;
  SdCardFont font(&pool);
  ASSERT_TRUE(font.load("fixture"));
  ASSERT_EQ(0, font.prewarm("ABCDEF", 1, false, false, false));  // kern-free prewarm, e.g. a UI string
  struct Step {
    const char* text;
    uint32_t left, right;
    int8_t kern;
  };
  // A subset without kerning pairs, then subsets whose pairs the earlier ones did not cover.
  for (const Step& step : {Step{"EF", 'E', 'F', 0}, Step{"AB", 'A', 'B', -3}, Step{"CD", 'C', 'D', -5}}) {
    ASSERT_EQ(0, font.prewarm(step.text, 1, false, true, false));
    EXPECT_EQ(step.kern, font.getEpdFont()->getKerning(step.left, step.right)) << step.text;
  }
}

TEST(SdCardFontTest, RedrawsAfterAKernFreeRebuildStillKern) {
  makeKerningFont();
  SdGlyphPool pool;
  SdCardFont font(&pool);
  ASSERT_TRUE(font.load("fixture"));
  ASSERT_EQ(0, font.prewarm("ABCD", 1, false, true, false));
  EXPECT_GE(font.prewarm("ABCDEF", 1, false, false, false), 0);  // bounded arena may use the miss path
  ASSERT_EQ(0, font.prewarm("ABCD", 1, false, true, false));     // the page again, served from that cache
  EXPECT_EQ(-3, font.getEpdFont()->getKerning('A', 'B'));
  EXPECT_EQ(-5, font.getEpdFont()->getKerning('C', 'D'));
}

TEST(SdCardFontTest, ClassIdsPastTheMatrixAreUnkerned) {
  makeKerningFont(1, 2, 250);  // U+0100 claims class 250 in a 2×2 matrix
  SdGlyphPool pool;
  SdCardFont font(&pool);
  ASSERT_TRUE(font.load("fixture"));
  ASSERT_EQ(1, font.prewarm(latinPage("ABCD", 0x100, 1).c_str(), 1, false, true, false));  // U+0100 has no glyph
  const EpdFont* epd = font.getEpdFont();
  EXPECT_EQ(0, epd->getKerning('A', 0x100));
  EXPECT_EQ(0, epd->getKerning(0x100, 'B'));
  EXPECT_EQ(-3, epd->getKerning('A', 'B'));
  EXPECT_EQ(-5, epd->getKerning('C', 'D'));
}

TEST(SdCardFontTest, PagesCanUseAll255KernClasses) {
  makeKerningFont(253, 255, 0);
  SdGlyphPool pool;
  SdCardFont font(&pool);
  ASSERT_TRUE(font.load("fixture"));
  ASSERT_EQ(253, font.prewarm(latinPage("ABCD", 0x100, 253).c_str(), 1, false, true, false));
  const EpdFont* epd = font.getEpdFont();
  EXPECT_EQ(-3, epd->getKerning('A', 'B'));
  EXPECT_EQ(-5, epd->getKerning('C', 'D'));
  EXPECT_EQ(0, epd->getKerning(0x100, 0x1FC));
}

TEST(SdCardFontTest, AFailedKernBuildKeepsTheLigatures) {
  makeKerningFont(253, 255, 0, true);
  SdGlyphPool pool;
  SdCardFont font(&pool);
  ASSERT_TRUE(font.load("fixture"));
  failNextArraySize = 255 * 255;  // the mini kern matrix
  ASSERT_EQ(253, font.prewarm(latinPage("ABCDEF", 0x100, 253).c_str(), 1, false, true, false));
  EXPECT_EQ(0U, failNextArraySize);
  const EpdFont* epd = font.getEpdFont();
  EXPECT_EQ(static_cast<uint32_t>('A'), epd->getLigature('E', 'F'));
  EXPECT_EQ(0, epd->getKerning('A', 'B'));
}

TEST(SdCardFontTest, LigatureRequestsServedFromAKernFreeArenaGetLigatures) {
  makeKerningFont(0, 0, 2, true);  // ligatures, no kern classes
  SdGlyphPool pool;
  SdCardFont font(&pool);
  ASSERT_TRUE(font.load("fixture"));
  ASSERT_EQ(0, font.prewarm("AEF", 1, false, false, false));  // kern-free prewarm, e.g. a UI string
  ASSERT_EQ(0, font.prewarm("EF", 1, false, true, false));
  EXPECT_EQ(static_cast<uint32_t>('A'), font.getEpdFont()->getLigature('E', 'F'));
}

TEST(SdCardFontTest, RedrawingAPrewarmedPageReadsNothing) {
  makeKerningFont();
  SdGlyphPool pool;
  SdCardFont font(&pool);
  ASSERT_TRUE(font.load("fixture"));
  for (const char* text : {"ABCD", "DEF"}) {  // with and without kerning pairs
    cacheBoundary(pool, font);
    ASSERT_EQ(0, font.prewarm(text, 1, false, true, false));
    cacheBoundary(pool, font);
    sdFontTestReads = 0;
    ASSERT_EQ(0, font.prewarm(text, 1, false, true, false));
    EXPECT_EQ(0U, sdFontTestReads) << text;
  }
}

TEST(SdCardFontTest, LaterPagesReadOnlyTheKernClassBlocksTheyUse) {
  makeKerningFont(300);  // five 64-entry blocks per class table
  SdGlyphPool pool;
  SdCardFont font(&pool);
  ASSERT_TRUE(font.load("fixture"));
  sdFontTestReads = 0;
  ASSERT_EQ(0, font.prewarm("ABCDEF", 1, false, true, false));
  const size_t firstReads = sdFontTestReads;
  EXPECT_EQ(-5, font.getEpdFont()->getKerning('C', 'D'));

  font.releaseResidentCaches();
  sdFontTestReads = 0;
  ASSERT_EQ(0, font.prewarm("ABCDEF", 1, false, true, false));
  EXPECT_EQ(firstReads - 8, sdFontTestReads);  // 4 of the 5 blocks skipped in each table
  const EpdFont* epd = font.getEpdFont();
  EXPECT_EQ(-3, epd->getKerning('A', 'B'));
  EXPECT_EQ(4, epd->getKerning('C', 'B'));
  EXPECT_EQ(-5, epd->getKerning('C', 'D'));
}

TEST(SdCardFontTest, AdvancesStayCorrectWhenPagesAddCodepointsOutOfOrder) {
  makeFont();
  for (uint32_t i = 0; i < GLYPHS; ++i) {
    put16(64 + 24 + i * sizeof(EpdGlyph) + offsetof(EpdGlyph, advanceX), (20 + i % 13) << 4);
  }
  SdGlyphPool pool;
  SdCardFont font(&pool);
  ASSERT_TRUE(font.load("fixture"));
  std::vector<uint32_t> added;
  for (uint32_t block : {3U, 0U, 5U, 1U, 4U, 2U}) {  // each merge lands before, between or after earlier ones
    ASSERT_EQ(0, font.buildAdvanceTable(page(FIRST + block * 80, 80).c_str(), 1));
    for (uint32_t cp = FIRST + block * 80; cp < FIRST + block * 80 + 80; ++cp) added.push_back(cp);
    for (uint32_t cp : added) {
      ASSERT_EQ((20 + (cp - FIRST) % 13) << 4, font.getAdvance(cp, 0)) << std::hex << cp;
    }
  }
}

TEST(SdCardFontTest, AdvanceMergesPastTheCapKeepTheLowestCodepoints) {
  constexpr uint32_t CACHE_LIMIT = 768;  // SdCardFont::ADVANCE_CACHE_LIMIT
  constexpr uint32_t GLYPH_COUNT = 1001;
  makeFont(GLYPH_COUNT);
  for (uint32_t i = 0; i < GLYPH_COUNT; ++i) {
    put16(64 + 24 + i * sizeof(EpdGlyph) + offsetof(EpdGlyph, advanceX), (20 + i % 13) << 4);
  }
  SdGlyphPool pool;
  SdCardFont font(&pool);
  ASSERT_TRUE(font.load("fixture"));
  // Reference: the sorted union of every page, truncated to the cap; a full table takes nothing more.
  std::vector<uint32_t> expected;
  for (uint32_t block : {6U, 1U, 9U, 3U, 0U, 7U, 4U, 8U, 2U, 5U}) {
    ASSERT_GE(font.buildAdvanceTable(page(FIRST + block * 100, 100).c_str(), 1), 0);
    if (expected.size() < CACHE_LIMIT) {
      for (uint32_t cp = FIRST + block * 100; cp < FIRST + block * 100 + 100; ++cp) expected.push_back(cp);
      std::sort(expected.begin(), expected.end());
      if (expected.size() > CACHE_LIMIT) expected.resize(CACHE_LIMIT);
    }
    for (uint32_t cp = FIRST; cp < FIRST + 1000; ++cp) {
      const bool cached = std::binary_search(expected.begin(), expected.end(), cp);
      ASSERT_EQ(cached ? (20 + (cp - FIRST) % 13) << 4 : 0, font.getAdvance(cp, 0)) << block << " " << std::hex << cp;
    }
  }
}

TEST(SdCardFontTest, AnEmptyOrFailedAdvanceBuildLeavesNoTable) {
  makeFont();
  SdGlyphPool pool;
  SdCardFont font(&pool);
  ASSERT_TRUE(font.load("fixture"));
  font.buildAdvanceTable("", 1);
  EXPECT_FALSE(font.hasAdvanceTable());
  failNextArraySize = (768 + 2) * sizeof(uint32_t);  // the codepoint scratch
  const std::string largeText = page(FIRST, 10) + std::string(4096, ' ');
  EXPECT_EQ(-1, font.buildAdvanceTable(largeText.c_str(), 1));
  EXPECT_FALSE(font.hasAdvanceTable());
  ASSERT_EQ(0, font.buildAdvanceTable(page(FIRST, 10).c_str(), 1));
  EXPECT_TRUE(font.hasAdvanceTable());
}

TEST(SdCardFontTest, AdvancesBuildForAStyleStoredInAnotherSlot) {
  // An italic-only font: style 2 lives in slot 0 of the compact style array.
  makeFont();
  sdFontTestFile[32] = 2;
  SdGlyphPool pool;
  SdCardFont font(&pool);
  ASSERT_TRUE(font.load("fixture"));
  ASSERT_TRUE(font.hasStyle(2));
  ASSERT_FALSE(font.hasStyle(0));
  const auto text = page(FIRST, 100);
  ASSERT_EQ(0, font.buildAdvanceTable(text.c_str(), 1 << 2));
  for (uint32_t cp = FIRST; cp < FIRST + 100; ++cp) {
    EXPECT_EQ(32 << 4, font.getAdvance(cp, 2));
  }
}

TEST(SdCardFontTest, AFullAdvanceTableSkipsTheScanWithoutAllocating) {
  makeFont(1001);
  SdGlyphPool pool;
  SdCardFont font(&pool);
  ASSERT_TRUE(font.load("fixture"));
  ASSERT_EQ(0, font.buildAdvanceTable(page(FIRST, 768).c_str(), 1));
  ASSERT_EQ(32U << 4, font.getAdvance(FIRST + 767, 0));
  const size_t allocationsBefore = nothrowArrayAllocations;
  EXPECT_EQ(0, font.buildAdvanceTable(page(FIRST + 768, 200).c_str(), 1));
  EXPECT_EQ(allocationsBefore, nothrowArrayAllocations);
  EXPECT_EQ(0, font.getAdvance(FIRST + 768, 0));
}

TEST(SdCardFontTest, OnePageWithMoreCodepointsThanTheTableKeepsTheLowestInAnyOrder) {
  constexpr uint32_t CACHE_LIMIT = 768;  // SdCardFont::ADVANCE_CACHE_LIMIT
  makeFont(1001);
  SdGlyphPool pool;
  SdCardFont font(&pool);
  ASSERT_TRUE(font.load("fixture"));
  // The top 768 first (ascending, so the largest is not the first held), then
  // the rest descending: each must displace the largest held. Every codepoint
  // appears twice, so repeats must not count twice.
  std::string text;
  for (uint32_t cp = FIRST + 232; cp < FIRST + 1000; ++cp) text += page(cp, 1) + page(cp, 1);
  for (uint32_t cp = FIRST + 231; cp + 1 > FIRST; --cp) text += page(cp, 1) + page(cp, 1);
  ASSERT_EQ(0, font.buildAdvanceTable(text.c_str(), 1));
  for (uint32_t cp = FIRST; cp < FIRST + 1000; ++cp) {
    ASSERT_EQ(cp < FIRST + CACHE_LIMIT ? 32U << 4 : 0U, font.getAdvance(cp, 0)) << std::hex << cp;
  }
}

TEST(SdCardFontTest, CodepointsTheFontLacksDoNotTakeTableSlotsWithoutAReplacementGlyph) {
  constexpr uint32_t CACHE_LIMIT = 768;  // SdCardFont::ADVANCE_CACHE_LIMIT
  makeFont(1001);
  put32(76, 0xFFFC);  // the last glyph is U+FFFC instead of U+FFFD
  put32(80, 0xFFFC);
  SdGlyphPool pool;
  SdCardFont font(&pool);
  ASSERT_TRUE(font.load("fixture"));
  // 300 codepoints below FIRST that the font lacks, each twice, then a full
  // table's worth it has. Only the latter can be cached, so all should be.
  const std::string lacking = page(0x4E00, 300);
  const std::string text = lacking + lacking + page(FIRST, CACHE_LIMIT);
  EXPECT_EQ(300, font.buildAdvanceTable(text.c_str(), 1));
  for (uint32_t cp = FIRST; cp < FIRST + CACHE_LIMIT; ++cp) {
    ASSERT_EQ(32U << 4, font.getAdvance(cp, 0)) << std::hex << cp;
  }
}

TEST(SdCardFontTest, ShortAdvancePreparationAvoidsLargeScratchAndRepeatedReads) {
  makeFont();
  SdGlyphPool pool;
  SdCardFont font(&pool);
  ASSERT_TRUE(font.load("fixture"));
  const std::string text = page(FIRST, 3);
  failNextArraySize = (768 + 2) * sizeof(uint32_t);
  const int result = font.buildAdvanceTable(text.c_str(), 1);
  failNextArraySize = 0;
  ASSERT_EQ(result, 0);
  for (uint32_t cp = FIRST; cp < FIRST + 3; ++cp) EXPECT_EQ(32 << 4, font.getAdvance(cp, 0));
  sdFontTestReads = 0;
  ASSERT_EQ(font.buildAdvanceTable(text.c_str(), 1), 0);
  EXPECT_EQ(sdFontTestReads, 0u);
}

TEST(SdCardFontTest, SmallCapsPrepareOriginalAndUppercasePackedMetricsWithoutBitmaps) {
  const struct {
    uint32_t first;
    uint32_t last;
    const char* words;
    size_t bytes;
  } cases[] = {
      {0x20, 0x7F, "ab\0z", 5}, {0x0391, 0x03CB, "\xCE\xB1\0\xCE\xB2", 6}, {0x0410, 0x044F, "\xD0\xB0\0\xD1\x8F", 6}};
  for (const auto& fixture : cases) {
    SCOPED_TRACE(fixture.first);
    makeFont(fixture.last - fixture.first + 2);
    put32(64, fixture.first);
    put32(68, fixture.last);
    for (uint32_t cp = fixture.first; cp <= fixture.last; ++cp) {
      put16(64 + 24 + (cp - fixture.first) * sizeof(EpdGlyph) + offsetof(EpdGlyph, advanceX), (10 + cp - fixture.first)
                                                                                                  << 4);
    }
    SdGlyphPool pool;
    SdCardFont font(&pool);
    ASSERT_TRUE(font.load("fixture"));
    const char* segments[] = {fixture.words};
    const size_t lengths[] = {fixture.bytes};
    ASSERT_EQ(font.buildAdvanceTablePacked(segments, lengths, 1, false, false, 1), 0);
    auto* cursor = reinterpret_cast<const uint8_t*>(fixture.words);
    EXPECT_EQ(font.getAdvance(utf8SmallCapsUpper(utf8NextCodepoint(&cursor)), 0), 0);
    ASSERT_EQ(font.buildAdvanceTablePacked(segments, lengths, 1, false, false, 1, nullptr, true), 0);
    cursor = reinterpret_cast<const uint8_t*>(fixture.words);
    const auto* end = cursor + fixture.bytes;
    while (cursor < end) {
      if (*cursor == 0) {
        ++cursor;
        continue;
      }
      const uint32_t cp = utf8NextCodepoint(&cursor);
      EXPECT_EQ(font.getAdvance(cp, 0), (10 + cp - fixture.first) << 4);
      const uint32_t upper = utf8SmallCapsUpper(cp);
      EXPECT_EQ(font.getAdvance(upper, 0), (10 + upper - fixture.first) << 4);
    }
    EXPECT_EQ(font.getStats().bitmapBytes, 0u);
    sdFontTestReads = 0;
    ASSERT_EQ(font.buildAdvanceTablePacked(segments, lengths, 1, false, false, 1, nullptr, true), 0);
    EXPECT_EQ(sdFontTestReads, 0u);
  }
}

TEST(SdCardFontTest, FontsWithTheSameCoverageShareOneIntervalTable) {
  makeFont();
  size_t firstLoad = 0;
  size_t secondLoad = 0;
  SdGlyphPool pool;
  auto first = std::make_unique<SdCardFont>(&pool);
  {
    const size_t before = nothrowArrayAllocations;
    ASSERT_TRUE(first->load("fixture"));
    firstLoad = nothrowArrayAllocations - before;
  }
  SdCardFont second(&pool);
  {
    const size_t before = nothrowArrayAllocations;
    ASSERT_TRUE(second.load("fixture"));
    secondLoad = nothrowArrayAllocations - before;
  }
  EXPECT_EQ(firstLoad - 1, secondLoad);

  // The table outlives the font that loaded it while another font still uses it.
  first.reset();
  EXPECT_TRUE(second.getEpdFont()->hasCodepoint(FIRST));
  EXPECT_TRUE(second.getEpdFont()->hasCodepoint(0xFFFD));
  EXPECT_FALSE(second.getEpdFont()->hasCodepoint(FIRST - 1));
}

TEST(SdCardFontTest, ACorruptGlyphEntryDrawsAsMissing) {
  makeFont();
  // Glyph 3 claims more bytes than its pixels need, as a corrupt .cpfont does.
  constexpr size_t GLYPH_OFFSET = 64 + 24;
  EpdGlyph glyph{};
  std::memcpy(&glyph, sdFontTestFile.data() + GLYPH_OFFSET + 3 * sizeof(glyph), sizeof(glyph));
  glyph.dataLength = 49335;
  std::memcpy(sdFontTestFile.data() + GLYPH_OFFSET + 3 * sizeof(glyph), &glyph, sizeof(glyph));
  SdGlyphPool pool;
  SdCardFont font(&pool);
  ASSERT_TRUE(font.load("fixture"));
  ASSERT_EQ(0, font.prewarm(page(FIRST, 5).c_str(), 1, false, false, false));
  for (uint32_t cp = FIRST; cp < FIRST + 5; ++cp) {
    const EpdGlyph* found = font.getEpdFont()->getGlyph(cp);
    ASSERT_NE(nullptr, found);
    if (cp == FIRST + 3) {
      EXPECT_EQ(0, found->dataLength);
      EXPECT_EQ(0, found->width);
    } else {
      EXPECT_EQ(BITMAP_BYTES, found->dataLength);
    }
  }
}

TEST(SdCardFontTest, VariableLengthEntriesSurviveChunkReuseAndIndexDeletion) {
  makeFont();
  for (uint32_t i = 0; i < GLYPHS; ++i) setGlyphBitmapLength(i, (i * 37) % 129);
  SdGlyphPool pool;
  SdCardFont font(&pool);
  ASSERT_TRUE(font.load("fixture"));
  for (uint32_t turn = 0; turn < 150; ++turn) {
    const uint32_t first = FIRST + (turn * 23) % 450;
    const auto text = page(FIRST, 3) + page(first, 40);
    cacheBoundary(pool, font);
    ASSERT_EQ(0, font.prewarm(text.c_str(), 1, false, false, false));
    sdFontTestReads = 0;
    expectPageBitmaps(font, first, 40);
    EXPECT_EQ(0U, sdFontTestReads);
  }
}

TEST(SdCardFontTest, SharedPoolSeparatesStylesAndFallbackUsesReplacementGlyph) {
  makeFont();
  const auto original = sdFontTestFile;
  const size_t section = original.size() - 64;
  sdFontTestFile.assign(96 + section * 2, 0);
  std::copy_n(original.data(), 64, sdFontTestFile.data());
  std::copy_n(original.data() + 32, 32, sdFontTestFile.data() + 64);
  std::copy_n(original.data() + 64, section, sdFontTestFile.data() + 96);
  std::copy_n(original.data() + 64, section, sdFontTestFile.data() + 96 + section);
  sdFontTestFile[12] = 2;
  sdFontTestFile[64] = 1;
  put32(56, 96);
  put32(88, 96 + section);
  for (uint32_t i = 0; i < GLYPHS; ++i) {
    put16(96 + section + 24 + i * sizeof(EpdGlyph) + offsetof(EpdGlyph, advanceX), 33 << 4);
  }
  SdGlyphPool pool;
  SdCardFont font(&pool);
  ASSERT_TRUE(font.load("fixture"));
  pool.beginScope();
  ASSERT_EQ(0, font.prewarm(page(FIRST, 5).c_str(), 2, false, false, false));
  ASSERT_EQ(0, font.prewarm(page(FIRST, 100).c_str(), 1, false, false, false));
  ASSERT_EQ(0, font.prewarm(page(FIRST, 20).c_str(), 2, false, false, false));
  sdFontTestReads = 0;
  EXPECT_EQ(32 << 4, font.getEpdFont(0)->getGlyph(FIRST)->advanceX);
  EXPECT_EQ(33 << 4, font.getEpdFont(1)->getGlyph(FIRST)->advanceX);
  EXPECT_EQ(0U, sdFontTestReads);
  EXPECT_EQ(font.getEpdFont(0)->getGlyph(0xFFFD), font.getEpdFont(0)->getGlyph(0x100));
}

// Under ASan, any access past an arena, index, scratch or fallback block faults.
TEST(SdCardFontTest, RandomGlyphSizesKeepBitmapsIntactThroughChunkReuse) {
  const auto savedLargest = ESP.largestBlock;
  uint32_t random = 0x12345678;
  const auto next = [&]() {
    random ^= random << 13;
    random ^= random >> 17;
    random ^= random << 5;
    return random;
  };
  for (uint32_t layout = 0; layout < 80; ++layout) {
    makeFont();
    for (uint32_t i = 0; i < GLYPHS; ++i) setGlyphBitmapLength(i, next() % 129);
    ESP.largestBlock = 4096 + 1100 + (next() % 1500) * 4;
    SdGlyphPool pool;
    SdCardFont font(&pool);
    ASSERT_TRUE(font.load("fixture"));
    for (uint32_t turn = 0; turn < 100; ++turn) {
      const uint32_t count = 1 + next() % 45;
      const uint32_t first = FIRST + next() % (GLYPHS - count - 1);
      cacheBoundary(pool, font);
      EXPECT_GE(font.prewarm(page(first, count).c_str(), 1, false, false, false), 0);
      expectPageBitmaps(font, first, count);
    }
  }
  ESP.largestBlock = savedLargest;
}

TEST(ReviewRepro, SmallFirstScopesConvergeOnDensePages) {
  makeFont();
  SdGlyphPool pool;
  SdCardFont font(&pool);
  ASSERT_TRUE(font.load("fixture"));
  for (uint32_t count : {5U, 20U, 60U, 60U, 60U}) {
    pool.beginScope();
    font.resetStats();
    ASSERT_EQ(0, font.prewarm(page(FIRST + 200, count).c_str(), 1, false, true, false));
    sdFontTestReads = 0;
    expectPageBitmaps(font, FIRST + 200, count);
    EXPECT_EQ(0U, sdFontTestReads);
    EXPECT_EQ(0U, font.getStats().blockedInsertions);
    pool.endScope();
  }
}

TEST(ReviewRepro, UiStringsReleasePinsAndRepaintsReadNothing) {
  makeFont();
  SdGlyphPool pool;
  SdCardFont font(&pool);
  ASSERT_TRUE(font.load("fixture"));
  for (uint32_t window = 0; window < 4; ++window) {
    const uint32_t first = FIRST + window * 30;
    font.prewarm(page(first, 30).c_str(), 1, false, false);
    for (int repaint = 0; repaint < 2; ++repaint) {
      sdFontTestReads = 0;
      font.resetStats();
      ASSERT_EQ(0, font.prewarm(page(first, 30).c_str(), 1, false, false));
      expectPageBitmaps(font, first, 30);
      EXPECT_EQ(0U, sdFontTestReads);
      EXPECT_EQ(0U, font.getStats().blockedInsertions);
    }
  }
}

TEST(ReviewRepro, MetadataPrewarmMeasuresWithoutBitmapReadsThenUpgradesOnDraw) {
  makeFont();
  SdGlyphPool pool;
  SdCardFont font(&pool);
  ASSERT_TRUE(font.load("fixture"));
  ASSERT_EQ(0, font.prewarm(page(FIRST, 20).c_str(), 1, true, false));
  sdFontTestReads = 0;
  font.resetStats();
  for (uint32_t cp = FIRST; cp < FIRST + 20; ++cp) ASSERT_NE(nullptr, font.getEpdFont()->getGlyph(cp));
  EXPECT_EQ(0U, sdFontTestReads);
  EXPECT_EQ(0U, font.getStats().seekCount);
  EXPECT_EQ(0U, font.getStats().bitmapBytes);
  const auto* before = font.getEpdFont()->getGlyph(FIRST);
  expectPageBitmaps(font, FIRST, 20);
  EXPECT_EQ(20U * BITMAP_BYTES, font.getStats().bitmapBytes);
  EXPECT_EQ(before, font.getEpdFont()->getGlyph(FIRST));
  sdFontTestReads = 0;
  expectPageBitmaps(font, FIRST, 20);
  EXPECT_EQ(0U, sdFontTestReads);
}

TEST(SdGlyphPoolTest, ExactChunkBoundaryGrowsWithoutMovingPinnedEntries) {
  SdGlyphPool pool;
  const auto slot = pool.registerFont();
  EpdGlyph glyph{};
  glyph.dataLength = SdGlyphPool::CHUNK_BYTES - sizeof(SdGlyphPool::Entry);
  auto* first = pool.insert(slot << 2, 1, glyph);
  ASSERT_NE(nullptr, first);
  SdGlyphPool::bitmap(first)[glyph.dataLength - 1] = 123;
  const auto allocations = sdFontPsramAllocations;
  ASSERT_NE(nullptr, pool.insert(slot << 2, 2, glyph));
  EXPECT_EQ(allocations + 1, sdFontPsramAllocations);
  EXPECT_EQ(first, pool.find(slot << 2, 1));
  EXPECT_EQ(123, SdGlyphPool::bitmap(first)[glyph.dataLength - 1]);
}

TEST(SdGlyphPoolTest, AllPinnedChunkCapUsesOverflowAndReusesChunksAfterUnitEnd) {
  SdGlyphPool pool;
  const auto key = pool.registerFont() << 2;
  EpdGlyph glyph{};
  glyph.dataLength = 4000;
  for (uint32_t cp = 0; cp < 9; ++cp) ASSERT_NE(nullptr, pool.insert(key, cp, glyph));
  const auto allocations = sdFontPsramAllocations;
  EXPECT_EQ(nullptr, pool.insert(key, 9, glyph));
  EXPECT_EQ(allocations, sdFontPsramAllocations);
  auto* extra = pool.insertOverflow(key, 9, glyph);
  ASSERT_NE(nullptr, extra);
  EXPECT_TRUE(pool.isOverflow(extra));
  pool.beginPrewarm();
  const auto settled = sdFontPsramAllocations;
  ASSERT_NE(nullptr, pool.insert(key, 10, glyph));
  EXPECT_EQ(nullptr, pool.find(key, 0));
  EXPECT_EQ(settled, sdFontPsramAllocations);
}

TEST(SdGlyphPoolTest, EntryCountCapDoesNotAllocateUnusableChunks) {
  SdGlyphPool pool;
  const auto key = pool.registerFont() << 2;
  EpdGlyph glyph{};
  for (uint32_t cp = 0; cp < 512; ++cp) ASSERT_NE(nullptr, pool.insert(key, cp, glyph));
  const auto allocations = sdFontPsramAllocations;
  EXPECT_EQ(nullptr, pool.insert(key, 512, glyph));
  EXPECT_EQ(allocations, sdFontPsramAllocations);
  pool.beginPrewarm();
  ASSERT_NE(nullptr, pool.insert(key, 512, glyph));
  EXPECT_EQ(allocations, sdFontPsramAllocations);
}

TEST(SdGlyphPoolTest, OversizedOverflowGrowsAndRetainsPixels) {
  SdGlyphPool pool;
  const auto key = pool.registerFont() << 2;
  EpdGlyph glyph{};
  glyph.dataLength = 5000;
  EXPECT_EQ(nullptr, pool.insert(key, 1, glyph));
  auto* first = pool.insertOverflow(key, 1, glyph);
  ASSERT_NE(nullptr, first);
  std::fill_n(SdGlyphPool::bitmap(first), glyph.dataLength, 73);
  EXPECT_EQ(73, SdGlyphPool::bitmap(pool.find(key, 1))[4999]);
  const auto allocations = sdFontPsramAllocations;
  ASSERT_NE(nullptr, pool.insertOverflow(key, 2, glyph));
  EXPECT_EQ(allocations, sdFontPsramAllocations);
  EXPECT_EQ(nullptr, pool.find(key, 1));
  EXPECT_NE(nullptr, pool.find(key, 2));
}

TEST(SdGlyphPoolTest, OverflowRingKeepsEightSmallEntriesAndEvictsFifo) {
  SdGlyphPool pool;
  const auto key = pool.registerFont() << 2;
  EpdGlyph glyph{};
  glyph.dataLength = 128;
  for (uint32_t cp = 0; cp < 9; ++cp) ASSERT_NE(nullptr, pool.insertOverflow(key, cp, glyph));
  EXPECT_EQ(nullptr, pool.find(key, 0));
  for (uint32_t cp = 1; cp < 9; ++cp) EXPECT_NE(nullptr, pool.find(key, cp)) << cp;
  EXPECT_TRUE(pool.isOverflow(pool.find(key, 8)));
}

TEST(SdGlyphPoolTest, FontsShareChunksWithoutEvictingCurrentScopePins) {
  SdGlyphPool pool;
  const auto a = pool.registerFont() << 2;
  const auto b = pool.registerFont() << 2;
  EpdGlyph glyph{};
  glyph.dataLength = 4000;
  pool.beginScope();
  auto* pinned = pool.insert(a, 1, glyph);
  ASSERT_NE(nullptr, pinned);
  for (uint32_t cp = 0; cp < 8; ++cp) ASSERT_NE(nullptr, pool.insert(b, cp, glyph));
  EXPECT_EQ(nullptr, pool.insert(b, 9, glyph));
  EXPECT_EQ(pinned, pool.find(a, 1));
  pool.endScope();
  pool.beginPrewarm();
  ASSERT_NE(nullptr, pool.insert(b, 9, glyph));
  EXPECT_EQ(nullptr, pool.find(a, 1));
  pool.beginPrewarm();
  ASSERT_NE(nullptr, pool.insert(a, 1, glyph));
  EXPECT_EQ(nullptr, pool.find(b, 0));
  pool.unregisterFont(b >> 2);
  EXPECT_NE(nullptr, pool.find(a, 1));
  EXPECT_EQ(nullptr, pool.find(b, 9));
}

TEST(SdGlyphPoolTest, PsramUsesRuntimeThirtyTwoChunkCap) {
  // PSRAM may join the heap after global constructors, so the cap is read at the first chunk.
  SdGlyphPool pool;
  HalMemory::hasPsram = true;
  const auto key = pool.registerFont() << 2;
  EpdGlyph glyph{};
  glyph.dataLength = 4000;
  for (uint32_t cp = 0; cp < 32; ++cp) ASSERT_NE(nullptr, pool.insert(key, cp, glyph));
  HalMemory::hasPsram = false;
  EXPECT_EQ(nullptr, pool.insert(key, 32, glyph));
  for (uint32_t cp = 0; cp < 32; ++cp) EXPECT_NE(nullptr, pool.find(key, cp));
}

TEST(SdGlyphPoolTest, OverflowFailureRecoversAndReleaseFreesAllStorage) {
  const auto outstanding = sdFontPsramAllocations - sdFontPsramFrees;
  {
    SdGlyphPool pool;
    const auto key = pool.registerFont() << 2;
    EpdGlyph glyph{};
    glyph.dataLength = 128;
    sdFontPsramFailCount = 2;  // the 4 KiB block and its exact-size retry both fail
    sdFontPsramFailAfter = 0;
    EXPECT_EQ(nullptr, pool.insertOverflow(key, 1, glyph));
    ASSERT_NE(nullptr, pool.insertOverflow(key, 1, glyph));
    pool.release();
    EXPECT_EQ(outstanding, sdFontPsramAllocations - sdFontPsramFrees);
    ASSERT_NE(nullptr, pool.insert(key, 1, glyph));
    pool.unregisterFont(key >> 2);
    EXPECT_EQ(outstanding, sdFontPsramAllocations - sdFontPsramFrees);
  }
  EXPECT_EQ(outstanding, sdFontPsramAllocations - sdFontPsramFrees);
}

TEST(SdCardFontTest, DenseOverflowPrewarmNeverReusesPendingBitmapStorage) {
  makeFont();
  SdGlyphPool pool;
  SdCardFont font(&pool);
  ASSERT_TRUE(font.load("fixture"));
  for (unsigned turn = 0; turn < 3; ++turn) {
    pool.beginScope();
    ASSERT_GT(font.prewarm(page(FIRST, 400).c_str(), 1, false, false, false), 0);
    expectPageBitmaps(font, FIRST, 400);
    pool.endScope();
  }
}

TEST(SdCardFontTest, MultipleFontsSharePoolAndUnloadingOnePreservesTheOther) {
  makeFont();
  SdGlyphPool pool;
  SdCardFont first(&pool);
  ASSERT_TRUE(first.load("fixture"));
  ASSERT_EQ(0, first.prewarm(page(FIRST, 10).c_str(), 1, false, false));
  const auto* pinned = first.getEpdFont()->getGlyph(FIRST);
  {
    SdCardFont second(&pool);
    ASSERT_TRUE(second.load("fixture"));
    ASSERT_EQ(0, second.prewarm(page(FIRST, 10).c_str(), 1, false, false));
    EXPECT_NE(pinned, second.getEpdFont()->getGlyph(FIRST));
    expectPageBitmaps(second, FIRST, 10);
  }
  sdFontTestReads = 0;
  EXPECT_EQ(pinned, first.getEpdFont()->getGlyph(FIRST));
  expectPageBitmaps(first, FIRST, 10);
  EXPECT_EQ(0U, sdFontTestReads);
}

TEST(SdGlyphPoolTest, GenerationWrapDoesNotReviveAncientPins) {
  SdGlyphPool pool;
  const auto key = pool.registerFont() << 2;
  EpdGlyph glyph{};
  glyph.dataLength = 4000;
  ASSERT_NE(nullptr, pool.insert(key, 1, glyph));
  for (unsigned generation = 0; generation < UINT16_MAX; ++generation) pool.beginPrewarm();
  const auto allocations = sdFontPsramAllocations;
  ASSERT_NE(nullptr, pool.insert(key, 2, glyph));
  EXPECT_EQ(nullptr, pool.find(key, 1));
  EXPECT_EQ(allocations, sdFontPsramAllocations);
}

TEST(SdGlyphPoolTest, PrewarmCompactsScatteredPinsButDrawingNeverMovesThem) {
  SdGlyphPool pool;
  const auto key = pool.registerFont() << 2;
  EpdGlyph glyph{};
  glyph.dataLength = 104;  // 32 entries per chunk.
  for (uint32_t cp = 0; cp < 9 * 32; ++cp) ASSERT_NE(nullptr, pool.insert(key, cp, glyph));
  pool.beginPrewarm();
  for (uint32_t chunk = 0; chunk < 9; ++chunk) ASSERT_NE(nullptr, pool.find(key, chunk * 32));
  // Drawing cannot move survivors, but prewarm can reclaim the 279 unused entries.
  const auto* first = pool.find(key, 0);
  EXPECT_EQ(nullptr, pool.insert(key, 1000, glyph));
  EXPECT_EQ(first, pool.find(key, 0));
  const auto allocations = sdFontPsramAllocations;
  EXPECT_NE(nullptr, pool.insert(key, 1000, glyph, true));
  for (uint32_t chunk = 0; chunk < 9; ++chunk) EXPECT_NE(nullptr, pool.find(key, chunk * 32));
  for (uint32_t turn = 1; turn < 12; ++turn) {
    pool.beginPrewarm();
    for (uint32_t chunk = 0; chunk < 9; ++chunk) ASSERT_NE(nullptr, pool.find(key, chunk * 32));
    ASSERT_NE(nullptr, pool.insert(key, 1000 + turn, glyph, true));
  }
  EXPECT_EQ(allocations, sdFontPsramAllocations);
}

TEST(SdGlyphPoolTest, PrewarmReclaimsSpaceBeforeGrowingBelowTheCap) {
  SdGlyphPool pool;
  const auto key = pool.registerFont() << 2;
  EpdGlyph glyph{};
  glyph.dataLength = 104;
  for (uint32_t cp = 0; cp < 32; ++cp) ASSERT_NE(nullptr, pool.insert(key, cp, glyph));
  pool.beginPrewarm();
  ASSERT_NE(nullptr, pool.find(key, 1));
  const auto allocations = sdFontPsramAllocations;
  ASSERT_NE(nullptr, pool.insert(key, 100, glyph, true));
  EXPECT_EQ(allocations, sdFontPsramAllocations);
  EXPECT_NE(nullptr, pool.find(key, 1));
  EXPECT_EQ(nullptr, pool.find(key, 0));
  // Once the current unit itself fills the chunk, growth is necessary.
  for (uint32_t cp = 101; cp < 131; ++cp) ASSERT_NE(nullptr, pool.insert(key, cp, glyph, true));
  ASSERT_NE(nullptr, pool.insert(key, 131, glyph, true));
  EXPECT_EQ(allocations + 1, sdFontPsramAllocations);
}

TEST(SdGlyphPoolTest, ScratchGrowsByNeedAndPreservesCodepointsOnAllocationFailure) {
  const auto live = sdFontPsramAllocations - sdFontPsramFrees;
  {
    SdGlyphPool pool;
    auto* scratch = pool.scratch(1);
    ASSERT_NE(nullptr, scratch);
    ASSERT_EQ(32U, scratch->capacity);
    for (uint32_t i = 0; i < 32; ++i) scratch->codepoints[i] = i + 100;
    const auto allocations = sdFontPsramAllocations;
    EXPECT_EQ(scratch, pool.scratch(32));
    EXPECT_EQ(allocations, sdFontPsramAllocations);
    sdFontPsramFailAfter = 0;
    EXPECT_EQ(nullptr, pool.scratch(33));
    EXPECT_EQ(32U, pool.scratch()->capacity);
    ASSERT_NE(nullptr, pool.scratch(33));
    EXPECT_EQ(64U, scratch->capacity);
    for (uint32_t i = 0; i < 32; ++i) EXPECT_EQ(i + 100, scratch->codepoints[i]);
    EXPECT_EQ(0U, reinterpret_cast<uintptr_t>(scratch->pending) % alignof(SdGlyphPool::Entry*));
    ASSERT_NE(nullptr, pool.scratch(512));
    EXPECT_EQ(512U, scratch->capacity);
    EXPECT_EQ(nullptr, pool.scratch(513));
  }
  EXPECT_EQ(live, sdFontPsramAllocations - sdFontPsramFrees);
}

TEST(SdCardFontTest, AnotherFontsPrewarmCompactsPinnedGlyphsAndResolvesMovedBitmapTargets) {
  makeFont();
  SdGlyphPool pool;
  SdCardFont a(&pool);
  SdCardFont b(&pool);
  ASSERT_TRUE(a.load("fixture"));
  ASSERT_TRUE(b.load("fixture"));
  ASSERT_EQ(0, a.prewarm(page(FIRST, 230).c_str(), 1, true, false));
  pool.beginScope();
  ASSERT_EQ(0, a.prewarm(page(FIRST + 1, 1).c_str(), 1, true, false));
  const auto* old = a.getEpdFont()->getGlyph(FIRST + 1);
  const auto allocations = sdFontPsramAllocations;
  ASSERT_EQ(0, b.prewarm(page(FIRST, 230).c_str(), 1, false, false));
  EXPECT_EQ(allocations, sdFontPsramAllocations);
  EXPECT_EQ(0U, b.getStats().blockedInsertions);
  EXPECT_NE(old, a.getEpdFont()->getGlyph(FIRST + 1));
  expectPageBitmaps(a, FIRST + 1, 1);
  sdFontTestReads = 0;
  expectPageBitmaps(b, FIRST, 230);
  EXPECT_EQ(0U, sdFontTestReads);
  pool.endScope();
}

TEST(SdGlyphPoolTest, CacheBoundaryReleasesOverflowButRetainsChunks) {
  SdGlyphPool pool;
  const auto key = pool.registerFont() << 2;
  EpdGlyph glyph{};
  glyph.dataLength = 128;
  ASSERT_NE(nullptr, pool.insert(key, 1, glyph));
  const auto live = sdFontPsramAllocations - sdFontPsramFrees;
  ASSERT_NE(nullptr, pool.insertOverflow(key, 2, glyph));
  pool.clearCache();
  EXPECT_EQ(live, sdFontPsramAllocations - sdFontPsramFrees);
  EXPECT_NE(nullptr, pool.find(key, 1));
  EXPECT_EQ(nullptr, pool.find(key, 2));
}

TEST(SdCardFontTest, CorruptRecordsNeverReadPixelsThroughPoolOrMetricsMisses) {
  for (const bool invalidOffset : {false, true}) {
    for (const bool prewarm : {false, true}) {
      makeFont();
      if (invalidOffset) {
        put32(glyphField(3, offsetof(EpdGlyph, dataOffset)), UINT32_MAX);
      } else {
        put16(glyphField(3, offsetof(EpdGlyph, dataLength)), 49335);
      }
      SdGlyphPool pool;
      SdCardFont font(&pool);
      ASSERT_TRUE(font.load("fixture"));
      SdCardFont::takeCorruptGlyphNotice();
      if (prewarm) ASSERT_EQ(0, font.prewarm(page(FIRST + 3, 1).c_str(), 1, true, false));
      const auto* glyph = font.getEpdFont()->getGlyph(FIRST + 3);
      ASSERT_NE(nullptr, glyph);
      EXPECT_EQ(0, glyph->width);
      EXPECT_EQ(0, glyph->dataLength);
      EXPECT_EQ(0, glyph->advanceX);
      EXPECT_TRUE(SdCardFont::takeCorruptGlyphNotice());
      EXPECT_FALSE(SdCardFont::takeCorruptGlyphNotice());
      const auto* data = font.getEpdFont()->data;
      const auto reads = sdFontTestReads;
      EXPECT_EQ(nullptr, data->vectorBitmapHandler(data->glyphMissCtx, glyph));
      EXPECT_EQ(reads, sdFontTestReads);
      EXPECT_EQ(0U, font.getStats().bitmapBytes);
      ASSERT_EQ(0, font.buildAdvanceTable(page(FIRST + 3, 1).c_str(), 1));
      EXPECT_EQ(0, font.getAdvance(FIRST + 3, 0));
      EXPECT_FALSE(SdCardFont::takeCorruptGlyphNotice());
      ASSERT_EQ(0, font.prewarm(page(FIRST, 5).c_str(), 1, false, false));
      expectPageBitmaps(font, FIRST, 3);
      expectPageBitmaps(font, FIRST + 4, 1);
    }
  }
}

TEST(SdGlyphPoolTest, CompactionThatFreesOnlyDroppedBytesCannotExceedTheEntryCap) {
  SdGlyphPool pool;
  const auto a = pool.registerFont() << 2;
  const auto b = pool.registerFont() << 2;
  EpdGlyph glyph{};  // 24-byte entries: the entry cap (512) binds before the chunk cap
  pool.beginScope();
  for (uint32_t cp = 0; cp < 200; ++cp) ASSERT_NE(nullptr, pool.insert(a, cp, glyph, true));
  for (uint32_t cp = 0; cp < 312; ++cp) ASSERT_NE(nullptr, pool.insert(b, cp, glyph, true));
  EXPECT_EQ(nullptr, pool.insert(b, 312, glyph, true));
  // Unloading font b leaves dropped bytes beside a's pinned entries; compaction may reclaim
  // those bytes, but the live count stays at the cap, so the next insert must still refuse.
  pool.dropFont(b >> 2);
  for (uint32_t cp = 200; cp < 512; ++cp) ASSERT_NE(nullptr, pool.insert(a, cp, glyph, true)) << cp;
  EXPECT_EQ(nullptr, pool.insert(a, 512, glyph, true));
  EXPECT_EQ(nullptr, pool.insert(a, 513, glyph, true));
  for (uint32_t cp = 0; cp < 512; ++cp) EXPECT_NE(nullptr, pool.find(a, cp)) << cp;
}

TEST(SdGlyphPoolTest, TrimUnpinnedFreesIdleChunksButKeepsTheCurrentUnit) {
  SdGlyphPool pool;
  const auto key = pool.registerFont() << 2;
  EpdGlyph glyph{};
  glyph.dataLength = 4000;
  for (uint32_t cp = 0; cp < 8; ++cp) ASSERT_NE(nullptr, pool.insert(key, cp, glyph));
  pool.beginScope();
  ASSERT_NE(nullptr, pool.insert(key, 8, glyph));  // reuses the oldest unpinned chunk
  const auto live = sdFontPsramAllocations - sdFontPsramFrees;
  pool.trimUnpinned();
  EXPECT_EQ(live - 7, sdFontPsramAllocations - sdFontPsramFrees);  // only the pinned chunk and the index stay
  EXPECT_NE(nullptr, pool.find(key, 8));
  for (uint32_t cp = 0; cp < 8; ++cp) EXPECT_EQ(nullptr, pool.find(key, cp)) << cp;
  pool.endScope();
  pool.beginPrewarm();
  pool.trimUnpinned();
  EXPECT_EQ(nullptr, pool.find(key, 8));
  ASSERT_NE(nullptr, pool.insert(key, 9, glyph));
}

TEST(SdGlyphPoolTest, OverflowFallsBackToTheExactGlyphSizeWhenTheBlockDoesNotFit) {
  SdGlyphPool pool;
  const auto key = pool.registerFont() << 2;
  EpdGlyph glyph{};
  glyph.dataLength = 100;
  sdFontPsramFailAfter = 0;  // the 4 KiB block fails, the exact-size retry succeeds
  auto* entry = pool.insertOverflow(key, 1, glyph);
  ASSERT_NE(nullptr, entry);
  EXPECT_TRUE(pool.isOverflow(entry));
  std::fill_n(SdGlyphPool::bitmap(entry), glyph.dataLength, 9);
  ASSERT_NE(nullptr, pool.insertOverflow(key, 2, glyph));  // replaces 1 in the small block
  EXPECT_EQ(nullptr, pool.find(key, 1));
  EXPECT_NE(nullptr, pool.find(key, 2));
  glyph.dataLength = 200;
  ASSERT_NE(nullptr, pool.insertOverflow(key, 3, glyph));  // regrows for a larger glyph
  EXPECT_NE(nullptr, pool.find(key, 3));
}

TEST(SdCardFontTest, DrawTimeMissReadsTheRecordOnceForMeasureAndDraw) {
  makeFont();
  SdGlyphPool pool;
  SdCardFont font(&pool);
  ASSERT_TRUE(font.load("fixture"));
  sdFontTestReads = 0;
  const auto* measured = font.getEpdFont()->getGlyph(FIRST + 7);
  ASSERT_NE(nullptr, measured);
  const auto recordReads = sdFontTestReads;
  EXPECT_GT(recordReads, 0U);
  // drawText measures, then renderCharImpl looks the glyph up again before drawing it.
  const auto* drawn = font.getEpdFont()->getGlyph(FIRST + 7);
  EXPECT_EQ(measured, drawn);
  EXPECT_EQ(recordReads, sdFontTestReads);
  const auto* data = font.getEpdFont()->data;
  ASSERT_NE(nullptr, data->vectorBitmapHandler(data->glyphMissCtx, drawn));
  EXPECT_GT(sdFontTestReads, recordReads);
  // A later miss replaces the slot; the drawn glyph is then served from the pool.
  font.getEpdFont()->getGlyph(FIRST + 8);
  const auto reads = sdFontTestReads;
  EXPECT_NE(nullptr, font.getEpdFont()->getGlyph(FIRST + 7));  // now a pool hit, no read
  EXPECT_EQ(reads, sdFontTestReads);
}
