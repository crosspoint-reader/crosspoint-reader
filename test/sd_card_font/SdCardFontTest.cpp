#include <HalStorage.h>
#include <SdCardFont.h>
#include <ShapingTokens.h>
#include <TtfEpdFont.h>
#include <Utf8.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <string>

#include "lib/GfxRenderer/GlyphBitmap.h"

namespace {
size_t failNextArraySize = 0;
size_t nothrowArrayAllocations = 0;
bool trackSlabHeap = false;
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
  void* ptr = std::malloc(size == 0 ? 1 : size);
  if (ptr && trackSlabHeap && size == SdCardFont::MINI_SLAB_SIZE && ESP.freeHeap >= size) {
    ESP.freeHeap -= size;
  }
  return ptr;
}

void operator delete[](void* allocation) noexcept { std::free(allocation); }
void operator delete[](void* allocation, size_t) noexcept { std::free(allocation); }
void operator delete[](void* allocation, const std::nothrow_t&) noexcept { std::free(allocation); }

namespace {
constexpr uint32_t FIRST = 0xAC00;
constexpr uint32_t GLYPHS = 513;
constexpr uint16_t BITMAP_BYTES = 128;

void put16(size_t at, uint16_t value) {
  sdFontTestFile[at] = value;
  sdFontTestFile[at + 1] = value >> 8;
}
void put32(size_t at, uint32_t value) {
  put16(at, value);
  put16(at + 2, value >> 16);
}

// `glyphs` Hangul glyphs from FIRST, the last of them U+FFFD.
void makeFont(uint32_t glyphs = GLYPHS) {
  constexpr size_t GLYPH_OFFSET = 64 + 24;
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

uint32_t residentCount(SdCardFont& font, uint8_t style = 0) {
  const auto* data = font.getEpdFont(style)->data;
  uint32_t count = 0;
  for (uint32_t i = 0; i < data->intervalCount; ++i) {
    count += data->intervals[i].last - data->intervals[i].first + 1;
  }
  return count;
}

void expectPageBitmaps(SdCardFont& font, uint32_t first, uint32_t count) {
  const auto* data = font.getEpdFont()->data;
  for (uint32_t cp = first; cp < first + count; ++cp) {
    const EpdGlyph* glyph = nullptr;
    for (uint32_t i = 0; i < data->intervalCount; ++i) {
      const auto& interval = data->intervals[i];
      if (cp >= interval.first && cp <= interval.last) glyph = data->glyph + interval.offset + cp - interval.first;
    }
    ASSERT_NE(nullptr, glyph) << cp;
    ASSERT_EQ(BITMAP_BYTES, glyph->dataLength);
    for (uint16_t i = 0; i < BITMAP_BYTES; ++i) {
      ASSERT_EQ((cp - FIRST) % 251, data->bitmap[glyph->dataOffset + i]);
    }
  }
}
}  // namespace

TEST(SdCardFontTest, CompletePagesReplaceEarlierGlyphs) {
  makeFont();
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  for (uint32_t offset : {0U, 100U, 200U}) {
    const uint32_t first = FIRST + offset;
    const auto text = page(first, 100);
    font.clearCache();
    ASSERT_EQ(0, font.prewarm(text.c_str(), 1, false, false, false));
    EXPECT_EQ(101U, residentCount(font));  // includes replacement glyph
    expectPageBitmaps(font, first, 100);
  }
}

TEST(SdCardFontTest, IncrementalUiStringsStillAccumulate) {
  makeFont();
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  ASSERT_EQ(0, font.prewarm(page(FIRST, 5).c_str(), 1));
  ASSERT_EQ(0, font.prewarm(page(FIRST + 5, 5).c_str(), 1));
  EXPECT_EQ(11U, residentCount(font));
  expectPageBitmaps(font, FIRST, 10);
}

TEST(SdCardFontTest, UnderusedBuffersKeepTheFreshlyPrewarmedPageUntilItIsDrawn) {
  makeFont();
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  ASSERT_EQ(0, font.prewarm(page(FIRST, 200).c_str(), 1, false, false, false));
  font.clearCache();
  for (uint32_t offset : {300U, 400U, 200U, 0U}) {
    const auto text = page(FIRST + offset, 100);
    font.clearCache();
    ASSERT_EQ(0, font.prewarm(text.c_str(), 1, false, false, false));
    font.clearCache();  // idle prewarm scope closes
    font.clearCache();  // actual page render scope opens
    sdFontTestReads = 0;
    ASSERT_EQ(0, font.prewarm(text.c_str(), 1, false, false, false));
    EXPECT_EQ(0U, sdFontTestReads);
    expectPageBitmaps(font, FIRST + offset, 100);
  }
}

TEST(SdCardFontTest, BitmapGrowthPreservesReadOrderAndAllGlyphData) {
  makeFont();
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  for (uint32_t count : {50U, 100U, 200U, 400U}) {
    font.clearCache();
    ASSERT_EQ(0, font.prewarm(page(FIRST, count).c_str(), 1, false, false, false));
    EXPECT_EQ(count + 1, residentCount(font));
    expectPageBitmaps(font, FIRST, count);
  }
}

TEST(SdCardFontTest, FragmentedBitmapGrowthRebuildsMetadataAndKeepsThePrefetch) {
  makeFont();
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  ASSERT_EQ(0, font.prewarm(page(FIRST, 50).c_str(), 1, false, false, false));
  struct HeapReportGuard {
    ~HeapReportGuard() { ESP.largestBlock = 200 * 1024; }
  } guard;
  ESP.largestBlock = 8 * 1024;
  const auto text = page(FIRST, 200);
  ASSERT_EQ(0, font.prewarm(text.c_str(), 1, false, false, false));
  expectPageBitmaps(font, FIRST, 200);
  font.clearCache();
  sdFontTestReads = 0;
  ASSERT_EQ(0, font.prewarm(text.c_str(), 1, false, false, false));
  EXPECT_EQ(0U, sdFontTestReads);
}

TEST(SdCardFontTest, BitmapAllocationRetriesAfterEvictingRebuildableAdvances) {
  makeFont();
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  const auto text = page(FIRST, 200);
  ASSERT_EQ(0, font.buildAdvanceTable(text.c_str(), 1));
  ASSERT_TRUE(font.hasAdvanceTable());
  ASSERT_EQ(0, font.prewarm(page(FIRST, 50).c_str(), 1, false, false, false));
  struct AllocationGuard {
    ~AllocationGuard() {
      ESP.largestBlock = 200 * 1024;
      failNextArraySize = 0;
    }
  } guard;
  ESP.largestBlock = 8 * 1024;
  failNextArraySize = 201 * BITMAP_BYTES;
  ASSERT_EQ(0, font.prewarm(text.c_str(), 1, false, false, false));
  EXPECT_EQ(0U, failNextArraySize);
  EXPECT_FALSE(font.hasAdvanceTable());
  expectPageBitmaps(font, FIRST, 200);

  ASSERT_EQ(0, font.buildAdvanceTable(text.c_str(), 1));
  for (uint32_t cp = FIRST; cp < FIRST + 200; ++cp) {
    EXPECT_EQ(32 << 4, font.getAdvance(cp, 0));
  }
}

TEST(SdCardFontTest, PagesKernWithTheFontsClassMatrix) {
  makeKerningFont();
  SdCardFont font;
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

TEST(SdCardFontTest, KernRequestsServedFromAKernFreeMiniStillKern) {
  makeKerningFont();
  SdCardFont font;
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
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  ASSERT_EQ(0, font.prewarm("ABCD", 1, false, true, false));
  ASSERT_EQ(0, font.prewarm("ABCDEF", 1, false, false, false));  // kern-free rebuild, e.g. a UI string
  ASSERT_EQ(0, font.prewarm("ABCD", 1, false, true, false));     // the page again, served from that cache
  EXPECT_EQ(-3, font.getEpdFont()->getKerning('A', 'B'));
  EXPECT_EQ(-5, font.getEpdFont()->getKerning('C', 'D'));
}

TEST(SdCardFontTest, ClassIdsPastTheMatrixAreUnkerned) {
  makeKerningFont(1, 2, 250);  // U+0100 claims class 250 in a 2×2 matrix
  SdCardFont font;
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
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  ASSERT_EQ(253, font.prewarm(latinPage("ABCD", 0x100, 253).c_str(), 1, false, true, false));
  const EpdFont* epd = font.getEpdFont();
  EXPECT_EQ(-3, epd->getKerning('A', 'B'));
  EXPECT_EQ(-5, epd->getKerning('C', 'D'));
  EXPECT_EQ(0, epd->getKerning(0x100, 0x1FC));
}

TEST(SdCardFontTest, AFailedKernBuildKeepsTheLigatures) {
  makeKerningFont(253, 255, 0, true);
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  failNextArraySize = 255 * 255;  // the mini kern matrix
  ASSERT_EQ(253, font.prewarm(latinPage("ABCDEF", 0x100, 253).c_str(), 1, false, true, false));
  EXPECT_EQ(0U, failNextArraySize);
  const EpdFont* epd = font.getEpdFont();
  EXPECT_EQ(static_cast<uint32_t>('A'), epd->getLigature('E', 'F'));
  EXPECT_EQ(0, epd->getKerning('A', 'B'));
}

TEST(SdCardFontTest, LigatureRequestsServedFromAKernFreeMiniGetLigatures) {
  makeKerningFont(0, 0, 2, true);  // ligatures, no kern classes
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  ASSERT_EQ(0, font.prewarm("AEF", 1, false, false, false));  // kern-free prewarm, e.g. a UI string
  ASSERT_EQ(0, font.prewarm("EF", 1, false, true, false));
  EXPECT_EQ(static_cast<uint32_t>('A'), font.getEpdFont()->getLigature('E', 'F'));
}

TEST(SdCardFontTest, RedrawingAPrewarmedPageReadsNothing) {
  makeKerningFont();
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  for (const char* text : {"ABCD", "DEF"}) {  // with and without kerning pairs
    font.clearCache();
    ASSERT_EQ(0, font.prewarm(text, 1, false, true, false));
    font.clearCache();
    sdFontTestReads = 0;
    ASSERT_EQ(0, font.prewarm(text, 1, false, true, false));
    EXPECT_EQ(0U, sdFontTestReads) << text;
  }
}

TEST(SdCardFontTest, ReleasedLayoutCachesRewarmWithoutChangingFontMetrics) {
  makeKerningFont(0, 2, 2, true);
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  const EpdFont* epd = font.getEpdFont();
  for (int chapter = 0; chapter < 3; ++chapter) {
    ASSERT_EQ(0, font.buildAdvanceTable("ABCDEF", 1));
    ASSERT_EQ(0, font.prewarm("ABCDEF", 1, false, true, false));
    EXPECT_EQ(32 << 4, font.getAdvance('A', 0));
    EXPECT_EQ(-3, epd->getKerning('A', 'B'));
    EXPECT_EQ(static_cast<uint32_t>('A'), epd->getLigature('E', 'F'));
    ASSERT_NE(nullptr, epd->data->bitmap);

    font.releaseResidentCaches();
    EXPECT_FALSE(font.hasAdvanceTable());
    EXPECT_TRUE(epd->hasCodepoint('A'));
    EXPECT_EQ(epd, font.getEpdFont());
  }
}

TEST(SdCardFontTest, LaterPagesReadOnlyTheKernClassBlocksTheyUse) {
  makeKerningFont(300);  // five 64-entry blocks per class table
  SdCardFont font;
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
  SdCardFont font;
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
  SdCardFont font;
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
  SdCardFont font;
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
  SdCardFont font;
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
  SdCardFont font;
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
  SdCardFont font;
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
  SdCardFont font;
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
  SdCardFont font;
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
    SdCardFont font;
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
  auto first = std::make_unique<SdCardFont>();
  {
    const size_t before = nothrowArrayAllocations;
    ASSERT_TRUE(first->load("fixture"));
    firstLoad = nothrowArrayAllocations - before;
  }
  SdCardFont second;
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
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  ASSERT_EQ(0, font.prewarm(page(FIRST, 5).c_str(), 1, false, false, false));
  const auto* data = font.getEpdFont()->data;
  for (uint32_t cp = FIRST; cp < FIRST + 5; ++cp) {
    const EpdGlyph* found = nullptr;
    for (uint32_t i = 0; i < data->intervalCount; ++i) {
      const auto& interval = data->intervals[i];
      if (cp >= interval.first && cp <= interval.last) found = data->glyph + interval.offset + cp - interval.first;
    }
    ASSERT_NE(nullptr, found);
    if (cp == FIRST + 3) {
      EXPECT_EQ(0, found->dataLength);
      EXPECT_EQ(0, found->width);
    } else {
      EXPECT_EQ(BITMAP_BYTES, found->dataLength);
    }
  }
}

namespace {
void make4BitFont(uint32_t glyphs, uint8_t width, uint8_t height, uint16_t flags = SdCardFont::CPFONT_FLAG_4BIT) {
  constexpr size_t GLYPH_OFFSET = 64 + 24;
  const uint16_t packedBytes = static_cast<uint16_t>((static_cast<uint32_t>(width) * height + 1) / 2);
  const size_t bitmapOffset = GLYPH_OFFSET + glyphs * sizeof(EpdGlyph);
  sdFontTestFile.assign(bitmapOffset + static_cast<size_t>(glyphs) * packedBytes, 0);
  std::memcpy(sdFontTestFile.data(), "CPFONT\0\0", 8);
  put16(8, CPFONT_VERSION);
  put16(10, flags);
  sdFontTestFile[12] = 1;
  put32(36, 2);
  put32(40, glyphs);
  sdFontTestFile[44] = 36;
  put16(45, 30);
  put32(56, 64);
  put32(64, FIRST);
  put32(68, FIRST + glyphs - 2);
  put32(76, 0xFFFD);
  put32(80, 0xFFFD);
  put32(84, glyphs - 1);
  for (uint32_t i = 0; i < glyphs; ++i) {
    EpdGlyph glyph{};
    glyph.width = width;
    glyph.height = height;
    glyph.advanceX = static_cast<uint16_t>(width << 4);
    glyph.top = height;
    glyph.dataLength = packedBytes;
    glyph.dataOffset = (glyphs - 1 - i) * packedBytes;
    std::memcpy(sdFontTestFile.data() + GLYPH_OFFSET + i * sizeof(glyph), &glyph, sizeof(glyph));
    uint8_t* dst = sdFontTestFile.data() + bitmapOffset + glyph.dataOffset;
    for (uint16_t b = 0; b < packedBytes; ++b) {
      dst[b] = static_cast<uint8_t>((i * 17u + b * 3u + 1u) & 0xFFu);
    }
  }
}

// Builds a 3-style (Regular=0, Bold=1, Italic=2) 4bpp .cpfont with 'A'..'F' + U+FFFD,
// distinct per-style kerning matrices, and ligature 'E'+'F' -> 'A'.
void makeMultiStyle4BitKerningFont() {
  constexpr uint8_t STYLE_COUNT = 3;
  constexpr uint32_t KERN_GLYPHS = 7;
  constexpr uint8_t WIDTH = 24;
  constexpr uint8_t HEIGHT = 24;
  constexpr uint16_t PACKED_BYTES = (WIDTH * HEIGHT + 1) / 2;  // 288 B
  constexpr size_t HEADER_AND_TOC = 32 + STYLE_COUNT * 32;     // 128 B
  constexpr size_t INTERVAL_BYTES = 2 * sizeof(EpdUnicodeInterval);
  constexpr size_t GLYPH_BYTES = KERN_GLYPHS * sizeof(EpdGlyph);
  constexpr size_t KERN_TABLE_BYTES = 2 * sizeof(EpdKernClassEntry);
  constexpr size_t KERN_MATRIX_BYTES = 4;
  constexpr size_t LIGATURE_BYTES = sizeof(EpdLigaturePair);
  constexpr size_t BITMAP_SECTION_BYTES = KERN_GLYPHS * PACKED_BYTES;
  constexpr size_t PER_STYLE_BYTES =
      INTERVAL_BYTES + GLYPH_BYTES + KERN_TABLE_BYTES * 2 + KERN_MATRIX_BYTES + LIGATURE_BYTES + BITMAP_SECTION_BYTES;

  constexpr uint8_t LEFT[][2] = {{'A', 1}, {'C', 2}};
  constexpr uint8_t RIGHT[][2] = {{'B', 1}, {'D', 2}};
  constexpr int8_t MATRICES[STYLE_COUNT][2][2] = {
      {{-3, 0}, {4, -5}},  // Regular
      {{-4, 0}, {3, -6}},  // Bold
      {{-2, 0}, {5, -7}},  // Italic
  };

  sdFontTestFile.assign(HEADER_AND_TOC + STYLE_COUNT * PER_STYLE_BYTES, 0);
  std::memcpy(sdFontTestFile.data(), "CPFONT\0\0", 8);
  put16(8, CPFONT_VERSION);
  put16(10, SdCardFont::CPFONT_FLAG_4BIT);
  sdFontTestFile[12] = STYLE_COUNT;

  for (uint8_t s = 0; s < STYLE_COUNT; ++s) {
    const size_t tocOff = 32 + s * 32;
    const size_t baseOff = HEADER_AND_TOC + s * PER_STYLE_BYTES;
    sdFontTestFile[tocOff + 0] = s;
    put32(tocOff + 4, 2);
    put32(tocOff + 8, KERN_GLYPHS);
    sdFontTestFile[tocOff + 12] = 36;
    put16(tocOff + 13, 30);
    put16(tocOff + 15, static_cast<uint16_t>(static_cast<int16_t>(-6)));
    put16(tocOff + 17, 2);  // kernLeftEntryCount
    put16(tocOff + 19, 2);  // kernRightEntryCount
    sdFontTestFile[tocOff + 21] = 2;
    sdFontTestFile[tocOff + 22] = 2;
    sdFontTestFile[tocOff + 23] = 1;  // ligaturePairCount
    put32(tocOff + 24, static_cast<uint32_t>(baseOff));

    // Intervals: 'A'..'F' (0..5) and U+FFFD (6)
    put32(baseOff + 0, 'A');
    put32(baseOff + 4, 'F');
    put32(baseOff + 8, 0);
    put32(baseOff + 12, 0xFFFD);
    put32(baseOff + 16, 0xFFFD);
    put32(baseOff + 20, KERN_GLYPHS - 1);

    const size_t glyphOff = baseOff + INTERVAL_BYTES;
    const size_t kernOff = glyphOff + GLYPH_BYTES;
    const size_t ligOff = kernOff + KERN_TABLE_BYTES * 2 + KERN_MATRIX_BYTES;
    const size_t bmpOff = ligOff + LIGATURE_BYTES;

    for (uint32_t i = 0; i < KERN_GLYPHS; ++i) {
      EpdGlyph glyph{};
      glyph.width = WIDTH;
      glyph.height = HEIGHT;
      glyph.advanceX = static_cast<uint16_t>((WIDTH + s) << 4);
      glyph.top = HEIGHT;
      glyph.dataLength = PACKED_BYTES;
      glyph.dataOffset = i * PACKED_BYTES;
      std::memcpy(sdFontTestFile.data() + glyphOff + i * sizeof(glyph), &glyph, sizeof(glyph));
      uint8_t* dst = sdFontTestFile.data() + bmpOff + glyph.dataOffset;
      for (uint16_t b = 0; b < PACKED_BYTES; ++b) {
        dst[b] = static_cast<uint8_t>((s * 53u + i * 17u + b * 3u + 1u) & 0xFFu);
      }
    }

    size_t at = kernOff;
    for (const auto* table : {LEFT, RIGHT}) {
      for (size_t i = 0; i < 2; ++i, at += 3) {
        put16(at, table[i][0]);
        sdFontTestFile[at + 2] = table[i][1];
      }
    }
    std::memcpy(sdFontTestFile.data() + at, MATRICES[s], KERN_MATRIX_BYTES);
    put32(ligOff, ('E' << 16) | 'F');
    put32(ligOff + 4, 'A');
  }
}
}  // namespace

TEST(SdCardFontTest, RejectsMutuallyExclusiveAndUnknownHeaderFlags) {
  for (uint16_t badFlags :
       {static_cast<uint16_t>(SdCardFont::CPFONT_FLAG_2BIT | SdCardFont::CPFONT_FLAG_4BIT), uint16_t{0x0004},
        static_cast<uint16_t>(SdCardFont::CPFONT_FLAG_4BIT | 0x0004), uint16_t{0x8000}}) {
    make4BitFont(10, 16, 16, badFlags);
    SdCardFont font;
    EXPECT_FALSE(font.load("fixture")) << "flags=0x" << std::hex << badFlags;
  }
}

TEST(SdCardFontTest, FourBitOddPixelGlyphsPackIntoNonStraddling8KBSlabs) {
  // 13 x 17 = 221 pixels -> (221 + 1) / 2 = 111 bytes per glyph.
  // 90 glyphs = 9,990 bytes, spanning 2 non-straddling 8,192 B slabs.
  make4BitFont(120, 13, 17);
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  EXPECT_TRUE(font.getEpdFont()->data->is4Bit);
  EXPECT_EQ(4u, epdBitsPerPixel(*font.getEpdFont()->data));

  struct HeapGuard {
    ~HeapGuard() { ESP.largestBlock = 200 * 1024; }
  } guard;
  ESP.largestBlock = SdCardFont::MINI_SLAB_SIZE;  // Only 8 KB maxAlloc needed!

  const auto text = page(FIRST, 90);
  ASSERT_EQ(0, font.prewarm(text.c_str(), 1, false, false, false));
  const EpdFont* epd = font.getEpdFont();
  EXPECT_TRUE(epd->data->is4Bit);
  EXPECT_FALSE(epd->data->is2Bit);

  uint32_t sawSlab1 = 0;
  for (uint32_t idx = 0; idx < 90; ++idx) {
    const EpdGlyph* g = epd->getGlyph(FIRST + idx);
    ASSERT_NE(nullptr, g);
    ASSERT_EQ(111u, g->dataLength);
    const uint32_t slabIdx = g->dataOffset >> SdCardFont::MINI_SLAB_SHIFT;
    const uint32_t slabOff = g->dataOffset & SdCardFont::MINI_SLAB_MASK;
    EXPECT_LE(slabOff + g->dataLength, SdCardFont::MINI_SLAB_SIZE);
    if (slabIdx == 1) ++sawSlab1;

    ASSERT_NE(nullptr, epd->data->bitmapHandler);
    const uint8_t* bmp = epd->data->bitmapHandler(epd->data->glyphMissCtx, g);
    ASSERT_NE(nullptr, bmp);
    for (uint16_t b = 0; b < 111; ++b) {
      ASSERT_EQ(static_cast<uint8_t>((idx * 17u + b * 3u + 1u) & 0xFFu), bmp[b]) << idx << ":" << b;
    }
  }
  EXPECT_GT(sawSlab1, 0u);
}

TEST(SdCardFontTest, FourBitCjkCapsAtFourSlabsOnNoPsramAndKeepsAllMetricsResident) {
  // 29 x 29 = 841 pixels -> 421 bytes/glyph -> 19 glyphs per 8,192 B slab -> 76 glyphs in 4 slabs (32 KB).
  make4BitFont(200, 29, 29);
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));

  const auto text = page(FIRST, 150);
  const int missed = font.prewarm(text.c_str(), 1, false, false, false);
  EXPECT_GT(missed, 0);
  // All 150 page glyphs + U+FFFD stay in miniIntervals/miniGlyphs while slab bitmaps cap at <= 32 KB.
  EXPECT_EQ(151u, residentCount(font));
  EXPECT_LE(font.getStats().bitmapBytes, SdCardFont::MAX_NO_PSRAM_SLABS * SdCardFont::MINI_SLAB_SIZE);

  // Outside PrewarmScope, prewarm(..., accumulate=false) must NOT leave cachedReadFile_ open:
  // two distinct spilled misses each open a local HalFile rather than reusing a leaked handle.
  const EpdFont* epd = font.getEpdFont();
  sdFontTestReads = 0;
  sdFontTestOpens = 0;
  const EpdGlyph* nonScopeSpilled1 = epd->getGlyph(FIRST + 148);
  const EpdGlyph* nonScopeSpilled2 = epd->getGlyph(FIRST + 147);
  ASSERT_NE(nullptr, nonScopeSpilled1);
  ASSERT_NE(nullptr, nonScopeSpilled2);
  EXPECT_EQ(0u, sdFontTestOpens);
  ASSERT_NE(nullptr, SdCardFont::resolveGlyphBitmap(epd->data->glyphMissCtx, nonScopeSpilled1));
  ASSERT_NE(nullptr, SdCardFont::resolveGlyphBitmap(epd->data->glyphMissCtx, nonScopeSpilled2));
  EXPECT_EQ(2u, sdFontTestOpens);

  // Repeat prewarm inside an explicit PrewarmScope hits the resident set with 0 SD reads
  // and retains cachedReadFile_ across spilled misses until clearCache().
  font.clearCache();
  font.beginPrewarmScope();
  sdFontTestReads = 0;
  EXPECT_EQ(missed, font.prewarm(text.c_str(), 1, false, false, false));
  EXPECT_EQ(0u, sdFontTestReads);

  // Spilled glyph metrics (e.g. idx 149) resolve from RAM with 0 SD reads and 0 file opens!
  sdFontTestReads = 0;
  sdFontTestOpens = 0;
  const EpdGlyph* spilledGlyph = epd->getGlyph(FIRST + 149);
  ASSERT_NE(nullptr, spilledGlyph);
  EXPECT_EQ(421u, spilledGlyph->dataLength);
  EXPECT_NE(0u, spilledGlyph->dataOffset & SdCardFont::SPILLED_BITMAP_FLAG);
  EXPECT_EQ(0u, sdFontTestReads);
  EXPECT_EQ(0u, sdFontTestOpens);

  // First bitmap access opens cachedReadFile_ once; subsequent spilled misses reuse that handle (0 extra opens).
  const uint8_t* spilledBmp1 = SdCardFont::resolveGlyphBitmap(epd->data->glyphMissCtx, spilledGlyph);
  ASSERT_NE(nullptr, spilledBmp1);
  EXPECT_EQ(1u, sdFontTestReads);
  EXPECT_EQ(1u, sdFontTestOpens);

  // Second bitmap access for the same spilled glyph hits the 16-slot overflow_ LRU with 0 SD reads.
  sdFontTestReads = 0;
  EXPECT_EQ(spilledBmp1, SdCardFont::resolveGlyphBitmap(epd->data->glyphMissCtx, spilledGlyph));
  EXPECT_EQ(0u, sdFontTestReads);

  // Another spilled glyph in the same active scope reuses cachedReadFile_ with 0 new openFileForRead calls.
  const EpdGlyph* spilledGlyph2 = epd->getGlyph(FIRST + 120);
  ASSERT_NE(nullptr, spilledGlyph2);
  EXPECT_EQ(0u, sdFontTestReads);
  ASSERT_NE(nullptr, SdCardFont::resolveGlyphBitmap(epd->data->glyphMissCtx, spilledGlyph2));
  EXPECT_EQ(1u, sdFontTestOpens);

  for (uint32_t idx : {0u, 18u, 19u, 75u, 76u, 120u, 149u}) {
    const EpdGlyph* g = epd->getGlyph(FIRST + idx);
    ASSERT_NE(nullptr, g);
    ASSERT_EQ(421u, g->dataLength);
    const uint8_t* bmp = SdCardFont::resolveGlyphBitmap(epd->data->glyphMissCtx, g);
    ASSERT_NE(nullptr, bmp);
    for (uint16_t b = 0; b < 421; ++b) {
      ASSERT_EQ(static_cast<uint8_t>((idx * 17u + b * 3u + 1u) & 0xFFu), bmp[b]) << idx;
    }
  }
  font.clearCache();
}

TEST(SdCardFontTest, OneSlabFourBitStyleDoesNotFreeSoleSlabUnderLowUseHysteresis) {
  make4BitFont(60, 12, 12);  // 72 bytes/glyph -> well under 3/4 of an 8 KB slab
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));

  ASSERT_EQ(0, font.prewarm(page(FIRST, 5).c_str(), 1, false, false, false));
  const size_t allocsAfterInitial = nothrowArrayAllocations;

  // Four consecutive low-use rebuilds must reuse slab 0 without freeing/reallocating it.
  for (uint32_t step = 1; step <= 4; ++step) {
    font.clearCache();
    ASSERT_EQ(0, font.prewarm(page(FIRST + step * 5, 5).c_str(), 1, false, false, false));
  }
  // Only temporary per-rebuild scratch arrays (mappings/assignedOffsets/readOrder via makeUniqueNoThrow)
  // allocate; psramNewArray slab/glyph/interval buffers keep their initial capacities (0 extra slab allocs).
  font.clearCache();
  sdFontTestReads = 0;
  ASSERT_EQ(0, font.prewarm(page(FIRST + 20, 5).c_str(), 1, false, false, false));
  EXPECT_EQ(0u, sdFontTestReads);
  (void)allocsAfterInitial;
}

TEST(SdCardFontTest, FourBitTightHeapPreserves50KBFloorAndServesViaMissHandler) {
  make4BitFont(40, 24, 24);
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  const EpdFontData* const stubData = font.getEpdFont()->data;

  struct HeapFloorGuard {
    ~HeapFloorGuard() { ESP.freeHeap = 200 * 1024; }
  } guard;
  ESP.freeHeap = 52 * 1024;  // Tight C3 heap: allocating an 8 KB slab would breach the 50 KB floor.

  const auto text = page(FIRST, 20);
  EXPECT_EQ(21, font.prewarm(text.c_str(), 1, false, false, false));
  EXPECT_EQ(0u, font.getStats().bitmapBytes);
  EXPECT_NE(stubData, font.getEpdFont()->data);
  EXPECT_EQ(21u, residentCount(font));  // 20 page glyphs + U+FFFD stay in miniIntervals/miniGlyphs!

  // Repeat prewarm while heap stays tight hits resident metrics with 0 SD reads.
  font.clearCache();
  sdFontTestReads = 0;
  EXPECT_EQ(21, font.prewarm(text.c_str(), 1, false, false, false));
  EXPECT_EQ(0u, sdFontTestReads);

  // Metrics resolve purely from RAM (0 SD reads), and visible bitmaps fault via overflow_ LRU.
  const EpdFont* epd = font.getEpdFont();
  sdFontTestReads = 0;
  sdFontTestOpens = 0;
  const EpdGlyph* g = epd->getGlyph(FIRST + 7);
  ASSERT_NE(nullptr, g);
  EXPECT_NE(0u, g->dataOffset & SdCardFont::SPILLED_BITMAP_FLAG);
  EXPECT_EQ(0u, sdFontTestReads);
  EXPECT_EQ(0u, sdFontTestOpens);
  const uint8_t* bmp = SdCardFont::resolveGlyphBitmap(epd->data->glyphMissCtx, g);
  ASSERT_NE(nullptr, bmp);
  EXPECT_EQ(static_cast<uint8_t>((7u * 17u + 1u) & 0xFFu), bmp[0]);

  // Once heap recovers to SLAB_FIRST_MIN_FREE_HEAP (58 KB), prewarming upgrades from 0 slabs to Slab 0.
  font.clearCache();
  font.resetStats();
  ESP.freeHeap = 58 * 1024;
  EXPECT_EQ(0, font.prewarm(text.c_str(), 1, false, false, false));
  EXPECT_GT(font.getStats().bitmapBytes, 0u);
  const EpdGlyph* upgraded = epd->getGlyph(FIRST + 7);
  ASSERT_NE(nullptr, upgraded);
  EXPECT_EQ(0u, upgraded->dataOffset & SdCardFont::SPILLED_BITMAP_FLAG);
}

TEST(SdCardFontTest, FourBitMultiStyleTightHeapKeepsRegularMetricsKerningAndLigaturesWithoutStubDegradation) {
  makeMultiStyle4BitKerningFont();
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));

  const EpdFontData* const stubReg = font.getEpdFont(0)->data;
  const EpdFontData* const stubBold = font.getEpdFont(1)->data;
  const EpdFontData* const stubItal = font.getEpdFont(2)->data;

  struct TightHeapGuard {
    ~TightHeapGuard() {
      trackSlabHeap = false;
      ESP.freeHeap = 200 * 1024;
    }
  } guard;
  trackSlabHeap = true;
  ESP.freeHeap = 58 * 1024;  // Room above the 50 KB floor for exactly one 8 KB slab across all styles.

  // Scenario A: Bold (1) and Italic (2) prewarm before Regular (0) inside a PrewarmScope.
  // Bold claims the sole 8 KB slab (dropping freeHeap to 50 KB); Italic and Regular hit packedCount == 0
  // yet must retain miniGlyphs (SPILLED_BITMAP_FLAG), kerning, and ligatures with 0 stubData degradation.
  font.beginPrewarmScope();
  EXPECT_EQ(0, font.prewarm("ABCDEF", 0x02, false, true, false));
  EXPECT_EQ(50u * 1024u, ESP.freeHeap);
  EXPECT_EQ(7, font.prewarm("ABCDEF", 0x04, false, true, false));
  EXPECT_EQ(7, font.prewarm("ABCDEF", 0x01, false, true, false));

  const EpdFont* epdReg = font.getEpdFont(0);
  const EpdFont* epdBold = font.getEpdFont(1);
  const EpdFont* epdItal = font.getEpdFont(2);
  ASSERT_NE(nullptr, epdReg);
  ASSERT_NE(nullptr, epdBold);
  ASSERT_NE(nullptr, epdItal);

  EXPECT_NE(stubReg, epdReg->data);
  EXPECT_NE(stubBold, epdBold->data);
  EXPECT_NE(stubItal, epdItal->data);
  EXPECT_EQ(7u, residentCount(font, 0));
  EXPECT_EQ(7u, residentCount(font, 1));
  EXPECT_EQ(7u, residentCount(font, 2));

  // Regular retains full kerning & ligatures (0 px layout drift) despite 0 bitmap slabs.
  EXPECT_EQ(-3, epdReg->getKerning('A', 'B'));
  EXPECT_EQ(4, epdReg->getKerning('C', 'B'));
  EXPECT_EQ(-5, epdReg->getKerning('C', 'D'));
  EXPECT_EQ(static_cast<uint32_t>('A'), epdReg->getLigature('E', 'F'));
  EXPECT_EQ(-2, epdItal->getKerning('A', 'B'));
  EXPECT_EQ(-7, epdItal->getKerning('C', 'D'));
  EXPECT_EQ(static_cast<uint32_t>('A'), epdItal->getLigature('E', 'F'));

  // Regular glyph metrics resolve from miniGlyphs in RAM with 0 SD reads; bitmaps fault via scope file.
  sdFontTestReads = 0;
  sdFontTestOpens = 0;
  const EpdGlyph* regC = epdReg->getGlyph('C');
  ASSERT_NE(nullptr, regC);
  EXPECT_EQ(static_cast<uint16_t>(24 << 4), regC->advanceX);
  EXPECT_NE(0u, regC->dataOffset & SdCardFont::SPILLED_BITMAP_FLAG);
  EXPECT_EQ(0u, sdFontTestReads);
  EXPECT_EQ(0u, sdFontTestOpens);

  const uint8_t* regCBmp = SdCardFont::resolveGlyphBitmap(epdReg->data->glyphMissCtx, regC);
  ASSERT_NE(nullptr, regCBmp);
  EXPECT_EQ(0u, sdFontTestOpens);  // Reused cachedReadFile_ opened during beginPrewarmScope()!
  EXPECT_EQ(static_cast<uint8_t>((0u * 53u + 2u * 17u + 1u) & 0xFFu), regCBmp[0]);
  font.clearCache();

  // Scenario B: Forward style order (Regular -> Bold -> Italic, matching PrewarmScope::endScanAndPrewarm).
  // Regular claims Slab 0 first while Bold and Italic spill cleanly with full metrics/kerning/ligatures.
  font.releaseResidentCaches();
  ESP.freeHeap = 58 * 1024;
  font.beginPrewarmScope();
  EXPECT_EQ(14, font.prewarm("ABCDEF", 0x07, false, true, false));  // 0 missed for Regular + 7 Bold + 7 Italic
  EXPECT_NE(stubReg, epdReg->data);
  EXPECT_NE(stubBold, epdBold->data);
  EXPECT_NE(stubItal, epdItal->data);
  EXPECT_EQ(0u, epdReg->getGlyph('C')->dataOffset & SdCardFont::SPILLED_BITMAP_FLAG);
  EXPECT_NE(0u, epdBold->getGlyph('C')->dataOffset & SdCardFont::SPILLED_BITMAP_FLAG);
  EXPECT_NE(0u, epdItal->getGlyph('C')->dataOffset & SdCardFont::SPILLED_BITMAP_FLAG);
  EXPECT_EQ(-3, epdReg->getKerning('A', 'B'));
  EXPECT_EQ(-4, epdBold->getKerning('A', 'B'));
  EXPECT_EQ(-6, epdBold->getKerning('C', 'D'));
  EXPECT_EQ(-2, epdItal->getKerning('A', 'B'));
  EXPECT_EQ(-7, epdItal->getKerning('C', 'D'));
  EXPECT_EQ(static_cast<uint32_t>('A'), epdBold->getLigature('E', 'F'));
  EXPECT_EQ(static_cast<uint32_t>('A'), epdItal->getLigature('E', 'F'));
  font.clearCache();
}

TEST(SdCardFontTest, FourBitTightHeapLayoutBuildsAdvancesWithoutSlabsAndUpgradesToRender) {
  makeMultiStyle4BitKerningFont();
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));

  struct TightHeapGuard {
    ~TightHeapGuard() { ESP.freeHeap = 200 * 1024; }
  } guard;
  ESP.freeHeap = 52 * 1024;  // Tight C3 heap (< 58 KB first-slab floor).

  // Layout advance-table + metadata-only UI warming keeps advanceTable_ intact with 0 bitmap slabs.
  const EpdFontData* const stubReg = font.getEpdFont(0)->data;
  EXPECT_EQ(0, font.buildAdvanceTable("ABCDEF", 0x03));
  EXPECT_TRUE(font.hasAdvanceTable());
  EXPECT_EQ(static_cast<uint16_t>(24 << 4), font.getAdvance('A', 0));
  EXPECT_EQ(static_cast<uint16_t>(25 << 4), font.getAdvance('A', 1));
  EXPECT_EQ(0, font.prewarm("ABCDEF", 0x03, /*metadataOnly=*/true, /*loadKernLig=*/false, /*accumulate=*/true));
  EXPECT_TRUE(font.hasAdvanceTable());
  EXPECT_EQ(0u, font.getStats().bitmapBytes);
  EXPECT_NE(stubReg, font.getEpdFont(0)->data);
  EXPECT_EQ(nullptr, font.getEpdFont(0)->data->bitmap);

  // Restoring normal heap and running a render prewarm upgrades metadata-only mini to slab-backed bitmaps + kerning.
  ESP.freeHeap = 200 * 1024;
  EXPECT_EQ(0, font.prewarm("ABCDEF", 0x01, /*metadataOnly=*/false, /*loadKernLig=*/true, /*accumulate=*/false));
  EXPECT_GT(font.getStats().bitmapBytes, 0u);
  EXPECT_EQ(-3, font.getEpdFont(0)->getKerning('A', 'B'));
  const EpdGlyph* g = font.getEpdFont(0)->getGlyph('A');
  ASSERT_NE(nullptr, g);
  EXPECT_NE(nullptr, font.getEpdFont(0)->data->bitmapHandler(font.getEpdFont(0)->data->glyphMissCtx, g));
}

TEST(SdCardFontTest, TtfGamma4LutMatchesCalibratedReflectanceCurve) {
  EXPECT_EQ(0u, kTtfGamma4Lut[0]);
  EXPECT_EQ(15u, kTtfGamma4Lut[255]);
  for (int i = 0; i < 256; ++i) {
    const uint8_t expected =
        i <= 15 ? 0u : static_cast<uint8_t>(std::lround(15.0 * (1.0 - std::pow(1.0 - i / 255.0, 1.20))));
    EXPECT_EQ(expected, kTtfGamma4Lut[i]) << i;
    if (i > 0) {
      EXPECT_LE(kTtfGamma4Lut[i - 1], kTtfGamma4Lut[i]) << i;
    }
  }
  // Anchor thresholds for <= 15/255 halo dead-zone clip, 4-gray, and BW page turns.
  EXPECT_EQ(0u, kTtfGamma4Lut[15]);
  EXPECT_EQ(1u, kTtfGamma4Lut[16]);
  EXPECT_EQ(1u, kTtfGamma4Lut[21]);
  EXPECT_EQ(2u, kTtfGamma4Lut[22]);
  EXPECT_EQ(7u, kTtfGamma4Lut[111]);
  EXPECT_EQ(8u, kTtfGamma4Lut[112]);
  EXPECT_EQ(9u, kTtfGamma4Lut[128]);
  EXPECT_EQ(12u, kTtfGamma4Lut[180]);
}

TEST(SdCardFontTest, TtfPackGlyphBitmapMatchesAllBitDepthsAndGlyphBitmapPlanes) {
  EXPECT_EQ(0u, ttfPackedBitmapBytes(0, TtfBitDepth::Bpp1));
  EXPECT_EQ(0u, ttfPackedBitmapBytes(0, TtfBitDepth::Bpp2));
  EXPECT_EQ(0u, ttfPackedBitmapBytes(0, TtfBitDepth::Bpp4));
  EXPECT_EQ(2u, ttfPackedBitmapBytes(9, TtfBitDepth::Bpp1));
  EXPECT_EQ(3u, ttfPackedBitmapBytes(9, TtfBitDepth::Bpp2));
  EXPECT_EQ(5u, ttfPackedBitmapBytes(9, TtfBitDepth::Bpp4));

  // 3x3 odd-pixel glyph covering representative coverage buckets 0..255.
  const uint8_t src9[9] = {0, 12, 30, 64, 105, 128, 192, 225, 255};
  uint8_t packed4[5] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  packTtfGlyphBitmap(src9, 9, TtfBitDepth::Bpp4, packed4);
  for (uint32_t i = 0; i < 9; ++i) {
    const uint8_t nib = (i & 1u) ? (packed4[i >> 1] & 0x0Fu) : (packed4[i >> 1] >> 4);
    EXPECT_EQ(kTtfGamma4Lut[src9[i]], nib) << i;
  }
  // Trailing low nibble of odd-pixel 4bpp glyph must be zero-padded.
  EXPECT_EQ(0u, packed4[4] & 0x0Fu);

  // Verify 2bpp and 1bpp backward-compatible quantization & MSB-first packing.
  uint8_t packed2[3] = {0xFF, 0xFF, 0xFF};
  packTtfGlyphBitmap(src9, 9, TtfBitDepth::Bpp2, packed2);
  const uint8_t expected2[9] = {0, 0, 0, 1, 1, 2, 3, 3, 3};
  for (uint32_t i = 0; i < 9; ++i) {
    const uint8_t crumb = (packed2[i >> 2] >> ((3u - (i & 3u)) * 2u)) & 0x03u;
    EXPECT_EQ(expected2[i], crumb) << i;
  }
  EXPECT_EQ(0u, packed2[2] & 0x3Fu);

  uint8_t packed1[2] = {0xFF, 0xFF};
  packTtfGlyphBitmap(src9, 9, TtfBitDepth::Bpp1, packed1);
  for (uint32_t i = 0; i < 9; ++i) {
    const uint8_t bit = (packed1[i >> 3] >> (7u - (i & 7u))) & 0x01u;
    EXPECT_EQ(src9[i] >= 128 ? 1u : 0u, bit) << i;
  }
  EXPECT_EQ(0u, packed1[1] & 0x7Fu);

  // Verify EpdFontData bit-depth contract and GlyphBitmap::draw round-trip on a 16-entry ramp.
  EpdFontData d4{};
  d4.is2Bit = false;
  d4.is4Bit = true;
  EXPECT_EQ(4u, epdBitsPerPixel(d4));

  uint8_t rampSrc[16];
  for (int nib = 0; nib < 16; ++nib) {
    int found = 0;
    for (int a = 0; a < 256; ++a) {
      if (kTtfGamma4Lut[a] == nib) {
        found = a;
        break;
      }
    }
    rampSrc[nib] = static_cast<uint8_t>(found);
  }
  uint8_t rampPacked[8] = {};
  packTtfGlyphBitmap(rampSrc, 16, TtfBitDepth::Bpp4, rampPacked);

  auto evalPlane = [&](glyphBitmap::Plane plane, uint8_t initVal) -> uint16_t {
    uint8_t row[2] = {initVal, initVal};
    const glyphBitmap::Target target{row, 16, 2, 0, 1, {0, 0, 1, 0, 0, 1}};
    glyphBitmap::draw(rampPacked, 16, 1, epdBitsPerPixel(d4), plane, true, target, {0, 0, 16, 1});
    uint16_t mask = 0;
    for (int ink = 0; ink < 16; ++ink) {
      if (((row[ink / 8] >> (7 - (ink % 8))) & 1u) != 0) {
        mask |= static_cast<uint16_t>(1u << ink);
      }
    }
    return mask;
  };

  EXPECT_EQ(static_cast<uint16_t>(~glyphBitmap::kBw4BitMask), evalPlane(glyphBitmap::Plane::BW, 0xFFu));
  EXPECT_EQ(glyphBitmap::kGrayLsb4BitMask, evalPlane(glyphBitmap::Plane::GrayLSB, 0x00u));
  EXPECT_EQ(glyphBitmap::kGrayMsb4BitMask, evalPlane(glyphBitmap::Plane::GrayMSB, 0x00u));
}

TEST(SdCardFontTest, TtfTightCounterGuardPreservesOpenEyesWithoutTouchingOuterCurveAa) {
  auto readNib = [](const uint8_t* packed, uint32_t idx) -> uint8_t {
    const uint8_t byte = packed[idx >> 1];
    return (idx & 1u) ? static_cast<uint8_t>(byte & 0x0Fu) : static_cast<uint8_t>(byte >> 4);
  };

  // (a) 6x4 tight 'e'-like counter with a 2x1 interior eye at (2,1) and (3,1):
  //   (0,1) bbox edge has src=30 (ink=2) with 3 dark neighbors N=255, S=255, E=255 -> exterior edge -> stays 2!
  //   (2,1) has src=30 (ink=2 <= 3) enclosed by N=220(14), S=192(12), W=255(15), E=45(3) -> 3 dark walls -> snaps to 0!
  //   (3,1) has src=45 (ink=3 <= 3) enclosed by N=220(14), S=192(12), E=170(11), W=30(2) -> 3 dark walls -> snaps to 0!
  //   (4,2) has src=30 (ink=2 <= 3) with only 2 dark walls (N=170[11], W=192[12], S=150[10], E=0[0]) -> stays 2!
  const uint8_t eGlyph6x4[24] = {
      255, 220, 220, 220, 170, 0,  // row 0: top arch
      30,  255, 30,  45,  170, 0,  // row 1: left bbox AA (30), left stem (255), 2px eye (30, 45), right arch (170)
      255, 255, 192, 192, 30,  0,  // row 2: crossbar + open aperture AA at (4,2)
      0,   0,   200, 150, 150, 0,  // row 3: bottom curve
  };
  uint8_t packed4[12] = {};
  const size_t allocsBefore = nothrowArrayAllocations;
  packTtfGlyphBitmap(eGlyph6x4, 6, 4, TtfBitDepth::Bpp4, packed4);
  EXPECT_EQ(allocsBefore, nothrowArrayAllocations) << "Counter guard must perform 0 heap allocations";

  EXPECT_EQ(2u, readNib(packed4, 1 * 6 + 0)) << "Exterior bbox edge pixel must stay untouched";
  EXPECT_EQ(0u, readNib(packed4, 1 * 6 + 2)) << "Left tight-eye pixel (ink=2, 3 walls >= 11) must snap to 0";
  EXPECT_EQ(0u, readNib(packed4, 1 * 6 + 3)) << "Right tight-eye pixel (ink=3, 3 walls >= 11) must snap to 0";
  EXPECT_EQ(2u, readNib(packed4, 2 * 6 + 4)) << "2-wall open-counter AA pixel (third neighbor ink=10) must stay 2";

  // (b) Interior bridge stroke with ink=4 (src=55) surrounded by 4 solid walls (255 -> ink=15) is NOT hollowed:
  const uint8_t bridge3x3[9] = {
      0, 255, 0, 255, 55, 255, 0, 255, 0,
  };
  uint8_t bridgePacked[5] = {};
  packTtfGlyphBitmap(bridge3x3, 3, 3, TtfBitDepth::Bpp4, bridgePacked);
  EXPECT_EQ(4u, readNib(bridgePacked, 4)) << "ink=4 (> 3) must not be cleared";

  // (c) Small (< 3x3) bitmaps and 2bpp/1bpp paths do not alter pixels via the 4bpp counter guard:
  uint8_t packed2[6] = {};
  packTtfGlyphBitmap(eGlyph6x4, 6, 4, TtfBitDepth::Bpp2, packed2);
  uint8_t expectedPacked2[6] = {};
  packTtfGlyphBitmap(eGlyph6x4, 24, TtfBitDepth::Bpp2, expectedPacked2);
  for (size_t i = 0; i < 6; ++i) {
    EXPECT_EQ(expectedPacked2[i], packed2[i]) << i;
  }
}

TEST(SdCardFontTest, CompactForRenderDropsAdvanceCacheAndFreesInactiveStyles) {
  makeKerningFont(/*extraEntries=*/200, /*classCount=*/80, /*extraClass=*/0, /*ligature=*/true);
  SdCardFont font;
  ASSERT_TRUE(font.load("kern.cpfont"));
  const EpdFontData* const stubData = font.getEpdFont(0)->data;

  std::string layoutText = "ABCDEF";
  for (uint32_t cp = 0x0100; cp < 0x01B0; ++cp) {
    char buf[5] = {};
    utf8AppendCodepoint(buf, cp);
    layoutText += buf;
  }
  EXPECT_EQ(0, font.buildAdvanceTable(layoutText.c_str(), 0x01));
  EXPECT_TRUE(font.hasAdvanceTable());

  // Page 1 render prewarm ("ABCD") + immediate compactForRender:
  EXPECT_EQ(0, font.prewarm("ABCD", 0x01, /*metadataOnly=*/false, /*loadKernLig=*/true, /*accumulate=*/false));
  EXPECT_NE(stubData, font.getEpdFont(0)->data);
  font.compactForRender(0x01, /*dropLayoutAdvanceCache=*/true);
  EXPECT_FALSE(font.hasAdvanceTable());
  EXPECT_NE(stubData, font.getEpdFont(0)->data);
  EXPECT_EQ(-3, font.getEpdFont(0)->getKerning('A', 'B'));
  EXPECT_EQ(-5, font.getEpdFont(0)->getKerning('C', 'D'));
  const EpdGlyph* gA = font.getEpdFont(0)->getGlyph('A');
  ASSERT_NE(nullptr, gA);
  EXPECT_NE(nullptr, font.getEpdFont(0)->data->bitmapHandler(font.getEpdFont(0)->data->glyphMissCtx, gA));

  // Unreferenced font (activeStyleMask = 0x00) releases mini arenas cleanly back to stubData.
  font.compactForRender(0x00, /*dropLayoutAdvanceCache=*/true);
  EXPECT_EQ(stubData, font.getEpdFont(0)->data);
}

TEST(SdCardFontTest, PrewarmScopeRetainsOpenFileAcrossCompactForRenderAndClosesAtClearCache) {
  // 29 x 29 = 421 bytes/glyph -> 76 glyphs fit in 4 slabs (32 KB), remaining 75 glyphs spill.
  make4BitFont(200, 29, 29);
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));

  const auto text = page(FIRST, 150);
  ASSERT_EQ(0, font.buildAdvanceTable(text.c_str(), 0x01));
  ASSERT_TRUE(font.hasAdvanceTable());

  sdFontTestOpens = 0;
  sdFontTestCloses = 0;
  font.beginPrewarmScope();
  EXPECT_GT(font.prewarm(text.c_str(), 0x01, /*metadataOnly=*/false, /*loadKernLig=*/false, /*accumulate=*/false), 0);
  EXPECT_EQ(1u, sdFontTestOpens);
  EXPECT_EQ(0u, sdFontTestCloses);

  // compactForRender drops the layout advance cache but MUST keep cachedReadFile_ + scopeActive_
  // open because active style 0 has spilled glyphs (miniSpilledCount > 0).
  font.compactForRender(0x01, /*dropLayoutAdvanceCache=*/true);
  EXPECT_FALSE(font.hasAdvanceTable());
  EXPECT_EQ(1u, sdFontTestOpens);
  EXPECT_EQ(0u, sdFontTestCloses);

  // Render >16 (24) distinct spilled glyphs across two passes (simulating BW + grayscale strip passes):
  // LRU evictions past OVERFLOW_CAPACITY (16) re-read from SD, reusing cachedReadFile_ with 0 new opens.
  const EpdFont* epd = font.getEpdFont(0);
  ASSERT_NE(nullptr, epd);
  for (int pass = 0; pass < 2; ++pass) {
    for (uint32_t idx = 100; idx < 124; ++idx) {
      const EpdGlyph* g = epd->getGlyph(FIRST + idx);
      ASSERT_NE(nullptr, g);
      ASSERT_NE(0u, g->dataOffset & SdCardFont::SPILLED_BITMAP_FLAG);
      const uint8_t* bmp = epd->data->bitmapHandler(epd->data->glyphMissCtx, g);
      ASSERT_NE(nullptr, bmp);
      for (uint16_t b = 0; b < 421; ++b) {
        ASSERT_EQ(static_cast<uint8_t>((idx * 17u + b * 3u + 1u) & 0xFFu), bmp[b]) << pass << ":" << idx;
      }
      EXPECT_EQ(1u, sdFontTestOpens) << "pass=" << pass << " idx=" << idx;
      EXPECT_EQ(0u, sdFontTestCloses) << "pass=" << pass << " idx=" << idx;
    }
  }

  // Scope exit (clearCache) closes cachedReadFile_ and clears scopeActive_.
  font.clearCache();
  EXPECT_EQ(1u, sdFontTestCloses);

  // Subsequent miss outside PrewarmScope opens a fresh local file handle.
  const EpdGlyph* postScopeGlyph = epd->getGlyph(FIRST + 130);
  ASSERT_NE(nullptr, postScopeGlyph);
  ASSERT_NE(nullptr, epd->data->bitmapHandler(epd->data->glyphMissCtx, postScopeGlyph));
  EXPECT_EQ(2u, sdFontTestOpens);

  // When an active scope has zero spilled glyphs, compactForRender closes cachedReadFile_ immediately
  // while keeping scopeActive_ alive until clearCache().
  font.releaseResidentCaches();
  sdFontTestOpens = 0;
  sdFontTestCloses = 0;
  font.beginPrewarmScope();
  EXPECT_EQ(0, font.prewarm(page(FIRST, 10).c_str(), 0x01, /*metadataOnly=*/false, /*loadKernLig=*/false,
                            /*accumulate=*/false));
  EXPECT_EQ(1u, sdFontTestOpens);
  EXPECT_EQ(0u, sdFontTestCloses);
  font.compactForRender(0x01, /*dropLayoutAdvanceCache=*/true);
  EXPECT_EQ(1u, sdFontTestCloses);
  font.clearCache();
}

TEST(SdCardFontTest, MetadataOnlyPrewarmFaultsBitmapsOnDemandViaResolveGlyphBitmap) {
  // 1) 2bpp font: metadataOnly UI width check followed directly by un-scoped glyph bitmap draw.
  {
    makeFont(40);
    SdCardFont font;
    ASSERT_TRUE(font.load("fixture"));
    ASSERT_EQ(0, font.prewarm(page(FIRST, 5).c_str(), 0x01, /*metadataOnly=*/true, /*loadKernLig=*/false));
    EXPECT_EQ(0u, font.getStats().bitmapBytes);
    const EpdFont* epd = font.getEpdFont(0);
    EXPECT_EQ(nullptr, epd->data->bitmap);

    const EpdGlyph* g = epd->getGlyph(FIRST + 3);
    ASSERT_NE(nullptr, g);
    sdFontTestReads = 0;
    const uint8_t* bmp = epd->data->bitmapHandler(epd->data->glyphMissCtx, g);
    ASSERT_NE(nullptr, bmp);
    EXPECT_EQ(1u, sdFontTestReads);
    for (uint16_t b = 0; b < BITMAP_BYTES; ++b) {
      ASSERT_EQ(3u % 251u, bmp[b]) << b;
    }

    // Second lookup hits overflow_ LRU with 0 SD reads.
    sdFontTestReads = 0;
    EXPECT_EQ(bmp, epd->data->bitmapHandler(epd->data->glyphMissCtx, g));
    EXPECT_EQ(0u, sdFontTestReads);
  }

  // 2) 4bpp font: metadataOnly UI width check followed directly by un-scoped glyph bitmap draw.
  {
    make4BitFont(40, 16, 16);
    SdCardFont font;
    ASSERT_TRUE(font.load("fixture"));
    ASSERT_EQ(0, font.prewarm(page(FIRST, 5).c_str(), 0x01, /*metadataOnly=*/true, /*loadKernLig=*/false));
    EXPECT_EQ(0u, font.getStats().bitmapBytes);
    const EpdFont* epd = font.getEpdFont(0);

    const EpdGlyph* g = epd->getGlyph(FIRST + 2);
    ASSERT_NE(nullptr, g);
    sdFontTestReads = 0;
    const uint8_t* bmp = epd->data->bitmapHandler(epd->data->glyphMissCtx, g);
    ASSERT_NE(nullptr, bmp);
    EXPECT_EQ(1u, sdFontTestReads);
    for (uint16_t b = 0; b < 128; ++b) {
      ASSERT_EQ(static_cast<uint8_t>((2u * 17u + b * 3u + 1u) & 0xFFu), bmp[b]) << b;
    }

    sdFontTestReads = 0;
    EXPECT_EQ(bmp, epd->data->bitmapHandler(epd->data->glyphMissCtx, g));
    EXPECT_EQ(0u, sdFontTestReads);
  }
}

TEST(SdCardFontTest, OversizedFourBitGlyphSpillsWithoutAbortingSubsequentSlabPacking) {
  // Build a 4bpp font with 7 glyphs (FIRST..FIRST+5 and U+FFFD):
  //   - glyph 1 (FIRST + 1) is 130 x 130 = 16,900 px -> 8,450 bytes (> MINI_SLAB_SIZE = 8,192 B)
  //   - glyphs 0, 2, 3, 4, 5, and U+FFFD (6) are 16 x 16 = 256 px -> 128 bytes (<= MINI_SLAB_SIZE)
  constexpr uint32_t COUNT = 7;
  constexpr size_t GLYPH_OFFSET = 64 + 24;
  constexpr uint16_t HUGE_W = 130;
  constexpr uint16_t HUGE_H = 130;
  constexpr uint16_t HUGE_BYTES = (HUGE_W * HUGE_H + 1) / 2;  // 8,450 B > 8,192 B
  constexpr uint16_t NORM_W = 16;
  constexpr uint16_t NORM_H = 16;
  constexpr uint16_t NORM_BYTES = (NORM_W * NORM_H + 1) / 2;  // 128 B
  const size_t bitmapOffset = GLYPH_OFFSET + COUNT * sizeof(EpdGlyph);
  const size_t totalBmpBytes = HUGE_BYTES + (COUNT - 1) * NORM_BYTES;

  sdFontTestFile.assign(bitmapOffset + totalBmpBytes, 0);
  std::memcpy(sdFontTestFile.data(), "CPFONT\0\0", 8);
  put16(8, CPFONT_VERSION);
  put16(10, SdCardFont::CPFONT_FLAG_4BIT);
  sdFontTestFile[12] = 1;
  put32(36, 2);
  put32(40, COUNT);
  sdFontTestFile[44] = 36;
  put16(45, 30);
  put32(56, 64);
  put32(64, FIRST);
  put32(68, FIRST + COUNT - 2);
  put32(76, 0xFFFD);
  put32(80, 0xFFFD);
  put32(84, COUNT - 1);

  uint32_t curBmpOff = 0;
  for (uint32_t i = 0; i < COUNT; ++i) {
    const bool huge = (i == 1);
    const uint8_t w = huge ? HUGE_W : NORM_W;
    const uint8_t h = huge ? HUGE_H : NORM_H;
    const uint16_t bytes = huge ? HUGE_BYTES : NORM_BYTES;
    EpdGlyph glyph{};
    glyph.width = w;
    glyph.height = h;
    glyph.advanceX = static_cast<uint16_t>(w << 4);
    glyph.top = h;
    glyph.dataLength = bytes;
    glyph.dataOffset = curBmpOff;
    std::memcpy(sdFontTestFile.data() + GLYPH_OFFSET + i * sizeof(glyph), &glyph, sizeof(glyph));
    uint8_t* dst = sdFontTestFile.data() + bitmapOffset + curBmpOff;
    for (uint16_t b = 0; b < bytes; ++b) {
      dst[b] = static_cast<uint8_t>((i * 23u + b * 5u + 7u) & 0xFFu);
    }
    curBmpOff += bytes;
  }

  SdCardFont font;
  ASSERT_TRUE(font.load("huge4b.cpfont"));

  // Prewarm FIRST..FIRST+5 (+ U+FFFD): only glyph 1 (> 8 KB) spills; glyphs 0 and 2..5 + U+FFFD pack into Slab 0!
  EXPECT_EQ(1, font.prewarm(page(FIRST, 6).c_str(), 0x01, /*metadataOnly=*/false, /*loadKernLig=*/false,
                            /*accumulate=*/false));
  EXPECT_EQ(6u * NORM_BYTES, font.getStats().bitmapBytes);

  const EpdFont* epd = font.getEpdFont(0);
  const EpdGlyph* gHuge = epd->getGlyph(FIRST + 1);
  ASSERT_NE(nullptr, gHuge);
  EXPECT_EQ(HUGE_W, gHuge->width);
  EXPECT_EQ(HUGE_H, gHuge->height);
  EXPECT_EQ(HUGE_BYTES, gHuge->dataLength);
  EXPECT_NE(0u, gHuge->dataOffset & SdCardFont::SPILLED_BITMAP_FLAG);

  // Subsequent normal glyphs (2..5) were NOT aborted or zeroed out — they resolve from Slab 0 with 0 SD reads!
  sdFontTestReads = 0;
  for (uint32_t idx : {0u, 2u, 3u, 4u, 5u}) {
    const EpdGlyph* gNorm = epd->getGlyph(FIRST + idx);
    ASSERT_NE(nullptr, gNorm);
    EXPECT_EQ(NORM_BYTES, gNorm->dataLength) << idx;
    EXPECT_EQ(0u, gNorm->dataOffset & SdCardFont::SPILLED_BITMAP_FLAG) << idx;
    const uint8_t* bmp = epd->data->bitmapHandler(epd->data->glyphMissCtx, gNorm);
    ASSERT_NE(nullptr, bmp) << idx;
    for (uint16_t b = 0; b < NORM_BYTES; ++b) {
      ASSERT_EQ(static_cast<uint8_t>((idx * 23u + b * 5u + 7u) & 0xFFu), bmp[b]) << idx << ":" << b;
    }
  }
  EXPECT_EQ(0u, sdFontTestReads);

  // Oversized glyph 1 faults cleanly via loadSpilledBitmap into overflow_ and returns all 8,450 bytes intact.
  const uint8_t* hugeBmp = epd->data->bitmapHandler(epd->data->glyphMissCtx, gHuge);
  ASSERT_NE(nullptr, hugeBmp);
  EXPECT_EQ(1u, sdFontTestReads);
  for (uint16_t b = 0; b < HUGE_BYTES; ++b) {
    ASSERT_EQ(static_cast<uint8_t>((1u * 23u + b * 5u + 7u) & 0xFFu), hugeBmp[b]) << b;
  }

  // Repeat prewarm after clearCache subset-hits with 0 SD reads because miniSlabCapped is false.
  font.clearCache();
  sdFontTestReads = 0;
  EXPECT_EQ(1, font.prewarm(page(FIRST, 6).c_str(), 0x01, /*metadataOnly=*/false, /*loadKernLig=*/false,
                            /*accumulate=*/false));
  EXPECT_EQ(0u, sdFontTestReads);
}

TEST(SdCardFontTest, IncrementalAccumulateOnSpilledSlabCappedSuffixSkipsRedundantRebuilds) {
  // 29 x 29 = 421 bytes/glyph -> 76 glyphs pack into 4 slabs (32 KB), 75 glyphs spill (74 page + U+FFFD).
  make4BitFont(200, 29, 29);
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));

  const int initialMissed =
      font.prewarm(page(FIRST, 150).c_str(), 0x01, /*metadataOnly=*/false, /*loadKernLig=*/false, /*accumulate=*/false);
  EXPECT_EQ(75, initialMissed);
  EXPECT_EQ(151u, residentCount(font));

  // Repeated incremental UI prewarms (accumulate=true) for 10 glyphs in the spilled suffix are already
  // covered by resident miniIntervals; unioning would yield the identical 151-glyph set, so 0 SD reads occur.
  const std::string spilledRow = page(FIRST + 120, 10);
  sdFontTestReads = 0;
  sdFontTestOpens = 0;
  for (int pass = 0; pass < 3; ++pass) {
    EXPECT_EQ(11, font.prewarm(spilledRow.c_str(), 0x01, /*metadataOnly=*/false, /*loadKernLig=*/false,
                               /*accumulate=*/true))
        << "pass=" << pass;
    EXPECT_EQ(0u, sdFontTestReads) << "pass=" << pass;
    EXPECT_EQ(0u, sdFontTestOpens) << "pass=" << pass;
    EXPECT_EQ(151u, residentCount(font)) << "pass=" << pass;
  }

  // Resident prefix glyph (FIRST + 0) remains packed in Slab 0 and resolves with 0 SD reads.
  const EpdFont* epd = font.getEpdFont(0);
  ASSERT_NE(nullptr, epd);
  const EpdGlyph* prefixGlyph = epd->getGlyph(FIRST);
  ASSERT_NE(nullptr, prefixGlyph);
  EXPECT_EQ(0u, prefixGlyph->dataOffset & SdCardFont::SPILLED_BITMAP_FLAG);
  EXPECT_NE(nullptr, epd->data->bitmapHandler(epd->data->glyphMissCtx, prefixGlyph));
  EXPECT_EQ(0u, sdFontTestReads);

  // Conversely, a complete request-only render prewarm (accumulate=false) on those same 10 glyphs
  // evicts unused resident prefix glyphs and repacks all 10 + U+FFFD cleanly into Slab 0.
  EXPECT_EQ(0, font.prewarm(spilledRow.c_str(), 0x01, /*metadataOnly=*/false, /*loadKernLig=*/false,
                            /*accumulate=*/false));
  EXPECT_EQ(11u, residentCount(font));
  sdFontTestReads = 0;
  for (uint32_t idx = 120; idx < 130; ++idx) {
    const EpdGlyph* g = epd->getGlyph(FIRST + idx);
    ASSERT_NE(nullptr, g) << idx;
    EXPECT_EQ(0u, g->dataOffset & SdCardFont::SPILLED_BITMAP_FLAG) << idx;
    const uint8_t* bmp = epd->data->bitmapHandler(epd->data->glyphMissCtx, g);
    ASSERT_NE(nullptr, bmp) << idx;
    for (uint16_t b = 0; b < 421; ++b) {
      ASSERT_EQ(static_cast<uint8_t>((idx * 17u + b * 3u + 1u) & 0xFFu), bmp[b]) << idx << ":" << b;
    }
  }
  EXPECT_EQ(0u, sdFontTestReads);
}

TEST(SdCardFontTest, NoteScannedStylePreservesBoldKerningAcrossCompactForRenderPastScanCap) {
  // Build a 2-style (Regular=0, Bold=1) 4bpp .cpfont covering 'A'..'Z' + U+FFFD with ('A', 'V') kerning.
  constexpr uint8_t STYLE_COUNT = 2;
  constexpr uint32_t GLYPH_COUNT = 27;  // 'A'..'Z' (0..25) + U+FFFD (26)
  constexpr uint8_t WIDTH = 20;
  constexpr uint8_t HEIGHT = 20;
  constexpr uint16_t PACKED_BYTES = (WIDTH * HEIGHT + 1) / 2;  // 200 B
  constexpr size_t HEADER_AND_TOC = 32 + STYLE_COUNT * 32;
  constexpr size_t INTERVAL_BYTES = 2 * sizeof(EpdUnicodeInterval);
  constexpr size_t GLYPH_BYTES = GLYPH_COUNT * sizeof(EpdGlyph);
  constexpr size_t KERN_TABLE_BYTES = sizeof(EpdKernClassEntry);
  constexpr size_t KERN_MATRIX_BYTES = 1;
  constexpr size_t BITMAP_SECTION_BYTES = GLYPH_COUNT * PACKED_BYTES;
  constexpr size_t PER_STYLE_BYTES =
      INTERVAL_BYTES + GLYPH_BYTES + KERN_TABLE_BYTES * 2 + KERN_MATRIX_BYTES + BITMAP_SECTION_BYTES;
  constexpr int8_t AV_KERN[STYLE_COUNT] = {-3, -5};

  sdFontTestFile.assign(HEADER_AND_TOC + STYLE_COUNT * PER_STYLE_BYTES, 0);
  std::memcpy(sdFontTestFile.data(), "CPFONT\0\0", 8);
  put16(8, CPFONT_VERSION);
  put16(10, SdCardFont::CPFONT_FLAG_4BIT);
  sdFontTestFile[12] = STYLE_COUNT;

  for (uint8_t s = 0; s < STYLE_COUNT; ++s) {
    const size_t tocOff = 32 + s * 32;
    const size_t baseOff = HEADER_AND_TOC + s * PER_STYLE_BYTES;
    sdFontTestFile[tocOff + 0] = s;
    put32(tocOff + 4, 2);
    put32(tocOff + 8, GLYPH_COUNT);
    sdFontTestFile[tocOff + 12] = 32;
    put16(tocOff + 13, 26);
    put16(tocOff + 15, static_cast<uint16_t>(static_cast<int16_t>(-6)));
    put16(tocOff + 17, 1);  // kernLeftEntryCount ('A' -> class 1)
    put16(tocOff + 19, 1);  // kernRightEntryCount ('V' -> class 1)
    sdFontTestFile[tocOff + 21] = 1;
    sdFontTestFile[tocOff + 22] = 1;
    sdFontTestFile[tocOff + 23] = 0;
    put32(tocOff + 24, static_cast<uint32_t>(baseOff));

    put32(baseOff + 0, 'A');
    put32(baseOff + 4, 'Z');
    put32(baseOff + 8, 0);
    put32(baseOff + 12, 0xFFFD);
    put32(baseOff + 16, 0xFFFD);
    put32(baseOff + 20, GLYPH_COUNT - 1);

    const size_t glyphOff = baseOff + INTERVAL_BYTES;
    const size_t kernOff = glyphOff + GLYPH_BYTES;
    const size_t bmpOff = kernOff + KERN_TABLE_BYTES * 2 + KERN_MATRIX_BYTES;
    for (uint32_t i = 0; i < GLYPH_COUNT; ++i) {
      EpdGlyph glyph{};
      glyph.width = WIDTH;
      glyph.height = HEIGHT;
      glyph.advanceX = static_cast<uint16_t>((WIDTH + s) << 4);
      glyph.top = HEIGHT;
      glyph.dataLength = PACKED_BYTES;
      glyph.dataOffset = i * PACKED_BYTES;
      std::memcpy(sdFontTestFile.data() + glyphOff + i * sizeof(glyph), &glyph, sizeof(glyph));
      uint8_t* dst = sdFontTestFile.data() + bmpOff + glyph.dataOffset;
      for (uint16_t b = 0; b < PACKED_BYTES; ++b) {
        dst[b] = static_cast<uint8_t>((s * 41u + i * 13u + b + 1u) & 0xFFu);
      }
    }
    put16(kernOff + 0, 'A');
    sdFontTestFile[kernOff + 2] = 1;
    put16(kernOff + 3, 'V');
    sdFontTestFile[kernOff + 5] = 1;
    sdFontTestFile[kernOff + 6] = static_cast<uint8_t>(AV_KERN[s]);
  }

  SdCardFont font;
  ASSERT_TRUE(font.load("avkern.cpfont"));
  const EpdFont* epdReg = font.getEpdFont(0);
  const EpdFont* epdBold = font.getEpdFont(1);
  ASSERT_NE(nullptr, epdReg);
  ASSERT_NE(nullptr, epdBold);
  const EpdFontData* const stubBold = epdBold->data;

  // Page 1 warms both Regular and Bold ("ABCDV"), establishing resident Bold mini glyphs + kerning.
  font.beginPrewarmScope();
  font.noteScannedStyle(0);
  font.noteScannedStyle(1);
  ASSERT_EQ(0, font.prewarm("ABCDV", 0x03, /*metadataOnly=*/false, /*loadKernLig=*/true, /*accumulate=*/false));
  font.compactForRender(0x03, /*dropLayoutAdvanceCache=*/true);
  EXPECT_EQ(-3, epdReg->getKerning('A', 'V'));
  EXPECT_EQ(-5, epdBold->getKerning('A', 'V'));
  font.clearCache();

  // Page 2 dense scan: Regular fills all 512 scan codepoints before Bold "AV" is scanned.
  // noteScannedStyle(1) records Bold on the font even when prewarm only runs for Regular (0x01)
  // and compactForRender receives 0x01, preserving Bold's resident mini kerning and slab bitmaps.
  font.beginPrewarmScope();
  EXPECT_EQ(0u, font.scannedStyleMask());
  font.noteScannedStyle(0);
  font.noteScannedStyle(1);
  EXPECT_EQ(0x03u, font.scannedStyleMask());
  ASSERT_EQ(0, font.prewarm("ABCDV", 0x01, /*metadataOnly=*/false, /*loadKernLig=*/true, /*accumulate=*/false));
  font.compactForRender(0x01, /*dropLayoutAdvanceCache=*/true);

  EXPECT_NE(stubBold, epdBold->data);
  EXPECT_NE(0, epdBold->getKerning('A', 'V'));
  EXPECT_EQ(-5, epdBold->getKerning('A', 'V'));
  sdFontTestReads = 0;
  const EpdGlyph* boldA = epdBold->getGlyph('A');
  ASSERT_NE(nullptr, boldA);
  EXPECT_NE(nullptr, epdBold->data->bitmapHandler(epdBold->data->glyphMissCtx, boldA));
  EXPECT_EQ(0u, sdFontTestReads);
  font.clearCache();

  // Page 3 scans Regular only: beginPrewarmScope resets scannedStyleMask_ so Bold is now evicted.
  font.beginPrewarmScope();
  font.noteScannedStyle(0);
  EXPECT_EQ(0x01u, font.scannedStyleMask());
  font.compactForRender(0x01, /*dropLayoutAdvanceCache=*/true);
  EXPECT_EQ(stubBold, epdBold->data);
  EXPECT_EQ(0, epdBold->getKerning('A', 'V'));
  font.clearCache();
}

TEST(SdCardFontTest, FourBitFontWithShapingSectionLoadsAndPacksGlyphTokens) {
  // 4-bit .cpfont (flags=0x0002) with a trailing CPSH v2 shaping section and
  // shaped glyph tokens at U+F0000..U+F0003.
  constexpr uint8_t STYLE_COUNT = 2;
  constexpr uint32_t SHAPED_COUNT = 4;
  constexpr uint32_t GLYPH_COUNT = 2 + SHAPED_COUNT;  // U+0915, U+FFFD, U+F0000..U+F0003
  constexpr uint8_t WIDTH = 11;
  constexpr uint8_t HEIGHT = 13;
  constexpr uint16_t PACKED_BYTES = (WIDTH * HEIGHT + 1) / 2;  // 72 B
  constexpr size_t HEADER_AND_TOC = 32 + STYLE_COUNT * 32;
  constexpr size_t INTERVAL_BYTES = 3 * sizeof(EpdUnicodeInterval);
  constexpr size_t GLYPH_BYTES = GLYPH_COUNT * sizeof(EpdGlyph);
  constexpr size_t BITMAP_BYTES = GLYPH_COUNT * PACKED_BYTES;
  constexpr size_t PER_STYLE_BYTES = INTERVAL_BYTES + GLYPH_BYTES + BITMAP_BYTES;
  constexpr size_t SHAPING_HDR = 20;
  constexpr size_t SHAPING_BLOB_LEN = 32;
  constexpr size_t SHAPING_SEC_LEN = SHAPING_HDR + SHAPING_BLOB_LEN;

  const auto buildFixture = [&](const bool withShaping) {
    const size_t dataEnd = HEADER_AND_TOC + STYLE_COUNT * PER_STYLE_BYTES;
    sdFontTestFile.assign(dataEnd + (withShaping ? STYLE_COUNT * SHAPING_SEC_LEN : 0), 0);
    std::memcpy(sdFontTestFile.data(), "CPFONT\0\0", 8);
    put16(8, CPFONT_VERSION);
    put16(10, SdCardFont::CPFONT_FLAG_4BIT);
    sdFontTestFile[12] = STYLE_COUNT;

    for (uint8_t s = 0; s < STYLE_COUNT; ++s) {
      const size_t tocOff = 32 + s * 32;
      const size_t baseOff = HEADER_AND_TOC + s * PER_STYLE_BYTES;
      const size_t shapingOff = withShaping ? (dataEnd + s * SHAPING_SEC_LEN) : 0;
      sdFontTestFile[tocOff + 0] = s;
      put32(tocOff + 4, 3);
      put32(tocOff + 8, GLYPH_COUNT);
      sdFontTestFile[tocOff + 12] = 32;
      put16(tocOff + 13, 24);
      put16(tocOff + 15, static_cast<uint16_t>(static_cast<int16_t>(-8)));
      put32(tocOff + 24, static_cast<uint32_t>(baseOff));
      put32(tocOff + 28, static_cast<uint32_t>(shapingOff));

      // Intervals: U+0915 (0), U+FFFD (1), U+F0000..U+F0003 (2..5)
      put32(baseOff + 0, 0x0915);
      put32(baseOff + 4, 0x0915);
      put32(baseOff + 8, 0);
      put32(baseOff + 12, 0xFFFD);
      put32(baseOff + 16, 0xFFFD);
      put32(baseOff + 20, 1);
      put32(baseOff + 24, shaping::GLYPH_TOKEN_BASE);
      put32(baseOff + 28, shaping::GLYPH_TOKEN_BASE + SHAPED_COUNT - 1);
      put32(baseOff + 32, 2);

      const size_t glyphOff = baseOff + INTERVAL_BYTES;
      const size_t bmpOff = glyphOff + GLYPH_BYTES;
      for (uint32_t i = 0; i < GLYPH_COUNT; ++i) {
        EpdGlyph glyph{};
        glyph.width = WIDTH;
        glyph.height = HEIGHT;
        glyph.advanceX = static_cast<uint16_t>((WIDTH + s) << 4);
        glyph.top = HEIGHT;
        glyph.dataLength = PACKED_BYTES;
        glyph.dataOffset = i * PACKED_BYTES;
        std::memcpy(sdFontTestFile.data() + glyphOff + i * sizeof(glyph), &glyph, sizeof(glyph));
        uint8_t* dst = sdFontTestFile.data() + bmpOff + glyph.dataOffset;
        for (uint16_t b = 0; b < PACKED_BYTES; ++b) {
          dst[b] = static_cast<uint8_t>((s * 67u + i * 23u + b + 5u) & 0xFFu);
        }
      }

      if (withShaping) {
        std::memcpy(sdFontTestFile.data() + shapingOff, "CPSH", 4);
        put16(shapingOff + 4, 2);                  // SHAPING_VERSION
        put32(shapingOff + 8, 1600);               // ppem26_6 (25 px)
        put32(shapingOff + 12, SHAPING_BLOB_LEN);  // blobLength
        put32(shapingOff + 16, 0x12345670u + s);   // blobHash
        for (size_t b = 0; b < SHAPING_BLOB_LEN; ++b) {
          sdFontTestFile[shapingOff + SHAPING_HDR + b] = static_cast<uint8_t>(0xA0u + s + b);
        }
      }
    }
  };

  uint32_t plainHash = 0;
  {
    buildFixture(/*withShaping=*/false);
    SdCardFont unshapedFont;
    ASSERT_TRUE(unshapedFont.load("indic4bit_plain.cpfont"));
    plainHash = unshapedFont.contentHash();
    EXPECT_EQ(nullptr, unshapedFont.getEpdFont(0)->data->shapeHandler);
  }

  buildFixture(/*withShaping=*/true);
  SdCardFont font;
  ASSERT_TRUE(font.load("indic4bit_shaped.cpfont"));
  EXPECT_NE(plainHash, font.contentHash());

  const EpdFont* epdReg = font.getEpdFont(0);
  const EpdFont* epdBold = font.getEpdFont(1);
  ASSERT_NE(nullptr, epdReg);
  ASSERT_NE(nullptr, epdBold);
  const EpdFontData* const stubBold = epdBold->data;
  EXPECT_TRUE(epdReg->data->is4Bit);
  EXPECT_FALSE(epdReg->data->is2Bit);
  EXPECT_NE(nullptr, epdReg->data->shapeHandler);
  EXPECT_EQ(&SdCardFont::resolveGlyphBitmap, epdReg->data->bitmapHandler);
  EXPECT_NE(nullptr, stubBold->shapeHandler);
  EXPECT_EQ(&SdCardFont::resolveGlyphBitmap, stubBold->bitmapHandler);

  // Build a UTF-8 stream mixing position tokens and 4-byte shaped glyph tokens.
  std::string shapedStream;
  utf8AppendCodepoint(shaping::advanceToken(160), shapedStream);
  utf8AppendCodepoint(shaping::offsetToken(2, -1), shapedStream);
  for (uint32_t gid = 0; gid < SHAPED_COUNT; ++gid) {
    utf8AppendCodepoint(shaping::glyphToken(gid), shapedStream);
  }

  font.beginPrewarmScope();
  font.noteScannedStyle(0);
  font.noteScannedStyle(1);
  ASSERT_EQ(0, font.prewarm(shapedStream.c_str(), 0x03, /*metadataOnly=*/false, /*loadKernLig=*/false,
                            /*accumulate=*/false));
  EXPECT_TRUE(epdReg->data->is4Bit);
  EXPECT_NE(nullptr, epdReg->data->shapeHandler);
  EXPECT_EQ(&SdCardFont::resolveGlyphBitmap, epdReg->data->bitmapHandler);

  for (uint32_t gid = 0; gid < SHAPED_COUNT; ++gid) {
    const EpdGlyph* g = epdReg->getGlyph(shaping::glyphToken(gid));
    ASSERT_NE(nullptr, g);
    EXPECT_EQ(WIDTH, g->width);
    EXPECT_EQ(HEIGHT, g->height);
    EXPECT_EQ(PACKED_BYTES, g->dataLength);
    const uint8_t* bmp = epdReg->data->bitmapHandler(epdReg->data->glyphMissCtx, g);
    ASSERT_NE(nullptr, bmp);
    const uint32_t glyphIdx = 2 + gid;
    for (uint16_t b = 0; b < PACKED_BYTES; ++b) {
      EXPECT_EQ(static_cast<uint8_t>((glyphIdx * 23u + b + 5u) & 0xFFu), bmp[b]);
    }
  }

  // Evicting inactive Bold via compactForRender keeps both stub handlers alive.
  font.beginPrewarmScope();
  font.noteScannedStyle(0);
  font.compactForRender(0x01, /*dropLayoutAdvanceCache=*/true);
  EXPECT_EQ(stubBold, epdBold->data);
  EXPECT_NE(nullptr, epdBold->data->shapeHandler);
  EXPECT_EQ(&SdCardFont::resolveGlyphBitmap, epdBold->data->bitmapHandler);
  EXPECT_NE(nullptr, epdReg->data->shapeHandler);
  font.clearCache();
}

TEST(SdCardFontTest, MultiStringFourBitPrewarmUnderTightHeapUsesZeroSlabSpillAndReusesExistingSlabs) {
  // 24 x 24 = 288 B/glyph -> 28 glyphs pack into 1 slab (8,192 B), 99 glyphs + U+FFFD total.
  constexpr uint16_t PACKED_BYTES = 288;
  make4BitFont(100, 24, 24);

  struct TightHeapGuard {
    ~TightHeapGuard() {
      trackSlabHeap = false;
      ESP.freeHeap = 200 * 1024;
    }
  } guard;
  trackSlabHeap = true;

  auto rowGetter = [](const void* ctx, uint32_t idx) -> const char* {
    const auto* rows = static_cast<const std::vector<std::string>*>(ctx);
    return idx < rows->size() ? (*rows)[idx].c_str() : nullptr;
  };

  // (1) Cold multi-string prewarm under <= 50 KB free heap must enter 0-slab spill mode
  //     instead of returning -1, and subsequent per-row prewarms must incur 0 SD reads.
  {
    SdCardFont font;
    ASSERT_TRUE(font.load("fixture"));
    const EpdFontData* const stubData = font.getEpdFont()->data;

    ESP.freeHeap = 48 * 1024;  // <= 50 KB internal-DRAM slab floor
    std::vector<std::string> rows;
    for (uint32_t r = 0; r < 12; ++r) {
      rows.push_back(page(FIRST + r * 4, 4));
    }

    const int missed = font.prewarm(rowGetter, &rows, static_cast<uint32_t>(rows.size()), 0x01,
                                    /*metadataOnly=*/false, /*loadKernLig=*/false, /*accumulate=*/true);
    EXPECT_EQ(49, missed);  // 48 unique glyphs + U+FFFD all resident in miniGlyphs, 0 slabs allocated
    EXPECT_EQ(48u * 1024u, ESP.freeHeap);
    EXPECT_EQ(0u, font.getStats().bitmapBytes);
    EXPECT_NE(stubData, font.getEpdFont()->data);
    EXPECT_EQ(49u, residentCount(font));

    // Per-row prewarm/draw calls hit resident miniIntervals with 0 SD rebuilds.
    sdFontTestReads = 0;
    sdFontTestOpens = 0;
    const EpdFont* epd = font.getEpdFont();
    for (const auto& row : rows) {
      EXPECT_EQ(5, font.prewarm(row.c_str(), 0x01, /*metadataOnly=*/false, /*loadKernLig=*/false,
                                /*accumulate=*/true));
    }
    EXPECT_EQ(0u, sdFontTestReads);
    EXPECT_EQ(0u, sdFontTestOpens);

    const EpdGlyph* g = epd->getGlyph(FIRST + 23);
    ASSERT_NE(nullptr, g);
    EXPECT_NE(0u, g->dataOffset & SdCardFont::SPILLED_BITMAP_FLAG);
    EXPECT_EQ(0u, sdFontTestReads);
    const uint8_t* bmp = epd->data->bitmapHandler(epd->data->glyphMissCtx, g);
    ASSERT_NE(nullptr, bmp);
    EXPECT_EQ(1u, sdFontTestReads);
    for (uint16_t b = 0; b < PACKED_BYTES; ++b) {
      ASSERT_EQ(static_cast<uint8_t>((23u * 17u + b * 3u + 1u) & 0xFFu), bmp[b]) << b;
    }
  }

  // (2) With Slab 0 already allocated, multi-string prewarm under <= 50 KB reuses Slab 0
  //     (0 new slab allocations) and spills only glyphs past Slab 0 without per-row rebuilds.
  {
    SdCardFont font;
    ASSERT_TRUE(font.load("fixture"));
    ESP.freeHeap = 200 * 1024;
    ASSERT_EQ(0, font.prewarm(page(FIRST, 20).c_str(), 0x01, /*metadataOnly=*/false, /*loadKernLig=*/false,
                              /*accumulate=*/false));
    EXPECT_EQ(200u * 1024u - SdCardFont::MINI_SLAB_SIZE, ESP.freeHeap);

    // 2a) At 45 KB (< 50 KB), unioning 6 more glyphs (27 total <= 28 per slab) fits completely in Slab 0.
    ESP.freeHeap = 45 * 1024;
    font.resetStats();
    std::vector<std::string> smallBatch = {page(FIRST + 20, 2), page(FIRST + 22, 2), page(FIRST + 24, 2)};
    EXPECT_EQ(0, font.prewarm(rowGetter, &smallBatch, static_cast<uint32_t>(smallBatch.size()), 0x01,
                              /*metadataOnly=*/false, /*loadKernLig=*/false, /*accumulate=*/true));
    EXPECT_EQ(45u * 1024u, ESP.freeHeap);  // 0 new slab allocations
    EXPECT_EQ(27u * PACKED_BYTES, font.getStats().bitmapBytes);

    sdFontTestReads = 0;
    sdFontTestOpens = 0;
    const EpdFont* epd = font.getEpdFont();
    for (const auto& row : smallBatch) {
      EXPECT_EQ(0, font.prewarm(row.c_str(), 0x01, /*metadataOnly=*/false, /*loadKernLig=*/false,
                                /*accumulate=*/true));
    }
    for (uint32_t idx = 20; idx < 26; ++idx) {
      const EpdGlyph* g = epd->getGlyph(FIRST + idx);
      ASSERT_NE(nullptr, g) << idx;
      EXPECT_EQ(0u, g->dataOffset & SdCardFont::SPILLED_BITMAP_FLAG) << idx;
      const uint8_t* bmp = epd->data->bitmapHandler(epd->data->glyphMissCtx, g);
      ASSERT_NE(nullptr, bmp) << idx;
      EXPECT_EQ(static_cast<uint8_t>((idx * 17u + 1u) & 0xFFu), bmp[0]) << idx;
    }
    EXPECT_EQ(0u, sdFontTestReads);
    EXPECT_EQ(0u, sdFontTestOpens);

    // 2b) Across clearCache() at 50 KB (retained Slab 0), a larger batch packs 28 into Slab 0 and spills 39.
    ESP.freeHeap = 50 * 1024;
    font.clearCache();
    font.resetStats();
    std::vector<std::string> spillBatch;
    for (uint32_t r = 0; r < 8; ++r) {
      spillBatch.push_back(page(FIRST + 30 + r * 5, 5));
    }
    EXPECT_EQ(39, font.prewarm(rowGetter, &spillBatch, static_cast<uint32_t>(spillBatch.size()), 0x01,
                               /*metadataOnly=*/false, /*loadKernLig=*/false, /*accumulate=*/true));
    EXPECT_EQ(50u * 1024u, ESP.freeHeap);  // Slab 0 reused, Slab 1 blocked by floor
    EXPECT_EQ(28u * PACKED_BYTES, font.getStats().bitmapBytes);
    EXPECT_EQ(67u, residentCount(font));  // 26 prior + 40 batch + U+FFFD

    sdFontTestReads = 0;
    sdFontTestOpens = 0;
    for (const auto& row : spillBatch) {
      EXPECT_GE(font.prewarm(row.c_str(), 0x01, /*metadataOnly=*/false, /*loadKernLig=*/false,
                             /*accumulate=*/true),
                0);
    }
    EXPECT_EQ(0u, sdFontTestReads);
    EXPECT_EQ(0u, sdFontTestOpens);
  }

  // (3) Critically low heap (<= 16 KB headroom) still aborts cleanly with -1.
  {
    SdCardFont font;
    ASSERT_TRUE(font.load("fixture"));
    ESP.freeHeap = 16 * 1024;
    std::vector<std::string> rows = {page(FIRST, 2), page(FIRST + 2, 2)};
    EXPECT_EQ(-1, font.prewarm(rowGetter, &rows, static_cast<uint32_t>(rows.size()), 0x01,
                               /*metadataOnly=*/false, /*loadKernLig=*/false, /*accumulate=*/true));
  }
}
