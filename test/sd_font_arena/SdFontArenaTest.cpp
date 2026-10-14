// SdCardFont::useArena(): every array a font allocates comes from a caller's BuildArena, so the font
// selector can load preview fonts into the lent secondary framebuffer instead of the heap. These
// tests pin the contract: the same glyphs, bitmaps and kerning as a heap load; no array allocation
// on the heap; nothing freed into the arena, so rewinding it after the font is gone is clean; a
// prewarm that does not fit whole still loads a prefix sized from the arena's room; and an arena too
// small for the load fails cleanly.
#include <Arduino.h>
#include <BuildArena.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <new>
#include <string>
#include <vector>

#include "EpdFontData.h"
#include "SdCardFont.h"

namespace {

bool countingArrays = false;
int arrayAllocs = 0;

}  // namespace

void* operator new[](const size_t size) {
  if (countingArrays) ++arrayAllocs;
  if (void* p = std::malloc(size ? size : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](const size_t size, const std::nothrow_t&) noexcept {
  if (countingArrays) ++arrayAllocs;
  return std::malloc(size ? size : 1);
}
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }

namespace {

constexpr uint32_t HEADER = 32;
constexpr uint32_t TOC_ENTRY = 32;
constexpr uint32_t FIRST_CP = 'A';
constexpr uint32_t LETTERS = 4;           // A..D
constexpr uint32_t GLYPHS = LETTERS + 1;  // + U+FFFD, which prewarm() always resolves
constexpr uint32_t INTERVALS = 2;

void putU16(std::vector<uint8_t>& b, const size_t at, const uint16_t v) {
  b[at] = v & 0xFF;
  b[at + 1] = v >> 8;
}
void putU32(std::vector<uint8_t>& b, const size_t at, const uint32_t v) {
  for (int i = 0; i < 4; i++) b[at + i] = (v >> (8 * i)) & 0xFF;
}

// A one-style v4 .cpfont: glyphs A-D and U+FFFD, each with `bitmapBytes` of a distinct byte pattern.
// With `kerning`, A and B are left classes 1 and 2, B and C right classes 1 and 2, and the 2x2
// matrix holds {-3, 1, 2, -1}, so prewarm builds a mini kern matrix through the SD chunk path.
std::vector<uint8_t> buildFont(const uint16_t bitmapBytes, const bool kerning) {
  const uint16_t kernLeft = kerning ? 2 : 0;
  const uint16_t kernRight = kerning ? 2 : 0;
  const uint8_t leftClasses = kerning ? 2 : 0;
  const uint8_t rightClasses = kerning ? 2 : 0;
  const uint32_t dataStart = HEADER + TOC_ENTRY;
  const uint32_t intervalsAt = dataStart;
  const uint32_t glyphsAt = intervalsAt + INTERVALS * sizeof(EpdUnicodeInterval);
  const uint32_t kernLeftAt = glyphsAt + GLYPHS * sizeof(EpdGlyph);
  const uint32_t kernRightAt = kernLeftAt + kernLeft * sizeof(EpdKernClassEntry);
  const uint32_t matrixAt = kernRightAt + kernRight * sizeof(EpdKernClassEntry);
  const uint32_t bitmapsAt = matrixAt + leftClasses * rightClasses;  // no ligatures
  std::vector<uint8_t> b(bitmapsAt + GLYPHS * bitmapBytes, 0);

  const char magic[8] = {'C', 'P', 'F', 'O', 'N', 'T', '\0', '\0'};
  std::copy(magic, magic + 8, b.begin());
  putU16(b, 8, 4);   // version
  putU16(b, 10, 0);  // 1-bit
  b[12] = 1;         // one style

  const size_t toc = HEADER;
  b[toc] = 0;  // style id
  putU32(b, toc + 4, INTERVALS);
  putU32(b, toc + 8, GLYPHS);
  b[toc + 12] = 20;                                // advanceY
  putU16(b, toc + 13, 16);                         // ascender
  putU16(b, toc + 15, static_cast<uint16_t>(-4));  // descender
  putU16(b, toc + 17, kernLeft);
  putU16(b, toc + 19, kernRight);
  b[toc + 21] = leftClasses;
  b[toc + 22] = rightClasses;
  putU32(b, toc + 24, dataStart);

  // A-D from glyph 0, U+FFFD at glyph 4.
  putU32(b, intervalsAt + 0, FIRST_CP);
  putU32(b, intervalsAt + 4, FIRST_CP + LETTERS - 1);
  putU32(b, intervalsAt + 8, 0);
  putU32(b, intervalsAt + 12, 0xFFFD);
  putU32(b, intervalsAt + 16, 0xFFFD);
  putU32(b, intervalsAt + 20, LETTERS);

  for (uint32_t g = 0; g < GLYPHS; g++) {
    const size_t at = glyphsAt + g * sizeof(EpdGlyph);
    b[at] = 8;                            // width
    b[at + 1] = 8;                        // height
    putU16(b, at + 2, 10 << 4);           // advanceX, 12.4 fixed point
    putU16(b, at + 6, 8);                 // top
    putU16(b, at + 8, bitmapBytes);       // dataLength
    putU32(b, at + 12, g * bitmapBytes);  // dataOffset
    for (uint32_t k = 0; k < bitmapBytes; k++) b[bitmapsAt + g * bitmapBytes + k] = static_cast<uint8_t>(g * 31 + k);
  }
  if (kerning) {
    putU16(b, kernLeftAt, 'A');
    b[kernLeftAt + 2] = 1;
    putU16(b, kernLeftAt + 3, 'B');
    b[kernLeftAt + 5] = 2;
    putU16(b, kernRightAt, 'B');
    b[kernRightAt + 2] = 1;
    putU16(b, kernRightAt + 3, 'C');
    b[kernRightAt + 5] = 2;
    const int8_t matrix[4] = {-3, 1, 2, -1};
    std::memcpy(&b[matrixAt], matrix, sizeof(matrix));
  }
  return b;
}

// Named after the running test: ctest -j runs each test in its own process.
std::string writeTemp(const std::vector<uint8_t>& bytes) {
  const auto path =
      std::filesystem::temp_directory_path() /
      (std::string("sd_font_arena_") + testing::UnitTest::GetInstance()->current_test_info()->name() + ".cpfont");
  FILE* f = std::fopen(path.string().c_str(), "wb");
  EXPECT_NE(f, nullptr);
  std::fwrite(bytes.data(), 1, bytes.size(), f);
  std::fclose(f);
  return path.string();
}

uint32_t glyphCount(const EpdFontData& d) {
  uint32_t n = 0;
  for (uint32_t i = 0; i < d.intervalCount; i++) n += d.intervals[i].last - d.intervals[i].first + 1;
  return n;
}

void expectSameGlyphs(const EpdFontData& heap, const EpdFontData& arena) {
  ASSERT_EQ(heap.intervalCount, arena.intervalCount);
  ASSERT_EQ(0, std::memcmp(heap.intervals, arena.intervals, heap.intervalCount * sizeof(EpdUnicodeInterval)));
  const uint32_t n = glyphCount(heap);
  ASSERT_EQ(0, std::memcmp(heap.glyph, arena.glyph, n * sizeof(EpdGlyph)));
  for (uint32_t g = 0; g < n; g++) {
    ASSERT_EQ(0, std::memcmp(heap.bitmap + heap.glyph[g].dataOffset, arena.bitmap + arena.glyph[g].dataOffset,
                             heap.glyph[g].dataLength))
        << "glyph " << g;
  }
}

void expectSameKerning(const EpdFontData& heap, const EpdFontData& arena) {
  ASSERT_NE(nullptr, heap.kernMatrix) << "the font's kerning was not exercised";
  ASSERT_EQ(heap.kernLeftEntryCount, arena.kernLeftEntryCount);
  ASSERT_EQ(heap.kernRightEntryCount, arena.kernRightEntryCount);
  ASSERT_EQ(heap.kernLeftClassCount, arena.kernLeftClassCount);
  ASSERT_EQ(heap.kernRightClassCount, arena.kernRightClassCount);
  EXPECT_EQ(
      0, std::memcmp(heap.kernLeftClasses, arena.kernLeftClasses, heap.kernLeftEntryCount * sizeof(EpdKernClassEntry)));
  EXPECT_EQ(0, std::memcmp(heap.kernRightClasses, arena.kernRightClasses,
                           heap.kernRightEntryCount * sizeof(EpdKernClassEntry)));
  EXPECT_EQ(0, std::memcmp(heap.kernMatrix, arena.kernMatrix, heap.kernLeftClassCount * heap.kernRightClassCount));
}

class CountingArrays {
 public:
  CountingArrays() {
    arrayAllocs = 0;
    countingArrays = true;
  }
  ~CountingArrays() { countingArrays = false; }
};

TEST(SdFontArena, ArenaLoadMatchesHeapLoad) {
  const std::string path = writeTemp(buildFont(64, /*kerning=*/true));
  SdCardFont heap;
  ASSERT_TRUE(heap.load(path.c_str()));
  ASSERT_EQ(0, heap.prewarm("ABCD", 0x01, /*metadataOnly=*/false, /*loadKernLigatureData=*/true));

  BuildArena arena(64 * 1024);
  ASSERT_TRUE(arena.valid());
  SdCardFont inArena;
  inArena.useArena(&arena);
  ASSERT_TRUE(inArena.load(path.c_str()));
  ASSERT_EQ(0, inArena.prewarm("ABCD", 0x01, false, true));
  EXPECT_GT(arena.used(), 0u);

  expectSameGlyphs(*heap.getEpdFont(0)->data, *inArena.getEpdFont(0)->data);
  expectSameKerning(*heap.getEpdFont(0)->data, *inArena.getEpdFont(0)->data);
}

TEST(SdFontArena, ArenaModeAllocatesNoHeapArraysAndRewindsClean) {
  const std::string path = writeTemp(buildFont(64, /*kerning=*/true));
  BuildArena arena(64 * 1024);  // its own buffer is a heap array, so it is made before counting
  ASSERT_TRUE(arena.valid());
  BuildArena::Block block = arena.reserveBlock();
  {
    CountingArrays counting;
    SdCardFont font;
    font.useArena(&arena);
    ASSERT_TRUE(font.load(path.c_str()));
    ASSERT_EQ(0, font.prewarm("ABCD", 0x01, false, true));
    EXPECT_EQ(0, arrayAllocs) << "an array came from the heap in arena mode";
  }  // the font is destroyed here: freeing an arena array must be a no-op, never delete[]
  EXPECT_GT(arena.highWater(), 0u);
  EXPECT_TRUE(arena.release(block));
  EXPECT_EQ(0u, arena.used());
}

TEST(SdFontArena, PrewarmRetrySizesFromTheArenaRoom) {
  // 2000-byte bitmaps: big enough that the retry's 4 KB headroom still leaves room for one glyph,
  // which is what tells arena-based sizing apart from heap-based sizing.
  constexpr uint16_t kBitmap = 2000;
  const std::string path = writeTemp(buildFont(kBitmap, /*kerning=*/false));

  size_t fullUse = 0;  // what a whole prewarm takes in an arena
  {
    BuildArena probe(64 * 1024);
    ASSERT_TRUE(probe.valid());
    SdCardFont font;
    font.useArena(&probe);
    ASSERT_TRUE(font.load(path.c_str()));
    ASSERT_EQ(0, font.prewarm("ABCD", 0x01));
    fullUse = probe.used();
  }

  // Leave 7,500 bytes where the five bitmaps (10,000) go: the first attempt cannot fit, and
  // (7,500 - 4,096) / 2,000 = 1 glyph fits the retry.
  BuildArena arena(fullUse - GLYPHS * kBitmap + 7500);
  ASSERT_TRUE(arena.valid());
  SdCardFont font;
  font.useArena(&arena);
  ASSERT_TRUE(font.load(path.c_str()));
  ESP.setMaxAllocHeap(0);  // the heap has nothing: a heap-sized retry would load no glyph at all
  const int missed = font.prewarm("ABCD", 0x01);
  ESP.setMaxAllocHeap(100 * 1024);
  EXPECT_GT(missed, 0) << "the first attempt was meant not to fit";
  EXPECT_LT(missed, static_cast<int>(GLYPHS)) << "the retry loaded nothing";
}

TEST(SdFontArena, ArenaTooSmallForTheLoadFailsCleanly) {
  const std::string path = writeTemp(buildFont(64, /*kerning=*/true));
  BuildArena arena(16);  // smaller than the 24-byte interval table
  ASSERT_TRUE(arena.valid());
  SdCardFont font;
  font.useArena(&arena);
  EXPECT_FALSE(font.load(path.c_str()));
  EXPECT_GT(arena.failedAllocSize(), 0u);
}

// The glyph-miss ring frees one slot at a time, which a bump arena cannot do: in arena mode every
// eviction would leave its bitmap behind, and a render that thrashes the ring (a prewarm that only
// partly fit) would fill the block and draw blank glyphs into a preview strip that then gets stored.
// So the ring stays on the heap, and a miss still serves the right bitmap with the arena full.
TEST(SdFontArena, GlyphMissesServeFromTheHeapWithTheArenaFull) {
  constexpr uint16_t kBitmap = 64;
  const std::string path = writeTemp(buildFont(kBitmap, /*kerning=*/false));
  BuildArena arena(64 * 1024);
  ASSERT_TRUE(arena.valid());
  SdCardFont font;
  font.useArena(&arena);
  ASSERT_TRUE(font.load(path.c_str()));
  ASSERT_EQ(0, font.prewarm("A", 0x01));  // B-D are left to the miss handler
  ASSERT_NE(nullptr, arena.alloc(arena.capacity() - arena.used(), 1));
  const size_t full = arena.used();

  const EpdFontData& data = *font.getEpdFont(0)->data;
  ASSERT_NE(nullptr, data.provider);
  for (uint32_t g = 1; g < LETTERS; g++) {
    const EpdGlyph* glyph = data.provider->missGlyph(FIRST_CP + g);
    ASSERT_NE(nullptr, glyph) << "miss for glyph " << g << " was refused";
    const uint8_t* bitmap = nullptr;
    ASSERT_TRUE(data.provider->ownsBitmap(glyph, bitmap));
    ASSERT_NE(nullptr, bitmap);
    for (uint32_t k = 0; k < kBitmap; k++) ASSERT_EQ(static_cast<uint8_t>(g * 31 + k), bitmap[k]) << "glyph " << g;
  }
  EXPECT_EQ(full, arena.used());
}

// The renderer asks the provider whose bitmap a glyph is. An array glyph is not the ring's: its
// bitmap is at EpdFontData::bitmap + dataOffset like any other.
TEST(SdFontArena, ProviderOwnsOnlyRingBitmaps) {
  const std::string path = writeTemp(buildFont(64, /*kerning=*/false));
  SdCardFont font;
  ASSERT_TRUE(font.load(path.c_str()));
  ASSERT_EQ(0, font.prewarm("A", 0x01));
  const EpdFont& epd = *font.getEpdFont(0);
  ASSERT_NE(nullptr, epd.data->provider);

  const EpdGlyphRef arrayGlyph = epd.getGlyph('A');
  ASSERT_TRUE(arrayGlyph);
  const uint8_t* bitmap = reinterpret_cast<const uint8_t*>(1);
  EXPECT_FALSE(epd.data->provider->ownsBitmap(arrayGlyph.sdRecord, bitmap));

  const EpdGlyphRef ringGlyph = epd.getGlyph('C');  // not prewarmed: comes from the miss ring
  ASSERT_TRUE(ringGlyph);
  bitmap = nullptr;
  EXPECT_TRUE(epd.data->provider->ownsBitmap(ringGlyph.sdRecord, bitmap));
  EXPECT_NE(nullptr, bitmap);
}

// A glyph with no pixels (a space) is still the ring's: the renderer must get "owned, no bitmap"
// rather than fall through and read EpdFontData::bitmap at an offset that belongs to nothing.
TEST(SdFontArena, ZeroSizeRingGlyphIsOwnedWithNoBitmap) {
  const std::string path = writeTemp(buildFont(0, /*kerning=*/false));
  SdCardFont font;
  ASSERT_TRUE(font.load(path.c_str()));
  const EpdFontData& data = *font.getEpdFont(0)->data;  // the stub: nothing prewarmed
  ASSERT_NE(nullptr, data.provider);

  const EpdGlyph* glyph = data.provider->missGlyph('B');
  ASSERT_NE(nullptr, glyph);
  const uint8_t* bitmap = reinterpret_cast<const uint8_t*>(1);
  EXPECT_TRUE(data.provider->ownsBitmap(glyph, bitmap));
  EXPECT_EQ(nullptr, bitmap);
}

}  // namespace
