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
std::vector<uint8_t> buildFont(const uint16_t bitmapBytes, const bool kerning, const bool twoBit = false,
                               const uint8_t side = 8) {
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
  putU16(b, 8, 4);                // version
  putU16(b, 10, twoBit ? 1 : 0);  // bit 0: 2-bit bitmaps
  b[12] = 1;                      // one style

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
    b[at] = side;                         // width
    b[at + 1] = side;                     // height
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

// The reader frees a large page arena once the page is on the panel (EpubReaderActivity): a CJK
// page's 25-31 KB of bitmaps would otherwise stay on the heap for as long as the page is read,
// although the next render rebuilds the arena from the card anyway. pageArenaBytes() is the
// heap the arena holds; releasePageArenaIfAtLeast() frees it only past a threshold, so a small
// Latin arena stays.
TEST(SdFontArena, PageArenaBytesAreTheHeapBitmapBytes) {
  constexpr uint16_t kBitmap = 300;
  const std::string path = writeTemp(buildFont(kBitmap, /*kerning=*/false));
  SdCardFont font;
  ASSERT_TRUE(font.load(path.c_str()));
  EXPECT_EQ(0u, font.pageArenaBytes());
  ASSERT_EQ(0, font.prewarm("ABCD", 0x01));
  EXPECT_EQ(GLYPHS * kBitmap, font.pageArenaBytes()) << "A-D and U+FFFD";
  font.clearCache();
  EXPECT_EQ(0u, font.pageArenaBytes());
}

TEST(SdFontArena, ASmallArenaIsKept) {
  constexpr uint16_t kBitmap = 300;
  const std::string path = writeTemp(buildFont(kBitmap, /*kerning=*/false));
  SdCardFont font;
  ASSERT_TRUE(font.load(path.c_str()));
  ASSERT_EQ(0, font.prewarm("ABCD", 0x01));
  EXPECT_EQ(0u, font.releasePageArenaIfAtLeast(GLYPHS * kBitmap + 1));
  EXPECT_EQ(GLYPHS * kBitmap, font.pageArenaBytes());
}

TEST(SdFontArena, ALargeArenaIsReleasedAndTheNextPrewarmRebuildsIt) {
  constexpr uint16_t kBitmap = 300;
  const std::string path = writeTemp(buildFont(kBitmap, /*kerning=*/false));
  SdCardFont reference;
  ASSERT_TRUE(reference.load(path.c_str()));
  ASSERT_EQ(0, reference.prewarm("ABCD", 0x01));

  SdCardFont font;
  ASSERT_TRUE(font.load(path.c_str()));
  ASSERT_EQ(0, font.prewarm("ABCD", 0x01));
  EXPECT_EQ(GLYPHS * kBitmap, font.releasePageArenaIfAtLeast(GLYPHS * kBitmap));
  EXPECT_EQ(0u, font.pageArenaBytes());
  ASSERT_EQ(0, font.prewarm("ABCD", 0x01));
  expectSameGlyphs(*reference.getEpdFont(0)->data, *font.getEpdFont(0)->data);
}

TEST(SdFontArena, AnArenaModeLoadHoldsNoHeapArena) {
  const std::string path = writeTemp(buildFont(300, /*kerning=*/false));
  BuildArena arena(64 * 1024);
  ASSERT_TRUE(arena.valid());
  SdCardFont font;
  font.useArena(&arena);
  ASSERT_TRUE(font.load(path.c_str()));
  ASSERT_EQ(0, font.prewarm("ABCD", 0x01));
  EXPECT_EQ(0u, font.pageArenaBytes()) << "the bitmaps are in the caller's arena, not on the heap";
  EXPECT_EQ(0u, font.releasePageArenaIfAtLeast(1));
}

// The pre-render draws black and white only, and a 2-bit glyph's black-and-white image is its ink
// mask: the BW pass draws every non-zero level (GfxRenderer's BW draw mask, raw {1,2,3}). An
// ink-only prewarm keeps just that mask, half the bytes, so a dense CJK page's arena fits the
// pre-render's heap. 8x8 2-bit glyphs are 16 bytes; their masks are 8.
namespace {

uint8_t level2Bit(const uint8_t* bitmap, const uint32_t pixel) {
  return (bitmap[pixel / 4] >> (6 - 2 * (pixel % 4))) & 3;
}
bool ink1Bit(const uint8_t* bitmap, const uint32_t pixel) { return ((bitmap[pixel / 8] >> (7 - pixel % 8)) & 1) != 0; }

void expectInkMaskOf(const uint8_t* full, const uint8_t* ink, const uint32_t pixels, const uint32_t g) {
  for (uint32_t p = 0; p < pixels; p++) {
    ASSERT_EQ(level2Bit(full, p) != 0, ink1Bit(ink, p)) << "glyph " << g << " pixel " << p;
  }
}

}  // namespace

TEST(SdFontArena, AnInkOnlyPrewarmKeepsEachGlyphsInkMaskInHalfTheBytes) {
  const std::string path = writeTemp(buildFont(16, /*kerning=*/false, /*twoBit=*/true));
  SdCardFont full;
  ASSERT_TRUE(full.load(path.c_str()));
  ASSERT_EQ(0, full.prewarm("ABCD", 0x01));
  SdCardFont ink;
  ASSERT_TRUE(ink.load(path.c_str()));
  ink.setInkOnlyPrewarm(true);
  ASSERT_EQ(0, ink.prewarm("ABCD", 0x01));

  const EpdFontData& f = *full.getEpdFont(0)->data;
  const EpdFontData& i = *ink.getEpdFont(0)->data;
  EXPECT_TRUE(f.is2Bit);
  EXPECT_FALSE(i.is2Bit) << "the renderer must read the arena as 1-bit";
  EXPECT_EQ(full.pageArenaBytes() / 2, ink.pageArenaBytes());
  ASSERT_EQ(f.intervalCount, i.intervalCount);
  const uint32_t n = glyphCount(f);
  for (uint32_t g = 0; g < n; g++) {
    ASSERT_EQ(glyphDataBytes(f.glyph[g].width, f.glyph[g].height, false), i.glyph[g].dataLength) << "glyph " << g;
    expectInkMaskOf(f.bitmap + f.glyph[g].dataOffset, i.bitmap + i.glyph[g].dataOffset,
                    f.glyph[g].width * f.glyph[g].height, g);
  }
}

TEST(SdFontArena, AnInkOnlyPrewarmOfA1BitFontChangesNothing) {
  const std::string path = writeTemp(buildFont(8, /*kerning=*/false));
  SdCardFont plain;
  ASSERT_TRUE(plain.load(path.c_str()));
  ASSERT_EQ(0, plain.prewarm("ABCD", 0x01));
  SdCardFont ink;
  ASSERT_TRUE(ink.load(path.c_str()));
  ink.setInkOnlyPrewarm(true);
  ASSERT_EQ(0, ink.prewarm("ABCD", 0x01));
  expectSameGlyphs(*plain.getEpdFont(0)->data, *ink.getEpdFont(0)->data);
  EXPECT_FALSE(ink.getEpdFont(0)->data->is2Bit);
}

// A glyph the prewarm did not load comes through the miss ring. Under an ink-only page it has to
// be an ink mask as well, since the page's font data says 1-bit; and a ring entry of one depth
// must never serve a draw of the other.
TEST(SdFontArena, InkOnlyMissesAreInkMasksAndNeverServeAFullDraw) {
  const std::string path = writeTemp(buildFont(16, /*kerning=*/false, /*twoBit=*/true));
  SdCardFont reference;
  ASSERT_TRUE(reference.load(path.c_str()));
  ASSERT_EQ(0, reference.prewarm("ABCD", 0x01));
  const EpdFontData& r = *reference.getEpdFont(0)->data;

  SdCardFont font;
  ASSERT_TRUE(font.load(path.c_str()));
  font.setInkOnlyPrewarm(true);
  ASSERT_EQ(0, font.prewarm("A", 0x01));  // C comes from the ring
  const EpdFontData& inkData = *font.getEpdFont(0)->data;
  ASSERT_FALSE(inkData.is2Bit);
  const EpdGlyph* glyph = inkData.provider->missGlyph('C');
  ASSERT_NE(nullptr, glyph);
  const uint8_t* bitmap = nullptr;
  ASSERT_TRUE(inkData.provider->ownsBitmap(glyph, bitmap));
  ASSERT_NE(nullptr, bitmap);
  EXPECT_EQ(8u, glyph->dataLength);
  expectInkMaskOf(r.bitmap + r.glyph[2].dataOffset, bitmap, 64, 2);

  // The next page renders in full: the ring, which still holds C's ink mask, must not hand it over.
  font.setInkOnlyPrewarm(false);
  ASSERT_EQ(0, font.prewarm("A", 0x01));
  const EpdFontData& fullData = *font.getEpdFont(0)->data;
  ASSERT_TRUE(fullData.is2Bit);
  glyph = fullData.provider->missGlyph('C');
  ASSERT_NE(nullptr, glyph);
  ASSERT_TRUE(fullData.provider->ownsBitmap(glyph, bitmap));
  EXPECT_EQ(16u, glyph->dataLength);
  EXPECT_EQ(0, std::memcmp(r.bitmap + r.glyph[2].dataOffset, bitmap, 16));
}

// The coverage fast path keeps a page's data when it already holds the codepoints. It must not keep
// ink masks for a draw that needs the grey levels, nor 2-bit glyphs a pre-render wants halved.
TEST(SdFontArena, APrewarmOfTheOtherDepthRebuildsTheSameGlyphs) {
  const std::string path = writeTemp(buildFont(16, /*kerning=*/false, /*twoBit=*/true));
  SdCardFont reference;
  ASSERT_TRUE(reference.load(path.c_str()));
  ASSERT_EQ(0, reference.prewarm("ABCD", 0x01));

  SdCardFont font;
  ASSERT_TRUE(font.load(path.c_str()));
  font.setInkOnlyPrewarm(true);
  ASSERT_EQ(0, font.prewarm("ABCD", 0x01));
  font.setInkOnlyPrewarm(false);
  ASSERT_EQ(0, font.prewarm("ABCD", 0x01));
  ASSERT_TRUE(font.getEpdFont(0)->data->is2Bit) << "ink masks served to a 2-bit draw";
  expectSameGlyphs(*reference.getEpdFont(0)->data, *font.getEpdFont(0)->data);

  font.setInkOnlyPrewarm(true);
  ASSERT_EQ(0, font.prewarm("ABCD", 0x01));
  EXPECT_FALSE(font.getEpdFont(0)->data->is2Bit) << "2-bit glyphs kept for an ink-only draw";
  EXPECT_EQ(reference.pageArenaBytes() / 2, font.pageArenaBytes());
}

// A CJK glyph at 14pt is ~180 bytes: more than one 64-byte read chunk, and often an odd byte count,
// whose last byte packs into half a mask byte. 23x23 2-bit is 133 bytes, read as 64 + 64 + 5.
TEST(SdFontArena, AGlyphLongerThanAChunkWithAnOddByteCountPacksWhole) {
  constexpr uint8_t kSide = 23;
  constexpr uint16_t kBytes = (kSide * kSide + 3) / 4;
  static_assert(kBytes == 133, "the case this test is about");
  const std::string path = writeTemp(buildFont(kBytes, /*kerning=*/false, /*twoBit=*/true, kSide));
  SdCardFont full;
  ASSERT_TRUE(full.load(path.c_str()));
  ASSERT_EQ(0, full.prewarm("AB", 0x01));
  SdCardFont ink;
  ASSERT_TRUE(ink.load(path.c_str()));
  ink.setInkOnlyPrewarm(true);
  ASSERT_EQ(0, ink.prewarm("AB", 0x01));
  const EpdFontData& f = *full.getEpdFont(0)->data;
  const EpdFontData& i = *ink.getEpdFont(0)->data;
  const uint32_t n = glyphCount(f);
  for (uint32_t g = 0; g < n; g++) {
    if (f.glyph[g].dataLength == 0) continue;
    ASSERT_EQ(glyphDataBytes(kSide, kSide, false), i.glyph[g].dataLength) << "glyph " << g;
    expectInkMaskOf(f.bitmap + f.glyph[g].dataOffset, i.bitmap + i.glyph[g].dataOffset, kSide * kSide, g);
  }
}
