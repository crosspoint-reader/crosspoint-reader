#include <FontPsram.h>
#include <HalStorage.h>
#include <Logging.h>
#include <SdCardFont.h>
#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>

namespace {
size_t failNextArraySize = 0;
bool trackArrayAllocations = false;
size_t arrayAllocations = 0;
size_t arrayBytes = 0;
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
  if (trackArrayAllocations) {
    ++arrayAllocations;
    arrayBytes += size;
  }
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

void put16(size_t at, uint16_t value) {
  sdFontTestFile[at] = value;
  sdFontTestFile[at + 1] = value >> 8;
}
void put32(size_t at, uint32_t value) {
  put16(at, value);
  put16(at + 2, value >> 16);
}

void makeFont(uint8_t styleCount = 1, bool varyingAdvances = false) {
  constexpr size_t GLYPH_OFFSET = 64 + 24;
  constexpr size_t BITMAP_OFFSET = GLYPH_OFFSET + GLYPHS * sizeof(EpdGlyph);
  sdFontTestFile.assign(BITMAP_OFFSET + GLYPHS * BITMAP_BYTES, 0);
  std::memcpy(sdFontTestFile.data(), "CPFONT\0\0", 8);
  put16(8, CPFONT_VERSION);
  sdFontTestFile[12] = 1;
  put32(36, 2);
  put32(40, GLYPHS);
  sdFontTestFile[44] = 32;
  put16(45, 32);
  put32(56, 64);
  put32(64, FIRST);
  put32(68, FIRST + GLYPHS - 2);
  put32(76, 0xFFFD);
  put32(80, 0xFFFD);
  put32(84, GLYPHS - 1);
  for (uint32_t i = 0; i < GLYPHS; ++i) {
    EpdGlyph glyph{};
    glyph.width = 32;
    glyph.height = 32;
    glyph.advanceX = (32 + (varyingAdvances ? i % 23 : 0)) << 4;
    glyph.top = 32;
    glyph.dataLength = BITMAP_BYTES;
    // Store bitmaps in reverse glyph order to exercise sorted reads on rebuild.
    glyph.dataOffset = (GLYPHS - 1 - i) * BITMAP_BYTES;
    std::memcpy(sdFontTestFile.data() + GLYPH_OFFSET + i * sizeof(glyph), &glyph, sizeof(glyph));
    std::memset(sdFontTestFile.data() + BITMAP_OFFSET + glyph.dataOffset, i % 251, BITMAP_BYTES);
  }
  if (styleCount > 1) {
    const size_t extraTocBytes = (styleCount - 1) * 32;
    sdFontTestFile.insert(sdFontTestFile.begin() + 64, extraTocBytes, 0);
    sdFontTestFile[12] = styleCount;
    put32(56, 64 + extraTocBytes);
    for (uint8_t si = 1; si < styleCount; ++si) {
      std::memcpy(sdFontTestFile.data() + 32 + si * 32, sdFontTestFile.data() + 32, 32);
      sdFontTestFile[32 + si * 32] = si;
    }
  }
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

uint32_t residentCount(SdCardFont& font) {
  const auto* data = font.getEpdFont()->data;
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

TEST(SdCardFontTest, UniformRangesServeUnwarmedGlyphsAndSurviveSectionCacheRelease) {
  makeFont(2);
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  ASSERT_TRUE(font.prepareAdvances());
  sdFontTestReads = 0;
  for (uint8_t style : {0, 1}) {
    for (uint32_t cp = FIRST; cp < FIRST + GLYPHS - 1; ++cp) EXPECT_EQ(32 << 4, font.getAdvance(cp, style));
  }
  EXPECT_EQ(0U, sdFontTestReads);
  font.releaseResidentCaches();  // Even a full cache release preserves verified font metadata.
  EXPECT_TRUE(font.hasAdvanceTable());
  EXPECT_EQ(32 << 4, font.getAdvance(FIRST + 400, 0));
  EXPECT_EQ(0U, sdFontTestReads);
}

TEST(SdCardFontTest, UniformMetricsAndSharedExceptionsFitWithin800PreparationBytes) {
  makeFont(2);
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  arrayAllocations = arrayBytes = 0;
  trackArrayAllocations = true;
  const bool prepared = font.prepareAdvances();
  trackArrayAllocations = false;
  ASSERT_TRUE(prepared);
  EXPECT_LE(arrayBytes, 800U);
  EXPECT_LE(arrayAllocations, 3U);
}

TEST(SdCardFontTest, LayoutWarmupAndLookupAllocateNoArrays) {
  makeFont(1, true);
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  ASSERT_TRUE(font.prepareAdvances());
  const auto text = page(FIRST, 400);
  arrayAllocations = 0;
  trackArrayAllocations = true;
  const int result = font.buildAdvanceTable(text.c_str(), 1);
  for (uint32_t i = 0; i < 400; ++i) EXPECT_EQ((32 + i % 23) << 4, font.getAdvance(FIRST + i, 0));
  trackArrayAllocations = false;
  EXPECT_EQ(0, result);
  EXPECT_EQ(0U, arrayAllocations);
}

TEST(SdCardFontTest, OneExceptionalWidthPreventsAUniformRangeClaim) {
  makeFont();
  EpdGlyph glyph{};
  constexpr size_t AT = 88 + 400 * sizeof(EpdGlyph);
  std::memcpy(&glyph, sdFontTestFile.data() + AT, sizeof(glyph));
  glyph.advanceX = 511;
  std::memcpy(sdFontTestFile.data() + AT, &glyph, sizeof(glyph));
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  ASSERT_TRUE(font.prepareAdvances());
  for (uint32_t i : {0U, 399U, 400U, 401U, 511U}) EXPECT_EQ(i == 400 ? 511 : 512, font.getAdvance(FIRST + i, 0));
}

TEST(SdCardFontTest, CacheEvictionKeepsExactAdvanceLookupWithoutReallocation) {
  makeFont(1, true);
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  ASSERT_TRUE(font.prepareAdvances());
  ASSERT_EQ(0, font.buildAdvanceTable(page(FIRST, 40).c_str(), 1));
  font.clearPersistentCache();
  arrayAllocations = 0;
  trackArrayAllocations = true;
  EXPECT_TRUE(font.hasAdvanceTable());
  for (uint32_t i : {0U, 200U, 399U}) EXPECT_EQ((32 + i % 23) << 4, font.getAdvance(FIRST + i, 0));
  trackArrayAllocations = false;
  EXPECT_EQ(0U, arrayAllocations);
}

TEST(SdCardFontTest, ReservationOomKeepsVerifiedRangesAndDirectMetricFallback) {
  makeFont();
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  failNextArraySize = 512;
  const bool prepared = font.prepareAdvances();
  const auto pendingFailure = failNextArraySize;
  failNextArraySize = 0;
  EXPECT_FALSE(prepared);
  EXPECT_EQ(0U, pendingFailure);
  EXPECT_TRUE(font.hasAdvanceTable());
  EXPECT_EQ(32 << 4, font.getAdvance(FIRST + 500, 0));
  EXPECT_EQ(32 << 4, font.getAdvance(0x1234, 0));  // replacement glyph
}

TEST(SdCardFontTest, BitmapRetryDropsExceptionsButKeepsUniformMetrics) {
  makeFont();
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  ASSERT_TRUE(font.prepareAdvances());
  ASSERT_EQ(0, font.buildAdvanceTable("?", 1));  // warm replacement in the exception cache
  const auto text = page(FIRST, 200);
  ASSERT_EQ(0, font.prewarm(page(FIRST, 50).c_str(), 1, false, false, false));
  ESP.largestBlock = 8 * 1024;
  failNextArraySize = 201 * BITMAP_BYTES;
  const int result = font.prewarm(text.c_str(), 1, false, false, false);
  ESP.largestBlock = 200 * 1024;
  const auto pendingFailure = failNextArraySize;
  failNextArraySize = 0;
  ASSERT_EQ(0, result);
  EXPECT_EQ(0U, pendingFailure);
  expectPageBitmaps(font, FIRST, 200);
  sdFontTestReads = 0;
  EXPECT_TRUE(font.hasAdvanceTable());
  EXPECT_EQ(32 << 4, font.getAdvance(FIRST + 300, 0));
  EXPECT_EQ(0U, sdFontTestReads);
  EXPECT_EQ(32 << 4, font.getAdvance('?', 0));
  EXPECT_EQ(1U, sdFontTestReads);  // exceptions were evicted by the bitmap retry
  sdFontTestReads = 0;
  arrayAllocations = 0;
  trackArrayAllocations = true;
  const int warmed = font.buildAdvanceTable("?", 1);
  trackArrayAllocations = false;
  EXPECT_EQ(0, warmed);
  EXPECT_EQ(0U, arrayAllocations);
  EXPECT_EQ(32 << 4, font.getAdvance('?', 0));
  EXPECT_EQ(1U, sdFontTestReads);  // layout does not recreate the evicted cache
}

TEST(SdCardFontTest, SectionReleasePreservesWarmedExceptions) {
  makeFont(1, true);
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  ASSERT_TRUE(font.prepareAdvances());
  const auto text = page(FIRST, 80);
  ASSERT_EQ(0, font.buildAdvanceTable(text.c_str(), 1));
  font.releaseResidentCaches(/*preserveAdvances=*/true);
  sdFontTestReads = 0;
  for (uint32_t i = 0; i < 80; ++i) EXPECT_EQ((32 + i % 23) << 4, font.getAdvance(FIRST + i, 0));
  EXPECT_EQ(0U, sdFontTestReads);
}

TEST(SdCardFontTest, PreparationIsIdempotentAndFontReloadInvalidatesMetrics) {
  makeFont();
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  ASSERT_TRUE(font.prepareAdvances());
  sdFontTestReads = arrayAllocations = 0;
  trackArrayAllocations = true;
  const bool prepared = font.prepareAdvances();
  trackArrayAllocations = false;
  EXPECT_TRUE(prepared);
  EXPECT_EQ(0U, arrayAllocations);
  EXPECT_EQ(0U, sdFontTestReads);
  makeFont(1, true);
  ASSERT_TRUE(font.load("different fixture"));
  EXPECT_FALSE(font.hasAdvanceTable());
  ASSERT_TRUE(font.prepareAdvances());
  EXPECT_EQ(33 << 4, font.getAdvance(FIRST + 1, 0));
}

TEST(SdCardFontTest, ZeroAndMaximumAdvancesAreLossless) {
  for (const uint16_t width : {0, 65535}) {
    makeFont();
    for (uint32_t i = 0; i < GLYPHS; ++i) put16(88 + i * sizeof(EpdGlyph) + 2, width);
    SdCardFont font;
    ASSERT_TRUE(font.load("fixture"));
    ASSERT_TRUE(font.prepareAdvances());
    ASSERT_EQ(0, font.buildAdvanceTable("unknown", 1));
    sdFontTestReads = 0;
    EXPECT_EQ(width, font.getAdvance(FIRST + 400, 0));
    EXPECT_EQ(width, font.getAdvance('u', 0));
    EXPECT_EQ(width > 16383 ? 1U : 0U, sdFontTestReads);  // wide exceptions read the exact value from SD
  }
}

TEST(SdCardFontTest, PackedWordsExtraTextAndFallbackStylesWarmTheActualGlyphs) {
  makeFont(1, true);
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  ASSERT_TRUE(font.prepareAdvances());
  const auto a = page(FIRST, 10) + '\0' + page(FIRST + 10, 10);
  const auto b = page(FIRST + 20, 10);
  const char* segments[] = {a.c_str(), b.c_str()};
  const size_t lengths[] = {a.size(), b.size()};
  const auto extra = page(FIRST + 30, 10);
  ASSERT_EQ(0, font.buildAdvanceTablePacked(segments, lengths, 2, true, true, 0x0E, extra.c_str()));
  EXPECT_EQ(0, font.resolveStyle(3));
  sdFontTestReads = 0;
  for (uint32_t i = 0; i < 40; ++i) EXPECT_EQ((32 + i % 23) << 4, font.getAdvance(FIRST + i, 0));
  EXPECT_EQ((32 + 512 % 23) << 4, font.getAdvance(' ', 0));
  EXPECT_EQ((32 + 512 % 23) << 4, font.getAdvance('-', 0));
  EXPECT_EQ(0U, sdFontTestReads);
}

TEST(SdCardFontTest, SupplementaryCodepointsAndLastUint16GlyphIndexAreLossless) {
  constexpr uint32_t start = 0x10000;
  constexpr uint32_t count = 65536;
  makeFont();
  put32(36, 1);
  put32(40, count);
  put32(64, start);
  put32(68, start + count - 1);
  sdFontTestFile.resize(76 + count * sizeof(EpdGlyph));
  for (uint32_t i = 0; i < count; ++i) {
    EpdGlyph glyph{};
    glyph.advanceX = i == 65535 ? 16383 : i;
    std::memcpy(sdFontTestFile.data() + 76 + i * sizeof(glyph), &glyph, sizeof(glyph));
  }
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  ASSERT_TRUE(font.prepareAdvances());
  ASSERT_EQ(0, font.buildAdvanceTable("\xF0\x9F\xBF\xBF", 1));  // U+1FFFF, glyph index 65535
  sdFontTestReads = 0;
  EXPECT_EQ(16383, font.getAdvance(0x1FFFF, 0));
  EXPECT_EQ(0U, sdFontTestReads);
  EXPECT_EQ(0, font.getAdvance(start, 0));
  EXPECT_EQ(0, font.getAdvance('?', 0));  // no replacement glyph
}

TEST(SdCardFontTest, TruncatedUniformScanDoesNotPublishUnverifiedMetrics) {
  makeFont();
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  sdFontTestFile.resize(88 + 400 * sizeof(EpdGlyph));
  EXPECT_FALSE(font.prepareAdvances());
  EXPECT_EQ(512, font.getAdvance(FIRST + 399, 0));
  EXPECT_EQ(0, font.getAdvance(FIRST + 400, 0));
}

TEST(SdCardFontTest, FontFirstEncounteredDuringLayoutUsesMetricsWithoutArrayAllocations) {
  makeFont(1, true);
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  const auto text = page(FIRST, 400);
  arrayAllocations = 0;
  trackArrayAllocations = true;
  const int result = font.buildAdvanceTable(text.c_str(), 1);
  EXPECT_TRUE(font.hasAdvanceTable());
  EXPECT_EQ(33 << 4, font.getAdvance(FIRST + 1, 0));
  trackArrayAllocations = false;
  EXPECT_EQ(0, result);
  EXPECT_EQ(0U, arrayAllocations);
}

TEST(SdCardFontTest, UniformMetadataOomFallsBackExactlyAndCanRetryAtReaderOpen) {
  makeFont();
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  failNextArraySize = 4;
  const bool prepared = font.prepareAdvances();
  const auto pendingFailure = failNextArraySize;
  failNextArraySize = 0;
  EXPECT_FALSE(prepared);
  EXPECT_EQ(0U, pendingFailure);
  EXPECT_EQ(512, font.getAdvance(FIRST + 400, 0));
  ASSERT_TRUE(font.prepareAdvances());
  sdFontTestReads = 0;
  EXPECT_EQ(512, font.getAdvance(FIRST + 400, 0));
  EXPECT_EQ(0U, sdFontTestReads);
}

TEST(SdCardFontTest, EvictionReportsStyleOccupancyAndFullCacheReadsWithoutLosingPeaks) {
  makeFont(2, true);
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  ASSERT_TRUE(font.prepareAdvances());
  ASSERT_EQ(0, font.buildAdvanceTable(page(FIRST, 400).c_str(), 1));
  ASSERT_EQ(0, font.buildAdvanceTable(page(FIRST, 8).c_str(), 2));
  sdFontTestReads = 0;
  EXPECT_EQ(512, font.getAdvance(FIRST, 0));
  EXPECT_EQ(512, font.getAdvance(FIRST, 1));
  for (uint32_t i = 248; i < 260; ++i) EXPECT_EQ((32 + i % 23) << 4, font.getAdvance(FIRST + i, 0));
  EXPECT_EQ(12U, sdFontTestReads);

  sdFontTestLogs.clear();
  sdFontTestCaptureLogs = true;
  font.clearPersistentCache();
  sdFontTestCaptureLogs = false;
  EXPECT_NE(std::string::npos, sdFontTestLogs.find("style=0 entries=248 peak=256 direct_reads=12 full_misses=12"));
  EXPECT_NE(std::string::npos, sdFontTestLogs.find("style=1 entries=8 peak=8 direct_reads=0 full_misses=0"));

  EXPECT_EQ(512, font.getAdvance(FIRST, 0));
  EXPECT_EQ(13U, sdFontTestReads);
  sdFontTestLogs.clear();
  sdFontTestCaptureLogs = true;
  font.clearPersistentCache();
  sdFontTestCaptureLogs = false;
  EXPECT_NE(std::string::npos, sdFontTestLogs.find("style=0 entries=0 peak=256 direct_reads=13 full_misses=12"));
}

TEST(SdCardFontTest, ReloadResetsAdvanceDiagnosticsAndUnpreparedFontsReportDirectReads) {
  makeFont(1, true);
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  ASSERT_TRUE(font.prepareAdvances());
  ASSERT_EQ(0, font.buildAdvanceTable(page(FIRST, 10).c_str(), 1));
  EXPECT_EQ((32 + 50 % 23) << 4, font.getAdvance(FIRST + 50, 0));
  ASSERT_TRUE(font.load("different fixture"));
  ASSERT_EQ(0, font.buildAdvanceTable(page(FIRST, 10).c_str(), 1));
  EXPECT_EQ(528, font.getAdvance(FIRST + 1, 0));

  sdFontTestLogs.clear();
  sdFontTestCaptureLogs = true;
  font.clearPersistentCache();
  sdFontTestCaptureLogs = false;
  EXPECT_NE(std::string::npos, sdFontTestLogs.find("style=0 entries=0 peak=0 direct_reads=1 full_misses=0"));
  EXPECT_NE(std::string::npos, sdFontTestLogs.find("reserved=0 uniform=0 scanned=0"));
}

TEST(SdCardFontTest, SharedPoolKeepsSparseStylesWarmAfterADenseStyleFillsIt) {
  makeFont(2);
  // Three quarters of each style are uniform; the remaining 256 widths vary.
  for (uint8_t si = 0; si < 2; ++si) put32(40 + si * 32, 1024);
  put32(68 + 32, FIRST + 767);
  put32(76 + 32, FIRST + 768);
  put32(80 + 32, FIRST + 1023);
  put32(84 + 32, 768);
  for (uint32_t i = 0; i < 1024; ++i) put16(120 + i * sizeof(EpdGlyph) + 2, i < 768 ? 512 : 512 + i);
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  ASSERT_TRUE(font.prepareAdvances());
  ASSERT_EQ(0, font.buildAdvanceTable(page(FIRST + 768, 160).c_str(), 1));
  ASSERT_EQ(0, font.buildAdvanceTable(page(FIRST + 768, 8).c_str(), 2));
  sdFontTestReads = 0;
  for (uint32_t i = 768; i < 776; ++i) EXPECT_EQ(512 + i, font.getAdvance(FIRST + i, 1));
  EXPECT_EQ(0U, sdFontTestReads);
  // The dense style cannot reclaim the later style's share; skip unusable warmup I/O.
  sdFontTestReads = 0;
  ASSERT_EQ(0, font.buildAdvanceTable(page(FIRST + 768, 160).c_str(), 1));
  EXPECT_EQ(0U, sdFontTestReads);
}

TEST(SdCardFontTest, FourVaryingStylesKeepNinetyDistinctMetricsEach) {
  makeFont(4, true);
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  ASSERT_TRUE(font.prepareAdvances());
  ASSERT_EQ(0, font.buildAdvanceTable(page(FIRST, 90).c_str(), 15));
  sdFontTestReads = 0;
  for (uint8_t si = 0; si < 4; ++si) {
    for (uint32_t i = 0; i < 90; ++i) EXPECT_EQ((32 + i % 23) << 4, font.getAdvance(FIRST + i, si));
  }
  EXPECT_EQ(0U, sdFontTestReads);
}

TEST(SdCardFontTest, PackedExceptionBoundariesDoNotAliasStylesOrTruncateWidths) {
  makeFont(4, true);
  const size_t payload = 160;
  const std::vector<uint8_t> original(sdFontTestFile.begin() + payload, sdFontTestFile.end());
  for (uint8_t si = 1; si < 4; ++si) {
    const auto offset = sdFontTestFile.size();
    sdFontTestFile.insert(sdFontTestFile.end(), original.begin(), original.end());
    put32(56 + si * 32, offset);
  }
  const uint16_t widths[] = {0, 16383, 16384, 65535};
  for (uint8_t si = 0; si < 4; ++si) {
    const size_t offset = payload + si * original.size();
    put16(offset + 24 + 2, widths[si]);
  }
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  ASSERT_TRUE(font.prepareAdvances());
  ASSERT_EQ(0, font.buildAdvanceTable(page(FIRST, 1).c_str(), 15));
  for (uint8_t si = 0; si < 4; ++si) {
    sdFontTestReads = 0;
    EXPECT_EQ(widths[si], font.getAdvance(FIRST, si));
    EXPECT_EQ(si < 2 ? 0U : 1U, sdFontTestReads);
  }
}

TEST(SdCardFontTest, UniformScanScratchOomLeavesExactFallbackAndCanRetry) {
  makeFont();
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  failNextArraySize = 256;
  const bool prepared = font.prepareAdvances();
  const auto pendingFailure = failNextArraySize;
  failNextArraySize = 0;
  EXPECT_FALSE(prepared);
  EXPECT_EQ(0U, pendingFailure);
  EXPECT_EQ(512, font.getAdvance(FIRST + 400, 0));
  ASSERT_TRUE(font.prepareAdvances());
  sdFontTestReads = 0;
  EXPECT_EQ(512, font.getAdvance(FIRST + 400, 0));
  EXPECT_EQ(0U, sdFontTestReads);
}

TEST(SdCardFontTest, SmallUniformIntervalDoesNotShrinkFourStyleLatinBudget) {
  makeFont(4, true);
  put32(160 + 4, FIRST + 15);
  put32(160 + 12, FIRST + 16);
  put32(160 + 16, FIRST + 511);
  put32(160 + 20, 16);
  for (uint32_t i = 0; i < 16; ++i) put16(184 + i * sizeof(EpdGlyph) + 2, 512);
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  ASSERT_TRUE(font.prepareAdvances());
  ASSERT_EQ(0, font.buildAdvanceTable(page(FIRST, 90).c_str(), 15));
  sdFontTestReads = 0;
  for (uint8_t si = 0; si < 4; ++si) {
    for (uint32_t i = 0; i < 90; ++i) EXPECT_EQ(i < 16 ? 512 : (32 + i % 23) << 4, font.getAdvance(FIRST + i, si));
  }
  EXPECT_EQ(0U, sdFontTestReads);
}

TEST(SdCardFontTest, BusyStyleBorrowsOtherwiseUnusedVaryingStyleCapacity) {
  makeFont(4, true);
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  ASSERT_TRUE(font.prepareAdvances());
  ASSERT_EQ(0, font.buildAdvanceTable(page(FIRST, 300).c_str(), 1));
  ASSERT_EQ(0, font.buildAdvanceTable(page(FIRST, 8).c_str(), 2));
  sdFontTestReads = 0;
  for (uint32_t i = 0; i < 300; ++i) EXPECT_EQ((32 + i % 23) << 4, font.getAdvance(FIRST + i, 0));
  for (uint32_t i = 0; i < 8; ++i) EXPECT_EQ((32 + i % 23) << 4, font.getAdvance(FIRST + i, 1));
  EXPECT_EQ(0U, sdFontTestReads);
}
