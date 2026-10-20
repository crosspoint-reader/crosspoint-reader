// TtfFont: a TrueType family installed into the glyph store, its glyphs rendered once and then read from flash; and
// TtfFontSet, families installed together into one store.
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "BuildArena.h"
#include "EpdFontFamily.h"
#include "GlyphProvider.h"
#include "SimNorFlash.h"
#include "TtfBuilder.h"
#include "TtfFont.h"
#include "TtfFontTestUtil.h"
#include "TtfLayoutBuilder.h"
#include "TtfTestUtil.h"

using namespace ttf_test;

namespace {
// A nothrow array new of exactly this many bytes fails, as the scratch block's does on a heap without room for it.
size_t refuseArrayBytes = 0;
// The bytes the program's news hold now: what a loaded family keeps is the difference across its load.
size_t heapBytesHeld = 0;

// A block with its size in front of it (malloc and free underneath, which the sanitizers pair as they pair any other).
constexpr size_t kSizeHeader = alignof(std::max_align_t);
void* holdBytes(const size_t size) noexcept {
  auto* const block = static_cast<unsigned char*>(std::malloc(kSizeHeader + size));
  if (!block) return nullptr;
  std::memcpy(block, &size, sizeof size);
  heapBytesHeld += size;
  return block + kSizeHeader;
}
void freeBytes(void* p) noexcept {
  if (!p) return;
  auto* const block = static_cast<unsigned char*>(p) - kSizeHeader;
  size_t size = 0;
  std::memcpy(&size, block, sizeof size);
  heapBytesHeld -= size;
  std::free(block);
}
}  // namespace

// The test program's new and delete, so a test can refuse an array new and count what the heap holds.
void* operator new(const size_t size) {
  if (void* p = holdBytes(size)) return p;
  throw std::bad_alloc();
}
void* operator new(const size_t size, const std::nothrow_t&) noexcept { return holdBytes(size); }
void* operator new[](const size_t size) {
  if (void* p = holdBytes(size)) return p;
  throw std::bad_alloc();
}
void* operator new[](const size_t size, const std::nothrow_t&) noexcept {
  if (refuseArrayBytes != 0 && size == refuseArrayBytes) return nullptr;
  return holdBytes(size);
}
void operator delete(void* p) noexcept { freeBytes(p); }
void operator delete(void* p, size_t) noexcept { freeBytes(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { freeBytes(p); }
void operator delete[](void* p) noexcept { freeBytes(p); }
void operator delete[](void* p, size_t) noexcept { freeBytes(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { freeBytes(p); }

namespace {

constexpr uint32_t kFlashBytes = 3538944 - 65536;  // the X3's font area: spiffs below the language slot
constexpr uint8_t kRegular = 0;
constexpr uint8_t kBold = 1;
constexpr uint8_t kItalic = 2;
constexpr uint8_t kIdentityA[glyphstore::kIdentityBytes] = {'f', 'a', 'm', 'i', 'l', 'y', '-', 'A'};
constexpr uint8_t kIdentityB[glyphstore::kIdentityBytes] = {'f', 'a', 'm', 'i', 'l', 'y', '-', 'B'};

/// What the rasterizer draws for `cp` at `points`, straight from the file: what a stored glyph must equal.
struct Reference {
  EpdGlyph metrics{};
  std::vector<uint8_t> bitmap;
};

/// The same at a pixels-per-em (26.6).
Reference referenceAt(const std::vector<uint8_t>& file, const uint32_t cp, const uint32_t ppem) {
  FontBytes bytes(file.data(), static_cast<uint32_t>(file.size()));
  ttf::Face face;
  EXPECT_EQ(ttf::FaceStatus::Ok, ttf::parseFace(bytes, &face));
  ttf::Rasterizer rasterizer;
  EXPECT_TRUE(rasterizer.allocate(
      ttf::planScratch(face, static_cast<uint32_t>(TtfFont::kMaxNativePx) << 6, ttf::kPoolBytesFlash)));
  Reference ref;
  EXPECT_EQ(ttf::Status::Ok, rasterizer.render(bytes, face, ttf::glyphIndex(bytes, face, cp), ppem, &ref.metrics));
  ref.bitmap.assign(rasterizer.output(), rasterizer.output() + ref.metrics.dataLength);
  return ref;
}

Reference reference(const std::vector<uint8_t>& file, const uint32_t cp, const uint8_t points) {
  return referenceAt(file, cp, ttf::ppemFromPoints(points));
}

constexpr uint32_t px(const uint32_t pixels) { return pixels << 6; }

/// A glyph's bitmap, wherever TtfFont keeps it: the ring, or the store's log.
std::vector<uint8_t> bitmapOf(const TtfFont& font, const uint8_t style, const EpdGlyph* glyph) {
  const uint8_t* bitmap = nullptr;
  if (!font.ownsBitmap(glyph, bitmap)) bitmap = font.font(style)->data->bitmap + glyph->dataOffset;
  return bitmap ? std::vector<uint8_t>(bitmap, bitmap + glyph->dataLength) : std::vector<uint8_t>{};
}

void expectDrawnAs(const Reference& ref, const TtfFont& font, const uint8_t style, const EpdGlyph* glyph) {
  ASSERT_NE(nullptr, glyph);
  EXPECT_EQ(ref.metrics.width, glyph->width);
  EXPECT_EQ(ref.metrics.height, glyph->height);
  EXPECT_EQ(ref.metrics.left, glyph->left);
  EXPECT_EQ(ref.metrics.top, glyph->top);
  EXPECT_EQ(ref.metrics.advanceX, glyph->advanceX);
  EXPECT_EQ(ref.bitmap, bitmapOf(font, style, glyph));
}

int yields = 0;
int lockChecks = 0;

/// A set of one family, loaded, held as that family: what each boot of the tests below sees.
struct Loaded {
  std::unique_ptr<TtfFontSet> set;
  TtfFont* operator->() const { return set->font(0); }
  TtfFont& operator*() const { return *set->font(0); }
  bool operator==(std::nullptr_t) const { return set == nullptr; }
};

class TtfFontTest : public ::testing::Test {
 protected:
  void SetUp() override {
    files[0].bytes = readFont("Bookerly/Bookerly-Regular.ttf");
    files[1].bytes = readFont("Bookerly/Bookerly-Bold.ttf");
    files[2].bytes = readFont("Bookerly/Bookerly-Italic.ttf");
    files[3].bytes = readFont("Bookerly/Bookerly-BoldItalic.ttf");
    yields = lockChecks = 0;
  }
  void TearDown() override {
    TtfFont::setHooks({});
    refuseArrayBytes = 0;
    EXPECT_EQ(0u, flash.violations());
    EXPECT_EQ(0u, flash.reprograms());
  }

  /// The family over the test's flash, loaded and set to `points`: what each boot sees. Null when it fails.
  Loaded open(const uint8_t* identity = kIdentityA, const uint8_t points = 14) {
    Loaded font{std::make_unique<TtfFontSet>(flash)};
    ttf::FileSource sources[4];
    for (int s = 0; s < 4; ++s) {
      if (!files[s].bytes.empty()) sources[s] = files[s].source();
    }
    if (!font.set->load(&sources, 1, identity, [this] { ++busy; })) return {};
    if (points != 0 && !font->setBodySize(points)) return {};
    return font;
  }

  /// A one-style family built in memory, so a test can give a glyph any outline.
  void useBuiltFont(const Font& font) {
    files[0].bytes = buildFont(font);
    for (int s = 1; s < 4; ++s) files[s].bytes.clear();
  }

  SimNorFlash flash{kFlashBytes};
  MemoryFile files[4];
  int busy = 0;
};

}  // namespace

TEST_F(TtfFontTest, TheFirstLoadInstallsTheFamilyAndTheNextReusesIt) {
  ASSERT_NE(nullptr, open());
  EXPECT_EQ(1, busy);
  const uint32_t erases = flash.totalSectorErases();
  const auto again = open();
  ASSERT_NE(nullptr, again);
  EXPECT_EQ(1, busy);  // not installed twice
  EXPECT_EQ(erases, flash.totalSectorErases());
  EXPECT_NE(nullptr, again->font(kBold));
}

TEST_F(TtfFontTest, AnotherFamilyIsInstalledOverTheStore) {
  ASSERT_NE(nullptr, open(kIdentityA));
  ASSERT_NE(nullptr, open(kIdentityB));
  EXPECT_EQ(2, busy);
}

TEST_F(TtfFontTest, AStoreThatNoLongerMapsIsInstalledAgain) {
  ASSERT_NE(nullptr, open());
  flash.setFreePages(20);  // a firmware update left fewer mapping pages than the install planned for
  const auto font = open();
  ASSERT_NE(nullptr, font);
  EXPECT_EQ(2, busy);
  expectDrawnAs(reference(files[kRegular].bytes, 'A', 14), *font, kRegular, font->glyph(kRegular, 'A'));
}

TEST_F(TtfFontTest, AStyleThatNoLongerMapsIsInstalledAgain) {
  ASSERT_NE(nullptr, open());
  // Fewer mapping pages than the install planned for, but enough for the index and log regions: open() succeeds
  // and only a style's own mapping fails.
  flash.setFreePages(26);
  const auto font = open();
  ASSERT_NE(nullptr, font);
  EXPECT_EQ(2, busy);
  EXPECT_NE(nullptr, font->font(kBold));
  expectDrawnAs(reference(files[kRegular].bytes, 'A', 14), *font, kRegular, font->glyph(kRegular, 'A'));
  expectDrawnAs(reference(files[kBold].bytes, 'A', 14), *font, kBold, font->glyph(kBold, 'A'));
}

TEST_F(TtfFontTest, AFamilyWithoutARegularStyleIsRefused) {
  files[0].bytes.clear();
  EXPECT_EQ(nullptr, open());
  EXPECT_EQ(0u, flash.totalSectorErases());
}

TEST_F(TtfFontTest, AFamilyTooLargeForThePartitionIsRefusedBeforeAnyErase) {
  SimNorFlash small{1024 * 1024};
  TtfFontSet set(small);
  ttf::FileSource sources[4];
  for (int s = 0; s < 4; ++s) sources[s] = files[s].source();
  EXPECT_FALSE(set.load(&sources, 1, kIdentityA, [this] { ++busy; }));
  EXPECT_EQ(0, busy);
  EXPECT_EQ(0u, small.totalSectorErases());
}

TEST_F(TtfFontTest, AStyleTheEngineCannotReadIsLeftOut) {
  files[kItalic].bytes.assign(5000, 0x41);  // the file changed after discovery: not a font any more
  const auto font = open();
  ASSERT_NE(nullptr, font);
  EXPECT_EQ(nullptr, font->font(kItalic));
  EXPECT_NE(nullptr, font->font(kBold));
}

TEST_F(TtfFontTest, StoredGlyphsAreWhatTheRasterizerDraws) {
  const auto font = open();
  ASSERT_NE(nullptr, font);
  for (const uint32_t cp : {uint32_t{'A'}, uint32_t{'g'}, uint32_t{'W'}, uint32_t{0xE9}, uint32_t{' '}}) {
    expectDrawnAs(reference(files[kRegular].bytes, cp, 14), *font, kRegular, font->glyph(kRegular, cp));
    expectDrawnAs(reference(files[kBold].bytes, cp, 14), *font, kBold, font->glyph(kBold, cp));
  }
}

TEST_F(TtfFontTest, AGlyphIsRenderedOnceAndThenReadFromTheStore) {
  const auto font = open();
  ASSERT_NE(nullptr, font);
  const EpdGlyph* first = font->glyph(kRegular, 'A');
  const uint32_t writes = flash.programCalls();
  const EpdGlyph* second = font->glyph(kRegular, 'A');
  EXPECT_EQ(first, second);  // the record in the log, in place
  EXPECT_EQ(1u, font->renders());
  EXPECT_EQ(1u, font->storeHits());
  EXPECT_EQ(writes, flash.programCalls());
  EXPECT_FALSE(font->ringMode());
}

TEST_F(TtfFontTest, StoredGlyphsSurviveTheNextBoot) {
  {
    const auto font = open();
    ASSERT_NE(nullptr, font);
    ASSERT_NE(nullptr, font->glyph(kBold, 'Q'));
  }
  const auto font = open();
  ASSERT_NE(nullptr, font);
  expectDrawnAs(reference(files[kBold].bytes, 'Q', 14), *font, kBold, font->glyph(kBold, 'Q'));
  EXPECT_EQ(0u, font->renders());
  EXPECT_EQ(1u, font->storeHits());
}

TEST_F(TtfFontTest, ACodepointTheFontLacksIsAskedOnlyOnce) {
  const auto font = open();
  ASSERT_NE(nullptr, font);
  EXPECT_EQ(nullptr, font->glyph(kRegular, 0x4E2D));  // Bookerly has no CJK
  const uint32_t writes = flash.programCalls();
  EXPECT_EQ(nullptr, font->glyph(kRegular, 0x4E2D));
  EXPECT_EQ(writes, flash.programCalls());  // the "missing" slot answered
}

TEST_F(TtfFontTest, DefaultIgnorableCodepointsHaveNoAdvanceAndNoInk) {
  const auto font = open();
  ASSERT_NE(nullptr, font);
  const uint32_t writes = flash.programCalls();
  const EpdGlyph* zwsp = font->glyph(kRegular, 0x200B);
  ASSERT_NE(nullptr, zwsp);
  EXPECT_EQ(0, zwsp->advanceX);
  EXPECT_EQ(0, zwsp->width);
  EXPECT_EQ(0, zwsp->height);
  const uint8_t* bitmap = &kIdentityA[0];
  EXPECT_TRUE(font->ownsBitmap(zwsp, bitmap));
  EXPECT_EQ(nullptr, bitmap);
  EXPECT_EQ(writes, flash.programCalls());  // nothing stored for it
}

TEST_F(TtfFontTest, EachBodySizeHasItsOwnGlyphs) {
  const auto font = open();
  ASSERT_NE(nullptr, font);
  const EpdGlyph* at14 = font->glyph(kRegular, 'M');
  ASSERT_TRUE(font->setBodySize(20));
  expectDrawnAs(reference(files[kRegular].bytes, 'M', 20), *font, kRegular, font->glyph(kRegular, 'M'));
  ASSERT_TRUE(font->setBodySize(14));
  EXPECT_EQ(at14, font->glyph(kRegular, 'M'));  // still stored under its own size
  EXPECT_EQ(2u, font->renders());
}

TEST_F(TtfFontTest, TheSmallestReaderSizeIsDrawnNatively) {
  const auto font = open(kIdentityA, 8);
  ASSERT_NE(nullptr, font);
  for (const uint32_t cp : {uint32_t{'a'}, uint32_t{'W'}, uint32_t{0xE9}}) {
    expectDrawnAs(reference(files[kRegular].bytes, cp, 8), *font, kRegular, font->glyph(kRegular, cp));
  }
}

TEST_F(TtfFontTest, TheLineMetricsAreTheFacesAtTheBodySize) {
  const auto font = open();
  ASSERT_NE(nullptr, font);
  FontBytes bytes(files[kRegular].bytes.data(), static_cast<uint32_t>(files[kRegular].bytes.size()));
  ttf::Face face;
  ASSERT_EQ(ttf::FaceStatus::Ok, ttf::parseFace(bytes, &face));
  const ttf::LineMetrics expected = ttf::lineMetrics(face, ttf::ppemFromPoints(14));
  const EpdFontData* data = font->font(kRegular)->data;
  EXPECT_EQ(expected.advanceY, data->advanceY);
  EXPECT_EQ(expected.ascender, data->ascender);
  EXPECT_EQ(expected.descender, data->descender);
  EXPECT_TRUE(data->is2Bit);
  EXPECT_EQ(0u, data->intervalCount);  // every lookup goes to the provider
  EXPECT_NE(nullptr, data->provider);
}

TEST_F(TtfFontTest, SizesOutsideTheReaderRangeAreRefused) {
  const auto font = open();
  ASSERT_NE(nullptr, font);
  EXPECT_FALSE(font->setBodySize(0));
  EXPECT_FALSE(font->setBodySize(TtfFont::kMaxPointSize + 1));
  EXPECT_EQ(14, font->bodySize());  // unchanged
  EXPECT_TRUE(font->setBodySize(TtfFont::kMaxPointSize));
}

TEST_F(TtfFontTest, AFaceWhoseLineHeightDoesNotFitIsRefused) {
  Font built;
  built.ascender = 5000;  // a 282 px line at the largest size
  built.glyphs.resize(2);
  built.glyphs[0].contours = {rect(0, 0, 400, 600)};
  built.glyphs[1].contours = {rect(50, 0, 450, 700)};
  built.cmap = {{'A', 1}};
  useBuiltFont(built);
  const auto font = open(kIdentityA, 0);
  ASSERT_NE(nullptr, font);
  EXPECT_TRUE(font->setBodySize(10));
  EXPECT_FALSE(font->setBodySize(TtfFont::kMaxPointSize));
  EXPECT_EQ(10, font->bodySize());
}

TEST_F(TtfFontTest, AWriteFailureSwitchesToTheRingWithTheSameGlyphs) {
  const auto font = open();
  ASSERT_NE(nullptr, font);
  ASSERT_NE(nullptr, font->glyph(kRegular, 'A'));  // stored before the failure
  flash.failNextProgram();
  const EpdGlyph* b = font->glyph(kRegular, 'B');
  EXPECT_TRUE(font->ringMode());
  expectDrawnAs(reference(files[kRegular].bytes, 'B', 14), *font, kRegular, b);
  const uint8_t* bitmap = nullptr;
  EXPECT_TRUE(font->ownsBitmap(b, bitmap));
  // Stored glyphs are still read from the store; new ones are drawn without a write.
  const uint32_t writes = flash.programCalls();
  EXPECT_FALSE(font->ownsBitmap(font->glyph(kRegular, 'A'), bitmap));
  expectDrawnAs(reference(files[kRegular].bytes, 'C', 14), *font, kRegular, font->glyph(kRegular, 'C'));
  EXPECT_EQ(writes, flash.programCalls());
}

TEST_F(TtfFontTest, RingRecordsStayValidForTheNextSevenLookups) {
  const auto font = open();
  ASSERT_NE(nullptr, font);
  flash.failNextProgram();
  const std::string letters = "BCDEFGHI";
  std::vector<const EpdGlyph*> glyphs;
  for (const char c : letters) glyphs.push_back(font->glyph(kRegular, static_cast<uint8_t>(c)));
  ASSERT_TRUE(font->ringMode());
  for (size_t i = 0; i < letters.size(); ++i) {
    expectDrawnAs(reference(files[kRegular].bytes, static_cast<uint8_t>(letters[i]), 14), *font, kRegular, glyphs[i]);
  }
}

TEST_F(TtfFontTest, AWipeAtTheNextBookOpenEndsTheRing) {
  const auto font = open();
  ASSERT_NE(nullptr, font);
  ASSERT_NE(nullptr, font->glyph(kRegular, 'A'));
  flash.failNextProgram();
  ASSERT_NE(nullptr, font->glyph(kRegular, 'B'));
  ASSERT_TRUE(font->ringMode());
  busy = 0;
  EXPECT_TRUE(font.set->wipeIfPending([this] { ++busy; }));
  EXPECT_EQ(1, busy);
  EXPECT_FALSE(font->ringMode());
  EXPECT_FALSE(font.set->wipeIfPending());  // nothing pending now
  const uint32_t renders = font->renders();
  const EpdGlyph* a = font->glyph(kRegular, 'A');  // the wipe emptied the store: rendered and stored again
  EXPECT_EQ(renders + 1, font->renders());
  const uint8_t* bitmap = nullptr;
  EXPECT_FALSE(font->ownsBitmap(a, bitmap));
  expectDrawnAs(reference(files[kRegular].bytes, 'A', 14), *font, kRegular, a);
}

TEST_F(TtfFontTest, AStoreMarkedFullActsAsAFullOneAndIsCounted) {
  // CMD:TTF:FULL: what a filled store does, at any size, without filling it.
  const TtfFontSet::StoreEvents before = TtfFontSet::storeEvents();
  {
    const auto font = open();
    ASSERT_NE(nullptr, font);
    ASSERT_NE(nullptr, font->glyph(kRegular, 'A'));
    TtfFontSet::Stats stats = font.set->stats();
    EXPECT_EQ(1, stats.families);
    EXPECT_FALSE(stats.ringMode);
    EXPECT_FALSE(stats.wipePending);
    EXPECT_GT(stats.logUsed, 0u);
    EXPECT_GT(stats.logSize, stats.logUsed);
    EXPECT_EQ(1u, stats.renders);
    ASSERT_TRUE(font.set->markFull());
    stats = font.set->stats();
    EXPECT_TRUE(stats.ringMode);
    EXPECT_TRUE(stats.wipePending);
    EXPECT_EQ(before.fills + 1, TtfFontSet::storeEvents().fills);
    expectDrawnAs(reference(files[kRegular].bytes, 'B', 14), *font, kRegular, font->glyph(kRegular, 'B'));
    EXPECT_TRUE(font->ringMode());  // drawn, not stored
  }
  const auto font = open();  // the next boot finishes the pending wipe
  ASSERT_NE(nullptr, font);
  EXPECT_FALSE(font.set->stats().wipePending);
  EXPECT_EQ(before.wipes + 1, TtfFontSet::storeEvents().wipes);
}

TEST_F(TtfFontTest, AFullStoreSwitchesToTheRingAndTheLoadAfterItWipes) {
  flash.setFreePages(16);  // the smallest log the planner makes: 4 pages
  {
    const auto font = open(kIdentityA, TtfFont::kMaxPointSize);
    ASSERT_NE(nullptr, font);
    for (uint32_t cp = 0x21; cp < 0x3000 && !font->ringMode(); ++cp) font->glyph(kRegular, cp);
    ASSERT_TRUE(font->ringMode());
    expectDrawnAs(reference(files[kRegular].bytes, 0x2026, TtfFont::kMaxPointSize), *font, kRegular,
                  font->glyph(kRegular, 0x2026));
  }
  busy = 0;
  const auto font = open(kIdentityA, TtfFont::kMaxPointSize);  // the next boot finishes the pending wipe
  ASSERT_NE(nullptr, font);
  EXPECT_EQ(1, busy);
  EXPECT_FALSE(font->ringMode());
  const uint8_t* bitmap = nullptr;
  EXPECT_FALSE(font->ownsBitmap(font->glyph(kRegular, 'A'), bitmap));
  EXPECT_EQ(1u, font->renders());
}

TEST_F(TtfFontTest, ABrokenGlyphIsMarkedMissingAndNotTriedAgain) {
  Font built;
  built.glyphs.resize(3);
  built.glyphs[0].contours = {rect(0, 0, 400, 600)};
  built.glyphs[1].contours = {rect(50, 0, 450, 700)};
  built.glyphs[2].raw = {0x00, 0x01};  // one contour, then nothing: Malformed
  built.cmap = {{'A', 1}, {'B', 2}};
  useBuiltFont(built);
  const auto font = open();
  ASSERT_NE(nullptr, font);
  EXPECT_EQ(nullptr, font->glyph(kRegular, 'B'));
  const uint32_t writes = flash.programCalls();
  EXPECT_EQ(nullptr, font->glyph(kRegular, 'B'));
  EXPECT_EQ(writes, flash.programCalls());
  EXPECT_NE(nullptr, font->glyph(kRegular, 'A'));
  EXPECT_EQ(nullptr, font->font(kBold));  // a one-style family
}

TEST_F(TtfFontTest, AGlyphReachingTooFarFromThePenIsMissing) {
  Font built;
  built.glyphs.resize(3);
  built.glyphs[0].contours = {rect(0, 0, 400, 600)};
  built.glyphs[1].contours = {rect(50, 0, 450, 700)};
  built.glyphs[2].contours = {rect(0, 2900, 100, 3000)};  // 163 px above the baseline at 26 pt: past int8
  built.cmap = {{'A', 1}, {'T', 2}};
  useBuiltFont(built);
  const auto font = open(kIdentityA, TtfFont::kMaxPointSize);
  ASSERT_NE(nullptr, font);
  EXPECT_EQ(nullptr, font->glyph(kRegular, 'T'));
  const uint32_t writes = flash.programCalls();
  EXPECT_EQ(nullptr, font->glyph(kRegular, 'T'));
  EXPECT_EQ(writes, flash.programCalls());  // marked missing for good
  ASSERT_TRUE(font->setBodySize(10));       // 63 px: drawn
  EXPECT_NE(nullptr, font->glyph(kRegular, 'T'));
}

TEST_F(TtfFontTest, TextMeasuresTheSameColdWarmAndFromTheRing) {
  const char* text = "Sphinx of black quartz, judge my vow";
  int coldWidth = 0, coldHeight = 0;
  {
    const auto font = open();
    ASSERT_NE(nullptr, font);
    font->font(kRegular)->getTextDimensions(text, &coldWidth, &coldHeight);
    ASSERT_GT(coldWidth, 0);
  }
  int warmWidth = 0, warmHeight = 0;
  {
    const auto font = open();
    ASSERT_NE(nullptr, font);
    font->font(kRegular)->getTextDimensions(text, &warmWidth, &warmHeight);
    EXPECT_EQ(0u, font->renders());
  }
  int ringWidth = 0, ringHeight = 0;
  {
    const auto font = open(kIdentityB);  // a fresh store...
    ASSERT_NE(nullptr, font);
    flash.failNextProgram();  // ...that fails at its first write
    font->font(kRegular)->getTextDimensions(text, &ringWidth, &ringHeight);
    EXPECT_TRUE(font->ringMode());
  }
  EXPECT_EQ(coldWidth, warmWidth);
  EXPECT_EQ(coldHeight, warmHeight);
  EXPECT_EQ(coldWidth, ringWidth);
  EXPECT_EQ(coldHeight, ringHeight);
}

TEST_F(TtfFontTest, TheFamilyServesEpdFontFamilyStyles) {
  const auto font = open();
  ASSERT_NE(nullptr, font);
  const EpdFontFamily family(font->font(0), font->font(1), font->font(2), font->font(3));
  const EpdGlyphRef bold = family.getGlyph('W', EpdFontFamily::BOLD);
  ASSERT_TRUE(bold);
  EXPECT_EQ(reference(files[kBold].bytes, 'W', 14).metrics.advanceX, bold.advanceX);
  EXPECT_NE(family.getGlyph('W', EpdFontFamily::REGULAR).advanceX, bold.advanceX);
}

TEST_F(TtfFontTest, TheAppHooksAreCalled) {
  TtfFont::setHooks({[] {
                       ++lockChecks;
                       return true;
                     },
                     [] { ++yields; }});
  const auto font = open();
  ASSERT_NE(nullptr, font);
  EXPECT_GT(yields, 0);  // between install chunks
  EXPECT_EQ(0, lockChecks);
  ASSERT_NE(nullptr, font->glyph(kRegular, 'A'));
  EXPECT_EQ(1, lockChecks);  // a render checks the lock; a store hit does not need it
  ASSERT_NE(nullptr, font->glyph(kRegular, 'A'));
  EXPECT_EQ(1, lockChecks);
}

// --- prefetch() -------------------------------------------------------------------------------------------------

namespace {

std::string utf8(const std::vector<uint32_t>& codepoints) {
  std::string out;
  for (const uint32_t cp : codepoints) {
    if (cp < 0x80) {
      out += static_cast<char>(cp);
    } else {
      out += static_cast<char>(0xC0 | (cp >> 6));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    }
  }
  return out;
}

}  // namespace

TEST_F(TtfFontTest, PrefetchStoresATextsMissingGlyphsInBatches) {
  const auto font = open();
  ASSERT_NE(nullptr, font);
  const uint32_t writes = flash.programCalls();
  font->prefetch(kRegular, "Hello world");
  EXPECT_EQ(8u, font->renders());  // H e l o space w r d
  // One log write per batch and one slot per glyph; one glyph at a time would be two writes each.
  EXPECT_GE(flash.programCalls() - writes, 9u);
  EXPECT_LE(flash.programCalls() - writes, 10u);
  for (const char c : std::string("Hello world")) {
    expectDrawnAs(reference(files[kRegular].bytes, static_cast<uint8_t>(c), 14), *font, kRegular,
                  font->glyph(kRegular, static_cast<uint8_t>(c)));
  }
  EXPECT_EQ(8u, font->renders());  // all found in the store
}

TEST_F(TtfFontTest, PrefetchSkipsWhatIsAlreadyStored) {
  const auto font = open();
  ASSERT_NE(nullptr, font);
  font->prefetch(kRegular, "Hello world");
  const uint32_t writes = flash.programCalls();
  font->prefetch(kRegular, "Hello world, world");
  EXPECT_EQ(9u, font->renders());               // only the comma is new
  EXPECT_EQ(writes + 2, flash.programCalls());  // one batch, one slot
}

TEST_F(TtfFontTest, PrefetchRendersAtMostItsLimitAndLookupsRenderTheRest) {
  const auto font = open();
  ASSERT_NE(nullptr, font);
  std::vector<uint32_t> codepoints;
  for (uint32_t cp = 'A'; cp <= 'Z'; ++cp) codepoints.push_back(cp);
  for (uint32_t cp = 'a'; cp <= 'z'; ++cp) codepoints.push_back(cp);
  for (uint32_t cp = '0'; cp <= '9'; ++cp) codepoints.push_back(cp);
  for (uint32_t cp = 0xC0; cp <= 0xD6; ++cp) codepoints.push_back(cp);
  font->prefetch(kRegular, utf8(codepoints).c_str());
  EXPECT_EQ(TtfFontSet::kPrefetchGlyphs, font->renders());
  for (const uint32_t cp : codepoints) {
    expectDrawnAs(reference(files[kRegular].bytes, cp, 14), *font, kRegular, font->glyph(kRegular, cp));
  }
  EXPECT_EQ(codepoints.size(), font->renders());
}

TEST_F(TtfFontTest, PrefetchMarksCodepointsTheFontLacks) {
  const auto font = open();
  ASSERT_NE(nullptr, font);
  font->prefetch(kRegular, "\xE4\xB8\xAD");  // U+4E2D
  const uint32_t writes = flash.programCalls();
  EXPECT_EQ(nullptr, font->glyph(kRegular, 0x4E2D));
  EXPECT_EQ(writes, flash.programCalls());
}

TEST_F(TtfFontTest, PrefetchInRingModeWritesNothing) {
  const auto font = open();
  ASSERT_NE(nullptr, font);
  flash.failNextProgram();
  ASSERT_NE(nullptr, font->glyph(kRegular, 'A'));
  ASSERT_TRUE(font->ringMode());
  const uint32_t writes = flash.programCalls();
  const uint32_t renders = font->renders();
  font->prefetch(kRegular, "DEF");
  EXPECT_EQ(writes, flash.programCalls());
  EXPECT_EQ(renders, font->renders());  // nothing to batch for: they are drawn as they are looked up
}

TEST_F(TtfFontTest, PrefetchYieldsBetweenGlyphs) {
  const auto font = open();
  ASSERT_NE(nullptr, font);
  TtfFont::setHooks({[] {
                       ++lockChecks;
                       return true;
                     },
                     [] { ++yields; }});
  font->prefetch(kRegular, "abc");
  EXPECT_EQ(3, yields);
  EXPECT_EQ(1, lockChecks);
}

TEST_F(TtfFontTest, EpdFontFamilyPrefetchReachesTheStylesFont) {
  const auto font = open();
  ASSERT_NE(nullptr, font);
  const EpdFontFamily family(font->font(0), font->font(1), font->font(2), font->font(3));
  family.prefetch("Wolf", 1u << EpdFontFamily::BOLD);
  EXPECT_EQ(4u, font->renders());
  expectDrawnAs(reference(files[kBold].bytes, 'W', 14), *font, kBold, font->glyph(kBold, 'W'));
  EXPECT_EQ(4u, font->renders());  // prefetched
}

// --- Native sizes: headings, CSS sizes and drop caps at their own pixel size ---

namespace {

/// A native size's glyph, as the renderer asks for it: through the style's provider.
const EpdGlyph* nativeGlyph(const EpdFont* font, const uint32_t cp) {
  return font ? font->data->provider->missGlyph(cp) : nullptr;
}

}  // namespace

TEST_F(TtfFontTest, ANativeSizeIsDrawnAtItsOwnPixelSize) {
  const auto font = open();
  ASSERT_NE(nullptr, font);
  const EpdFont* fonts[4] = {};
  ASSERT_TRUE(font->nativeFonts(40, fonts));
  for (const uint8_t style : {kRegular, kBold, kItalic}) {
    ASSERT_NE(nullptr, fonts[style]);
    expectDrawnAs(referenceAt(files[style].bytes, 'M', px(40)), *font, style, nativeGlyph(fonts[style], 'M'));
  }
  // Its line metrics are the face's at 40 px.
  FontBytes bytes(files[kRegular].bytes.data(), static_cast<uint32_t>(files[kRegular].bytes.size()));
  ttf::Face face;
  ASSERT_EQ(ttf::FaceStatus::Ok, ttf::parseFace(bytes, &face));
  const ttf::LineMetrics expected = ttf::lineMetrics(face, px(40));
  EXPECT_EQ(expected.advanceY, fonts[kRegular]->data->advanceY);
  EXPECT_EQ(expected.ascender, fonts[kRegular]->data->ascender);
  EXPECT_EQ(expected.descender, fonts[kRegular]->data->descender);
}

TEST_F(TtfFontTest, ANativeSizeIsMadeOnceAndStoredApartFromTheBody) {
  const auto font = open();
  ASSERT_NE(nullptr, font);
  const EpdFont* first[4] = {};
  const EpdFont* again[4] = {};
  const EpdFont* other[4] = {};
  ASSERT_TRUE(font->nativeFonts(40, first));
  ASSERT_TRUE(font->nativeFonts(40, again));
  ASSERT_TRUE(font->nativeFonts(41, other));
  EXPECT_EQ(first[kRegular], again[kRegular]);
  EXPECT_NE(first[kRegular], other[kRegular]);
  EXPECT_EQ(2u, font->nativeSizeCount());
  const EpdGlyph* body = font->glyph(kRegular, 'M');
  const EpdGlyph* at40 = nativeGlyph(first[kRegular], 'M');
  ASSERT_NE(nullptr, at40);
  EXPECT_NE(body->width, at40->width);
  EXPECT_EQ(at40, nativeGlyph(again[kRegular], 'M'));  // read from the store the second time
  EXPECT_EQ(2u, font->renders());
}

TEST_F(TtfFontTest, NativeSizesOutsideTheRangeAreRefused) {
  const auto font = open();
  ASSERT_NE(nullptr, font);
  const EpdFont* fonts[4] = {};
  EXPECT_FALSE(font->nativeFonts(TtfFont::kMinNativePx - 1, fonts));
  EXPECT_FALSE(font->nativeFonts(TtfFont::kMaxNativePx + 1, fonts));
  EXPECT_EQ(nullptr, fonts[kRegular]);
  EXPECT_EQ(0u, font->nativeSizeCount());
}

TEST_F(TtfFontTest, TheLargestNativeSizeDrawsEveryGlyphOfTheFamily) {
  // Planned for: the output region holds each glyph, and its top fits EpdGlyphRef.
  const auto font = open();
  ASSERT_NE(nullptr, font);
  EXPECT_EQ(119, font->maxNativePx());  // Bookerly: 120 px less the pixel kept in hand for rounding
  const EpdFont* fonts[4] = {};
  ASSERT_TRUE(font->nativeFonts(font->maxNativePx(), fonts));
  for (const uint32_t cp : {uint32_t{'W'}, uint32_t{'@'}, uint32_t{'Q'}, uint32_t{0xC5}}) {
    for (const uint8_t style : {kRegular, kBold, kItalic}) {
      const EpdGlyph* glyph = nativeGlyph(fonts[style], cp);
      ASSERT_NE(nullptr, glyph) << std::hex << cp << " style " << int{style};
      expectDrawnAs(referenceAt(files[style].bytes, cp, px(font->maxNativePx())), *font, style, glyph);
    }
  }
}

TEST_F(TtfFontTest, TheLargestNativeSizeKeepsTheFamilysTallestGlyphInsideEpdGlyphRef) {
  // A glyph reaching 1.2 em above the baseline (stacked accents, as in Vietnamese capitals) passes int8's 127 px
  // top above 105 px: the family's native sizes stop there, and larger text is the largest of them scaled.
  Font built;
  built.glyphs.resize(2);
  built.glyphs[0].contours = {rect(0, 0, 400, 600)};
  built.glyphs[1].contours = {rect(50, 0, 450, 1200)};
  built.cmap = {{'A', 1}};
  useBuiltFont(built);
  const auto font = open();
  ASSERT_NE(nullptr, font);
  EXPECT_EQ(104, font->maxNativePx());  // 127 * 1000 / 1200, less a pixel for rounding
  const EpdFont* fonts[4] = {};
  EXPECT_FALSE(font->nativeFonts(105, fonts));
  ASSERT_TRUE(font->nativeFonts(104, fonts));
  const EpdGlyph* tall = nativeGlyph(fonts[kRegular], 'A');
  ASSERT_NE(nullptr, tall);  // drawn, not marked missing
  EXPECT_LE(tall->top, INT8_MAX);
}

TEST_F(TtfFontTest, TheRingStaysWithinItsBudgetAndGoesWithTheNativeSizes) {
  // A native size's glyphs are large (2-4 KB at the largest size); drawn through the ring, eight of them would hold
  // tens of KB until the next wipe. A record is promised only until the next miss, so the oldest go first.
  const auto font = open();
  ASSERT_NE(nullptr, font);
  const EpdFont* fonts[4] = {};
  ASSERT_TRUE(font->nativeFonts(font->maxNativePx(), fonts));
  flash.failNextProgram();  // the size's claim fails: its glyphs are drawn into the ring
  for (const char c : std::string("WMQ@BDGHKNOR")) {
    const EpdGlyph* glyph = nativeGlyph(fonts[kRegular], static_cast<uint8_t>(c));
    const uint8_t* bitmap = nullptr;
    ASSERT_TRUE(font->ownsBitmap(glyph, bitmap)) << c;
    expectDrawnAs(referenceAt(files[kRegular].bytes, static_cast<uint8_t>(c), px(font->maxNativePx())), *font, kRegular,
                  glyph);  // the newest record is always whole
    EXPECT_LE(font.set->ringBytes(), TtfFontSet::kRingBudgetBytes) << c;
  }
  font->dropNativeSizes();  // only where no record is in use: book open
  EXPECT_EQ(0u, font.set->ringBytes());
}

TEST_F(TtfFontTest, WhenEveryNativeSizeIsHeldTheNearestServesUntilTheyAreDropped) {
  const auto font = open();
  ASSERT_NE(nullptr, font);
  const EpdFont* fonts[4] = {};
  for (uint8_t i = 0; i < TtfFont::kMaxNativeSizes; ++i) {
    ASSERT_TRUE(font->nativeFonts(static_cast<uint8_t>(TtfFont::kMinNativePx + i), fonts)) << int{i};
  }
  const EpdFont* largestHeld[4] = {};
  const uint8_t largestPx = TtfFont::kMinNativePx + TtfFont::kMaxNativeSizes - 1;
  uint32_t served = 0;
  ASSERT_TRUE(font->nativeFonts(largestPx, largestHeld, &served));
  EXPECT_EQ(px(largestPx), served);  // its own size
  ASSERT_TRUE(font->nativeFonts(font->maxNativePx(), fonts, &served));
  EXPECT_EQ(largestHeld[kRegular], fonts[kRegular]);
  EXPECT_EQ(px(largestPx), served);  // the nearest size serves, and says so: the caller scales it
  EXPECT_EQ(TtfFont::kMaxNativeSizes, font->nativeSizeCount());
  font->dropNativeSizes();
  EXPECT_EQ(0u, font->nativeSizeCount());
  ASSERT_TRUE(font->nativeFonts(font->maxNativePx(), fonts));
  expectDrawnAs(referenceAt(files[kRegular].bytes, 'M', px(font->maxNativePx())), *font, kRegular,
                nativeGlyph(fonts[kRegular], 'M'));
}

TEST_F(TtfFontTest, ADroppedSizeMadeAgainFindsItsStoredGlyphs) {
  const auto font = open();
  ASSERT_NE(nullptr, font);
  const EpdFont* fonts[4] = {};
  ASSERT_TRUE(font->nativeFonts(40, fonts));
  ASSERT_NE(nullptr, nativeGlyph(fonts[kRegular], 'M'));
  font->dropNativeSizes();
  ASSERT_TRUE(font->nativeFonts(40, fonts));
  expectDrawnAs(referenceAt(files[kRegular].bytes, 'M', px(40)), *font, kRegular, nativeGlyph(fonts[kRegular], 'M'));
  EXPECT_EQ(1u, font->renders());
}

TEST_F(TtfFontTest, ASizeTheFullTableRefusesIsDrawnFromTheRingWhileTheOthersStore) {
  const auto font = open();  // the body takes the first size entry
  ASSERT_NE(nullptr, font);
  const EpdFont* fonts[4] = {};
  // Every native size takes an entry at its first glyph: 16, dropped, then 15 more fill the 32 entries.
  for (uint8_t pixels = 20; pixels < 20 + 31; ++pixels) {
    if (font->nativeSizeCount() == TtfFont::kMaxNativeSizes) font->dropNativeSizes();
    ASSERT_TRUE(font->nativeFonts(pixels, fonts));
    ASSERT_NE(nullptr, nativeGlyph(fonts[kRegular], 'a')) << int{pixels};
  }
  font->dropNativeSizes();
  ASSERT_TRUE(font->nativeFonts(60, fonts));
  const EpdGlyph* refused = nativeGlyph(fonts[kRegular], 'M');
  const uint8_t* bitmap = nullptr;
  EXPECT_TRUE(font->ownsBitmap(refused, bitmap));  // drawn without being stored
  expectDrawnAs(referenceAt(files[kRegular].bytes, 'M', px(60)), *font, kRegular, refused);
  EXPECT_FALSE(font->ringMode());
  EXPECT_FALSE(font->ownsBitmap(font->glyph(kRegular, 'Q'), bitmap));  // the body still stores its glyphs
  // The wipe at the next book open frees the table.
  EXPECT_TRUE(font.set->wipeIfPending());
  EXPECT_FALSE(font->ownsBitmap(nativeGlyph(fonts[kRegular], 'M'), bitmap));
  expectDrawnAs(referenceAt(files[kRegular].bytes, 'M', px(60)), *font, kRegular, nativeGlyph(fonts[kRegular], 'M'));
}

TEST_F(TtfFontTest, PrefetchStoresANativeSizesGlyphs) {
  const auto font = open();
  ASSERT_NE(nullptr, font);
  const EpdFont* fonts[4] = {};
  ASSERT_TRUE(font->nativeFonts(48, fonts));
  fonts[kRegular]->data->provider->prefetch("Chapter One");
  const uint32_t renders = font->renders();
  EXPECT_EQ(10u, renders);  // C h a p t e r O n and the space (no ink, but a glyph): each once
  for (const char c : std::string("Chapter One")) {
    expectDrawnAs(referenceAt(files[kRegular].bytes, static_cast<uint8_t>(c), px(48)), *font, kRegular,
                  nativeGlyph(fonts[kRegular], static_cast<uint8_t>(c)));
  }
  EXPECT_EQ(renders, font->renders());
}

// --- Kerning and ligatures ---

namespace {

// Glyphs 1-4 for A-D, 1000 units per em. A kerns B by -100 units and D by 0; C kerns with nothing.
Font kernedBoxes() {
  Font font;
  font.glyphs.resize(5);
  for (auto& glyph : font.glyphs) glyph.contours = {rect(0, 0, 400, 600)};
  for (uint16_t g = 1; g <= 4; ++g) font.cmap[static_cast<uint32_t>('A' + g - 1)] = g;
  LayoutLookup lookup;
  PairSubtable pairs;
  pairs.pairs = {{1, {{2, -100}, {4, 0}}}};
  lookup.pairSubtables = {pairs};
  font.extraTables = {{"GPOS", layoutTable({{"latn", {0}}}, {{"kern", {0}}}, {lookup})}};
  return font;
}

}  // namespace

TEST_F(TtfFontTest, KerningIsTheFontsAtEachSizesScale) {
  useBuiltFont(kernedBoxes());
  const auto font = open();
  // -100 units at 29.17 px per em: -2.9 px, in 4.4. Asked again, as EpdFont asks, it comes from the store.
  EXPECT_EQ(-47, font->kerning(kRegular, 'A', 'B'));
  EXPECT_EQ(-47, font->font(kRegular)->getKerning('A', 'B'));
  EXPECT_EQ(1u, font->kernLookups());
  const EpdFont* fonts[4] = {};
  ASSERT_TRUE(font->nativeFonts(48, fonts));
  EXPECT_EQ(-77, fonts[kRegular]->getKerning('A', 'B'));  // the same pair at 48 px: -4.8 px
  EXPECT_EQ(1u, font->kernLookups());
}

TEST_F(TtfFontTest, StoredKerningServesTheNextBoot) {
  useBuiltFont(kernedBoxes());
  {
    const auto font = open();
    ASSERT_EQ(-47, font->kerning(kRegular, 'A', 'B'));
  }
  const auto font = open();
  EXPECT_EQ(-47, font->kerning(kRegular, 'A', 'B'));
  EXPECT_EQ(0u, font->kernLookups());
}

TEST_F(TtfFontTest, OnlyPairsWhoseLeftGlyphKernsAreStored) {
  useBuiltFont(kernedBoxes());
  const auto font = open();
  const uint32_t writes = flash.programCalls();
  EXPECT_EQ(0, font->kerning(kRegular, 'C', 'A'));  // C kerns with nothing: no slot spent
  EXPECT_EQ(writes, flash.programCalls());
  EXPECT_EQ(0, font->kerning(kRegular, 'A', 'D'));  // A kerns, not with D: remembered as 0
  EXPECT_LT(writes, flash.programCalls());
  EXPECT_EQ(0, font->kerning(kRegular, 'A', 'D'));
  EXPECT_EQ(2u, font->kernLookups());
}

TEST_F(TtfFontTest, ALeftGlyphThatKernsWithNothingIsLookedUpOnce) {
  // Such pairs are not stored, yet every word gap asks for one (the space kerns with nothing in Bookerly).
  useBuiltFont(kernedBoxes());
  const auto font = open();
  EXPECT_EQ(0, font->kerning(kRegular, 'C', 'A'));
  EXPECT_EQ(1u, font->kernLookups());
  EXPECT_EQ(0, font->kerning(kRegular, 'C', 'A'));  // the same pair again
  EXPECT_EQ(0, font->kerning(kRegular, 'C', 'B'));  // another pair with C on the left
  const EpdFont* fonts[4] = {};
  ASSERT_TRUE(font->nativeFonts(48, fonts));
  EXPECT_EQ(0, fonts[kRegular]->getKerning('C', 'D'));  // and at another size
  EXPECT_EQ(1u, font->kernLookups());
  // A left glyph that kerns still kerns, whatever its pairs before gave: a right glyph the font lacks ('Z') says
  // nothing of the left one, nor does a pair it kerns by 0.
  EXPECT_EQ(0, font->kerning(kRegular, 'A', 'Z'));
  EXPECT_EQ(0, font->kerning(kRegular, 'A', 'D'));
  EXPECT_EQ(-47, font->kerning(kRegular, 'A', 'B'));
  EXPECT_EQ(4u, font->kernLookups());
}

TEST_F(TtfFontTest, KerningGoesOnWithoutWritesInRingMode) {
  useBuiltFont(kernedBoxes());
  const auto font = open();
  flash.failNextProgram();
  ASSERT_NE(nullptr, font->glyph(kRegular, 'A'));
  ASSERT_TRUE(font->ringMode());
  const uint32_t writes = flash.programCalls();
  EXPECT_EQ(-47, font->kerning(kRegular, 'A', 'B'));
  EXPECT_EQ(writes, flash.programCalls());
}

TEST_F(TtfFontTest, AFontWithoutKerningAsksNothing) {
  Font plain = kernedBoxes();
  plain.extraTables.clear();
  useBuiltFont(plain);
  const auto font = open();
  EXPECT_EQ(0, font->kerning(kRegular, 'A', 'B'));
  EXPECT_EQ(0u, font->kernLookups());
}

// What a family keeps on the heap grows with the styles it has, a native size's too: a book's accent family of one
// style holds no memory for the three it lacks.
TEST_F(TtfFontTest, AFamilyHoldsMemoryOnlyForTheStylesItHas) {
  Font built = kernedBoxes();
  built.extraTables.clear();  // no kerning: nothing else a style holds differs between them
  const std::vector<uint8_t> bytes = buildFont(built);
  const uint8_t* const identities[] = {kIdentityA, kIdentityB, kIdentityA};
  const int styleCounts[] = {1, 2, 4};
  size_t family[3] = {};
  size_t native[3] = {};
  for (int i = 0; i < 3; ++i) {
    for (int s = 0; s < 4; ++s) {
      files[s].bytes = s < styleCounts[i] ? bytes : std::vector<uint8_t>{};
    }
    ASSERT_NE(nullptr, open(identities[i]));  // installed: what an install or the first mapping keeps is not counted
    const size_t before = heapBytesHeld;
    const auto font = open(identities[i]);
    ASSERT_NE(nullptr, font);
    family[i] = heapBytesHeld - before;
    const EpdFont* fonts[4] = {};
    const size_t beforeNative = heapBytesHeld;
    ASSERT_TRUE(font->nativeFonts(40, fonts));
    native[i] = heapBytesHeld - beforeNative;
    for (int s = 0; s < 4; ++s) EXPECT_EQ(s < styleCounts[i], fonts[s] != nullptr) << s;
    EXPECT_EQ(styleCounts[i] > kBold, font->font(kBold) != nullptr);
  }
  std::printf("[  heap    ] loaded set, 1/2/4 styles: %zu/%zu/%zu B; a native size: %zu/%zu/%zu B\n", family[0],
              family[1], family[2], native[0], native[1], native[2]);
  const size_t perStyle = family[1] - family[0];
  EXPECT_GT(perStyle, 0u);
  EXPECT_EQ(3 * perStyle, family[2] - family[0]);
  const size_t perNativeStyle = native[1] - native[0];
  EXPECT_GT(perNativeStyle, 0u);
  EXPECT_EQ(3 * perNativeStyle, native[2] - native[0]);
}

TEST_F(TtfFontTest, TheFiLigatureDrawsTheGlyphGsubNames) {
  Font font;
  font.glyphs.resize(4);
  font.glyphs[0].contours = {rect(0, 0, 400, 600)};
  font.glyphs[1].contours = {rect(0, 0, 300, 700)};  // f
  font.glyphs[2].contours = {rect(0, 0, 200, 500)};  // i
  font.glyphs[3].contours = {rect(0, 0, 900, 700)};  // the fi ligature: wider than f and i together
  font.cmap = {{'f', 1}, {'i', 2}};
  LayoutLookup liga;
  liga.type = 4;
  liga.ligatures = {{{1, 2}, 3}};
  font.extraTables = {{"GSUB", layoutTable({{"latn", {0}}}, {{"liga", {0}}}, {liga}, /*gsub=*/true)}};
  useBuiltFont(font);
  const auto ttf = open();
  ASSERT_NE(nullptr, ttf);
  const EpdFont* regular = ttf->font(kRegular);
  EXPECT_EQ(1u, regular->data->ligaturePairCount);
  const char* rest = "i";
  EXPECT_EQ(0xFB01u, regular->applyLigatures('f', rest));
  const EpdGlyph* fi = ttf->glyph(kRegular, 0xFB01);
  ASSERT_NE(nullptr, fi);
  EXPECT_GE(fi->width, 26);  // 900 units at 29.17 px per em
  // Every size shares the style's table.
  const EpdFont* fonts[4] = {};
  ASSERT_TRUE(ttf->nativeFonts(40, fonts));
  EXPECT_EQ(regular->data->ligaturePairs, fonts[kRegular]->data->ligaturePairs);
}

TEST_F(TtfFontTest, ALigatureKernsAsTheGlyphGsubNames) {
  // "fi" then "x": EpdFont asks for the pair (U+FB01, x), which the font kerns through the ligature's glyph (4),
  // not through the cmap (no U+FB01 there) nor the i.
  Font font;
  font.glyphs.resize(5);
  for (auto& glyph : font.glyphs) glyph.contours = {rect(0, 0, 400, 600)};
  font.cmap = {{'f', 1}, {'i', 2}, {'x', 3}};
  LayoutLookup liga;
  liga.type = 4;
  liga.ligatures = {{{1, 2}, 4}};
  LayoutLookup kern;
  PairSubtable pairs;
  pairs.pairs = {{4, {{3, -100}}}};
  kern.pairSubtables = {pairs};
  font.extraTables = {{"GSUB", layoutTable({{"latn", {0}}}, {{"liga", {0}}}, {liga}, /*gsub=*/true)},
                      {"GPOS", layoutTable({{"latn", {0}}}, {{"kern", {0}}}, {kern})}};
  useBuiltFont(font);
  const auto ttf = open();
  ASSERT_NE(nullptr, ttf);
  // -100 units at 29.17 px per em, in 4.4, as in layout so in drawing.
  EXPECT_EQ(-47, ttf->kerning(kRegular, 0xFB01, 'x'));
  EXPECT_EQ(-47, ttf->font(kRegular)->getKerning(0xFB01, 'x'));
  EXPECT_EQ(0, ttf->kerning(kRegular, 'i', 'x'));
}

// --- TtfFontSet: families drawn from one store ------------------------------------------------------------------

namespace {

constexpr uint8_t kIdentitySet[glyphstore::kIdentityBytes] = {'s', 'e', 't'};

// A body family (Bookerly, four styles) and an accent family (Inter, two), as Publisher Fonts loads a reader font
// and a book's accent family.
class TtfFontSetTest : public TtfFontTest {
 protected:
  void SetUp() override {
    TtfFontTest::SetUp();
    accent[kRegular].bytes = readFont("Inter/Inter-Regular.ttf");
    accent[kBold].bytes = readFont("Inter/Inter-Bold.ttf");
  }

  /// `count` of the body and the accent family (the accent family first if `accentFirst`), on `chip` or the test's
  /// flash, each at 14 pt: what each boot sees. Null when it fails.
  std::unique_ptr<TtfFontSet> openSet(const uint8_t* identity = kIdentitySet, const uint8_t count = 2,
                                      const bool accentFirst = false, SimNorFlash* chip = nullptr) {
    auto set = std::make_unique<TtfFontSet>(chip ? *chip : flash);
    ttf::FileSource sources[2][4];
    MemoryFile* order[2] = {files, accent};
    if (accentFirst) std::swap(order[0], order[1]);
    for (uint8_t f = 0; f < 2; ++f) {
      for (int s = 0; s < 4; ++s) {
        if (!order[f][s].bytes.empty()) sources[f][s] = order[f][s].source();
      }
    }
    if (!set->load(sources, count, identity, [this] { ++busy; })) return nullptr;
    for (uint8_t f = 0; f < count; ++f) {
      if (!set->font(f)->setBodySize(14)) return nullptr;
    }
    return set;
  }

  MemoryFile accent[4];
};

}  // namespace

TEST_F(TtfFontSetTest, TheFamiliesAreInstalledTogetherAndTheNextLoadReusesThem) {
  ASSERT_NE(nullptr, openSet());
  EXPECT_EQ(1, busy);
  const uint32_t erases = flash.totalSectorErases();
  const auto set = openSet();
  ASSERT_NE(nullptr, set);
  EXPECT_EQ(1, busy);  // not installed twice
  EXPECT_EQ(erases, flash.totalSectorErases());
  EXPECT_EQ(2, set->familyCount());
  EXPECT_NE(nullptr, set->font(0)->font(kItalic));
  EXPECT_NE(nullptr, set->font(1)->font(kBold));
  EXPECT_EQ(nullptr, set->font(1)->font(kItalic));  // no Inter italic here
  EXPECT_EQ(nullptr, set->font(2));
}

TEST_F(TtfFontSetTest, EachFamilyDrawsItsOwnGlyphsAndFindsThemTheNextBoot) {
  {
    const auto set = openSet();
    ASSERT_NE(nullptr, set);
    for (const uint32_t cp : {uint32_t{'A'}, uint32_t{'g'}, uint32_t{0xE9}}) {
      expectDrawnAs(reference(files[kRegular].bytes, cp, 14), *set->font(0), kRegular,
                    set->font(0)->glyph(kRegular, cp));
      expectDrawnAs(reference(accent[kRegular].bytes, cp, 14), *set->font(1), kRegular,
                    set->font(1)->glyph(kRegular, cp));
    }
    expectDrawnAs(reference(accent[kBold].bytes, 'W', 14), *set->font(1), kBold, set->font(1)->glyph(kBold, 'W'));
  }
  const auto set = openSet();
  ASSERT_NE(nullptr, set);
  expectDrawnAs(reference(files[kRegular].bytes, 'g', 14), *set->font(0), kRegular, set->font(0)->glyph(kRegular, 'g'));
  expectDrawnAs(reference(accent[kRegular].bytes, 'g', 14), *set->font(1), kRegular,
                set->font(1)->glyph(kRegular, 'g'));
  EXPECT_EQ(0u, set->font(0)->renders());  // both from the store
  EXPECT_EQ(0u, set->font(1)->renders());
}

TEST_F(TtfFontSetTest, TheFamiliesShareOneScratchBlock) {
  SimNorFlash alone{kFlashBytes};
  uint32_t largest = 0;
  for (const bool accentFirst : {false, true}) {
    const auto one = openSet(accentFirst ? kIdentityB : kIdentityA, 1, accentFirst, &alone);
    ASSERT_NE(nullptr, one);
    largest = std::max(largest, one->scratchBytes());
  }
  const auto set = openSet();
  ASSERT_NE(nullptr, set);
  EXPECT_EQ(largest, set->scratchBytes());  // sized for the larger family, not for the two
}

TEST_F(TtfFontSetTest, ASetChangedSinceTheInstallIsInstalledAgain) {
  ASSERT_NE(nullptr, openSet());
  // The body alone (a book without an accent family) is another set, though family 0's keys would still match.
  ttf::FileSource sources[4];
  for (int s = 0; s < 4; ++s) sources[s] = files[s].source();
  TtfFontSet body(flash);
  ASSERT_TRUE(body.load(&sources, 1, kIdentityA, [this] { ++busy; }));
  EXPECT_EQ(2, busy);
  ASSERT_TRUE(body.font(0)->setBodySize(14));
  expectDrawnAs(reference(files[kRegular].bytes, 'A', 14), *body.font(0), kRegular, body.font(0)->glyph(kRegular, 'A'));
  EXPECT_EQ(1u, body.font(0)->renders());  // the install emptied the store
}

TEST_F(TtfFontSetTest, TheFamiliesInAnotherOrderAreAnotherSet) {
  {
    const auto set = openSet();
    ASSERT_NE(nullptr, set);
    ASSERT_NE(nullptr, set->font(0)->glyph(kRegular, 'A'));
  }
  // The caller's identity says the order (ttf::setIdentity): family 0 is now Inter, and draws as Inter.
  const auto swapped = openSet(kIdentityB, 2, /*accentFirst=*/true);
  ASSERT_NE(nullptr, swapped);
  EXPECT_EQ(2, busy);
  expectDrawnAs(reference(accent[kRegular].bytes, 'A', 14), *swapped->font(0), kRegular,
                swapped->font(0)->glyph(kRegular, 'A'));
}

TEST_F(TtfFontSetTest, KerningIsEachFamilysOwnAndServesTheNextBoot) {
  SimNorFlash alone{kFlashBytes};
  int8_t own[2] = {};
  for (const bool accentFirst : {false, true}) {
    const auto one = openSet(accentFirst ? kIdentityB : kIdentityA, 1, accentFirst, &alone);
    ASSERT_NE(nullptr, one);
    own[accentFirst ? 1 : 0] = one->font(0)->kerning(kRegular, 'A', 'V');
  }
  ASSERT_NE(own[0], own[1]);  // the two fonts kern A-V differently
  {
    const auto set = openSet();
    ASSERT_NE(nullptr, set);
    EXPECT_EQ(own[0], set->font(0)->kerning(kRegular, 'A', 'V'));
    EXPECT_EQ(own[1], set->font(1)->kerning(kRegular, 'A', 'V'));
  }
  const auto set = openSet();
  ASSERT_NE(nullptr, set);
  EXPECT_EQ(own[0], set->font(0)->kerning(kRegular, 'A', 'V'));
  EXPECT_EQ(own[1], set->font(1)->kerning(kRegular, 'A', 'V'));
  EXPECT_EQ(0u, set->font(0)->kernLookups() + set->font(1)->kernLookups());  // both from the pair index
}

TEST_F(TtfFontSetTest, EachFamilyHasItsOwnNativeSizes) {
  const auto set = openSet();
  ASSERT_NE(nullptr, set);
  const EpdFont* body[4] = {};
  const EpdFont* accented[4] = {};
  ASSERT_TRUE(set->font(0)->nativeFonts(40, body));
  ASSERT_TRUE(set->font(1)->nativeFonts(40, accented));
  expectDrawnAs(referenceAt(files[kRegular].bytes, 'M', px(40)), *set->font(0), kRegular,
                nativeGlyph(body[kRegular], 'M'));
  expectDrawnAs(referenceAt(accent[kRegular].bytes, 'M', px(40)), *set->font(1), kRegular,
                nativeGlyph(accented[kRegular], 'M'));
  EXPECT_EQ(1, set->font(0)->nativeSizeCount());
  EXPECT_EQ(1, set->font(1)->nativeSizeCount());
}

TEST_F(TtfFontSetTest, PrefetchStoresOnlyItsFamilysGlyphs) {
  const auto set = openSet();
  ASSERT_NE(nullptr, set);
  set->font(1)->prefetch(kRegular, "Hamburgefonts");
  const uint32_t accentRenders = set->font(1)->renders();
  EXPECT_GT(accentRenders, 0u);
  ASSERT_NE(nullptr, set->font(1)->glyph(kRegular, 'H'));
  EXPECT_EQ(accentRenders, set->font(1)->renders());  // stored by the batch
  ASSERT_NE(nullptr, set->font(0)->glyph(kRegular, 'H'));
  EXPECT_EQ(1u, set->font(0)->renders());  // the body family's H was not
}

TEST_F(TtfFontSetTest, AFailedWriteRingsEveryFamilyAndOneWipeServesThemAgain) {
  const auto set = openSet();
  ASSERT_NE(nullptr, set);
  ASSERT_NE(nullptr, set->font(0)->glyph(kRegular, 'A'));
  flash.failNextProgram();
  const EpdGlyph* b = set->font(1)->glyph(kRegular, 'B');
  EXPECT_TRUE(set->font(0)->ringMode());  // the store is the set's
  EXPECT_TRUE(set->font(1)->ringMode());
  expectDrawnAs(reference(accent[kRegular].bytes, 'B', 14), *set->font(1), kRegular, b);
  expectDrawnAs(reference(files[kRegular].bytes, 'C', 14), *set->font(0), kRegular, set->font(0)->glyph(kRegular, 'C'));
  busy = 0;
  EXPECT_TRUE(set->wipeIfPending([this] { ++busy; }));
  EXPECT_EQ(1, busy);
  EXPECT_FALSE(set->ringMode());
  for (uint8_t f = 0; f < 2; ++f) {
    const uint8_t* bitmap = nullptr;
    EXPECT_FALSE(set->font(f)->ownsBitmap(set->font(f)->glyph(kRegular, 'A'), bitmap)) << int{f};  // stored again
  }
  expectDrawnAs(reference(files[kRegular].bytes, 'A', 14), *set->font(0), kRegular, set->font(0)->glyph(kRegular, 'A'));
  expectDrawnAs(reference(accent[kRegular].bytes, 'A', 14), *set->font(1), kRegular,
                set->font(1)->glyph(kRegular, 'A'));
}

TEST_F(TtfFontSetTest, AnAccentFamilyWithoutARegularStyleRefusesTheSetBeforeAnyErase) {
  accent[kRegular].bytes.clear();
  EXPECT_EQ(nullptr, openSet());
  EXPECT_EQ(0, busy);
  EXPECT_EQ(0u, flash.totalSectorErases());
}

// --- Stage B phase 3: several accent families ------------------------------------------------------------------

namespace {

// Families of files, in the set's order, loaded on `chip` at 14 pt as the set `identity`; null when the set cannot
// be drawn.
std::unique_ptr<TtfFontSet> openFamilies(SimNorFlash& chip, const std::vector<MemoryFile*>& families, int& busy,
                                         const uint8_t* identity = kIdentitySet) {
  auto set = std::make_unique<TtfFontSet>(chip);
  std::vector<std::array<ttf::FileSource, 4>> sources(families.size());
  for (size_t f = 0; f < families.size(); ++f) {
    for (int s = 0; s < 4; ++s) {
      if (!families[f][s].bytes.empty()) sources[f][s] = families[f][s].source();
    }
  }
  static_assert(sizeof(std::array<ttf::FileSource, 4>) == sizeof(TtfFontSet::Files), "one family's files");
  if (!set->load(reinterpret_cast<const TtfFontSet::Files*>(sources.data()), static_cast<uint8_t>(families.size()),
                 identity, [&busy] { ++busy; })) {
    return nullptr;
  }
  for (uint8_t f = 0; f < families.size(); ++f) {
    if (!set->font(f)->setBodySize(14)) return nullptr;
  }
  return set;
}

}  // namespace

TEST_F(TtfFontSetTest, ABodyAndThreeAccentFamiliesAreDrawnFromOneStore) {
  // Inter as the body (two styles), and three one-style accent families: the four of TtfFontSet::kMaxFamilies.
  MemoryFile ubuntu[4];
  MemoryFile ubuntuBold[4];
  MemoryFile bookerly[4];
  ubuntu[kRegular].bytes = readFont("Ubuntu/Ubuntu-Regular.ttf");
  ubuntuBold[kRegular].bytes = readFont("Ubuntu/Ubuntu-Bold.ttf");
  bookerly[kRegular].bytes = files[kRegular].bytes;
  const auto set = openFamilies(flash, {accent, ubuntu, ubuntuBold, bookerly}, busy);
  ASSERT_NE(nullptr, set);
  EXPECT_EQ(TtfFontSet::kMaxFamilies, set->familyCount());
  EXPECT_EQ(1, busy);
  const MemoryFile* const sources[] = {accent, ubuntu, ubuntuBold, bookerly};
  for (uint8_t f = 0; f < 4; ++f) {
    expectDrawnAs(reference(sources[f][kRegular].bytes, 'g', 14), *set->font(f), kRegular,
                  set->font(f)->glyph(kRegular, 'g'));
  }
}

namespace {

// Sector erases over the store's first style blocks: `bytes` from where planLayout puts them, a page in.
uint32_t erasesOver(const SimNorFlash& flash, const uint32_t bytes) {
  uint32_t total = 0;
  const uint32_t first = glyphstore::kHeaderRegionBytes / glyphstore::kSectorBytes;
  for (uint32_t s = first; s < first + glyphstore::alignUp(bytes, glyphstore::kSectorBytes) / glyphstore::kSectorBytes;
       ++s) {
    total += flash.sectorErases(s);
  }
  return total;
}

}  // namespace

TEST_F(TtfFontSetTest, AnotherSetWithTheSameLeadingFamilyKeepsItsStyleBlocks) {
  // A TrueType reader font beside one book's accent family, then beside another's: the reader font's blocks hold
  // their files where the new layout puts them, and are neither erased nor written again.
  ASSERT_NE(nullptr, openSet());
  const uint32_t bodyBytes = static_cast<uint32_t>(files[kRegular].bytes.size());
  const uint32_t before = erasesOver(flash, bodyBytes);
  MemoryFile ubuntu[4];
  ubuntu[kRegular].bytes = readFont("Ubuntu/Ubuntu-Regular.ttf");
  const auto set = openFamilies(flash, {files, ubuntu}, busy, kIdentityB);
  ASSERT_NE(nullptr, set);
  EXPECT_EQ(2, busy);                               // installed again: another set
  EXPECT_EQ(before, erasesOver(flash, bodyBytes));  // Bookerly Regular's block kept
  EXPECT_EQ(0u, flash.reprograms());                // and nothing written over it
  expectDrawnAs(reference(files[kBold].bytes, 'W', 14), *set->font(0), kBold, set->font(0)->glyph(kBold, 'W'));
  expectDrawnAs(reference(ubuntu[kRegular].bytes, 'W', 14), *set->font(1), kRegular,
                set->font(1)->glyph(kRegular, 'W'));  // where Inter was: written anew
}

TEST_F(TtfFontSetTest, ABlockHoldingAnotherFileIsWrittenAgain) {
  ASSERT_NE(nullptr, openSet());
  const uint32_t interBytes = static_cast<uint32_t>(accent[kRegular].bytes.size());
  const uint32_t before = erasesOver(flash, interBytes);
  // Inter first: its block starts where Bookerly Regular's did, which holds Bookerly.
  const auto swapped = openSet(kIdentityB, 2, /*accentFirst=*/true);
  ASSERT_NE(nullptr, swapped);
  EXPECT_LT(before, erasesOver(flash, interBytes));
  expectDrawnAs(reference(accent[kRegular].bytes, 'A', 14), *swapped->font(0), kRegular,
                swapped->font(0)->glyph(kRegular, 'A'));
}

TEST_F(TtfFontSetTest, MoreStylesThanTheStoreHoldsAreRefusedBeforeAnyErase) {
  // Bookerly's four, Inter's two, Bookerly's four again: ten style blocks for the store's eight.
  EXPECT_EQ(nullptr, openFamilies(flash, {files, accent, files}, busy));
  EXPECT_EQ(0, busy);
  EXPECT_EQ(0u, flash.totalSectorErases());
}

// --- Stage 2c-4: SD mode, a style's tables in the store and its outlines on the card ---------------------------

namespace {

constexpr uint32_t kPage = glyphstore::kPageBytes;
// Bookerly's four styles take 35 of a font area's pages in flash mode (the whole files), 19 in SD mode (their tables,
// 3 pages each): on 24 pages only SD mode fits.
constexpr uint32_t kSdOnlyBytes = 24 * kPage;

void expectWrittenOnce(const SimNorFlash& chip) {
  EXPECT_EQ(0u, chip.violations());
  EXPECT_EQ(0u, chip.reprograms());
}

uint32_t erasesBetween(const SimNorFlash& chip, const uint32_t from, const uint32_t to) {
  uint32_t total = 0;
  for (uint32_t sector = from / glyphstore::kSectorBytes; sector < to / glyphstore::kSectorBytes; ++sector) {
    total += chip.sectorErases(sector);
  }
  return total;
}

}  // namespace

TEST_F(TtfFontSetTest, AFamilyTooLargeForFlashModeIsDrawnFromTheCardAsFlashModeDrawsIt) {
  SimNorFlash chip{kSdOnlyBytes};
  {
    const auto set = openFamilies(chip, {files}, busy);
    ASSERT_NE(nullptr, set);
    EXPECT_EQ(1, busy);
    TtfFont& font = *set->font(0);
    EXPECT_TRUE(font.readsCard());
    for (uint8_t style = 0; style < 4; ++style) {
      for (const uint32_t cp : {uint32_t{'A'}, uint32_t{'g'}, uint32_t{'W'}, uint32_t{0xE9}, uint32_t{' '}}) {
        expectDrawnAs(reference(files[style].bytes, cp, 14), font, style, font.glyph(style, cp));
      }
    }
    const EpdFont* fonts[4] = {};
    ASSERT_TRUE(font.nativeFonts(40, fonts));
    expectDrawnAs(referenceAt(files[kRegular].bytes, 'M', px(40)), font, kRegular, nativeGlyph(fonts[kRegular], 'M'));
    EXPECT_GT(set->sdPageReads(), 0u);
  }
  // The next boot draws them from the store: neither rendered again nor read from the card, but for the table
  // directory the open compares with the block's.
  for (MemoryFile& file : files) file.offsets.clear();
  const auto set = openFamilies(chip, {files}, busy);
  ASSERT_NE(nullptr, set);
  EXPECT_EQ(1, busy);
  expectDrawnAs(reference(files[kBold].bytes, 'W', 14), *set->font(0), kBold, set->font(0)->glyph(kBold, 'W'));
  EXPECT_EQ(0u, set->font(0)->renders());
  const std::vector<uint8_t>& bold = files[kBold].bytes;
  const uint32_t directoryEnd = 12 + 16u * static_cast<uint32_t>(bold[4] << 8 | bold[5]);
  ASSERT_FALSE(files[kBold].offsets.empty());
  for (const uint32_t offset : files[kBold].offsets) EXPECT_LT(offset, directoryEnd);
  expectWrittenOnce(chip);
}

TEST_F(TtfFontSetTest, KerningAndLigaturesInSdModeAreFlashModes) {
  SimNorFlash chip{kSdOnlyBytes};
  const auto card = openFamilies(chip, {files}, busy);
  const auto whole = openFamilies(flash, {files}, busy);
  ASSERT_NE(nullptr, card);
  ASSERT_NE(nullptr, whole);
  ASSERT_TRUE(card->font(0)->readsCard());
  ASSERT_FALSE(whole->font(0)->readsCard());
  for (const auto& [left, right] : {std::pair<uint32_t, uint32_t>{'A', 'V'}, {'T', 'o'}, {'W', 'e'}, {'f', 'i'}}) {
    EXPECT_EQ(whole->font(0)->kerning(kRegular, left, right), card->font(0)->kerning(kRegular, left, right));
  }
  EXPECT_NE(0, card->font(0)->kerning(kRegular, 'A', 'V'));
  const EpdFont* regular = card->font(0)->font(kRegular);
  ASSERT_NE(nullptr, regular);
  ASSERT_GT(regular->data->ligaturePairCount, 0u);  // GSUB read from the store, in place
  EXPECT_EQ(whole->font(0)->font(kRegular)->data->ligaturePairCount, regular->data->ligaturePairCount);
  const EpdGlyph* fromCard = card->font(0)->glyph(kRegular, 0xFB01);
  const EpdGlyph* fromFlash = whole->font(0)->glyph(kRegular, 0xFB01);
  ASSERT_NE(nullptr, fromCard);
  ASSERT_NE(nullptr, fromFlash);
  EXPECT_EQ(bitmapOf(*whole->font(0), kRegular, fromFlash), bitmapOf(*card->font(0), kRegular, fromCard));
}

TEST_F(TtfFontSetTest, APrefetchReadsTheOutlinesForwardThroughTheFile) {
  // A backward seek makes SdFat walk the file's cluster chain from its start (spec 5.1): the page's misses, in
  // whatever order the text has them, are drawn in glyph order, which is file order.
  SimNorFlash chip{kSdOnlyBytes};
  const auto set = openFamilies(chip, {files}, busy);
  ASSERT_NE(nullptr, set);
  files[kRegular].offsets.clear();
  set->font(0)->prefetch(kRegular, "zyxwvutsrqponmlkjihgfedcbaZYXWVUTSRQPONMLKJIHGFEDCBA");
  const std::vector<uint32_t>& reads = files[kRegular].offsets;
  ASSERT_GT(reads.size(), 4u);
  EXPECT_TRUE(std::is_sorted(reads.begin(), reads.end()));
  const uint32_t rendered = set->font(0)->renders();
  EXPECT_EQ(52u, rendered);
  for (const uint32_t cp : {uint32_t{'a'}, uint32_t{'q'}, uint32_t{'Z'}}) {
    expectDrawnAs(reference(files[kRegular].bytes, cp, 14), *set->font(0), kRegular, set->font(0)->glyph(kRegular, cp));
  }
  EXPECT_EQ(rendered, set->font(0)->renders());  // stored by the batch
}

TEST_F(TtfFontSetTest, AFileOnTheCardThatChangedInstallsTheSetAgain) {
  // The identity covers each file's name, size and time; its stamp, the path too. A store whose SD-mode file is
  // another than the one its tables came from would read outlines at the wrong offsets, so it is installed again.
  SimNorFlash chip{kSdOnlyBytes};
  ASSERT_NE(nullptr, openFamilies(chip, {files}, busy));
  files[kItalic].stamp ^= 1;  // the same name, size and time, in another directory
  auto set = openFamilies(chip, {files}, busy);
  ASSERT_NE(nullptr, set);
  EXPECT_EQ(2, busy);
  expectDrawnAs(reference(files[kItalic].bytes, 'g', 14), *set->font(0), kItalic, set->font(0)->glyph(kItalic, 'g'));
  set.reset();
  // Another file of another size under the same identity: installed again too, its own outlines drawn.
  files[kItalic].bytes = readFont("Bookerly/Bookerly-BoldItalic.ttf");
  set = openFamilies(chip, {files}, busy);
  ASSERT_NE(nullptr, set);
  EXPECT_EQ(3, busy);
  expectDrawnAs(reference(files[kItalic].bytes, 'g', 14), *set->font(0), kItalic, set->font(0)->glyph(kItalic, 'g'));
  set.reset();
  ASSERT_NE(nullptr, openFamilies(chip, {files}, busy));
  EXPECT_EQ(3, busy);  // and kept from then on
  expectWrittenOnce(chip);
}

TEST_F(TtfFontSetTest, AFileRewrittenInPlaceAtTheSameSizeAndTimeInstallsTheSetAgain) {
  // The stamp and the size match; the file's table directory does not (one table's checksum changed).
  SimNorFlash chip{kSdOnlyBytes};
  ASSERT_NE(nullptr, openFamilies(chip, {files}, busy));
  std::vector<uint8_t>& italic = files[kItalic].bytes;
  italic[findRecord(italic, "GPOS") + 4] ^= 0x5A;
  const auto set = openFamilies(chip, {files}, busy);
  ASSERT_NE(nullptr, set);
  EXPECT_EQ(2, busy);
  expectDrawnAs(reference(italic, 'k', 14), *set->font(0), kItalic, set->font(0)->glyph(kItalic, 'k'));
  expectWrittenOnce(chip);
}

TEST_F(TtfFontSetTest, AnAccentStaysWholeWhenNothingWithMoreOnTheCardFits) {
  // 47 pages and 17 free mapping pages: both families whole fit with a 4-page log, which the 8-page rule would
  // refuse; but Inter's tables, mapped beside Bookerly's largest style, pass the mapping budget. Whole beats left
  // out.
  SimNorFlash tight{47 * kPage, 17};
  const auto set = openFamilies(tight, {files, accent}, busy);
  ASSERT_NE(nullptr, set);
  EXPECT_EQ(2, set->familyCount());
  EXPECT_FALSE(set->font(0)->readsCard());
  EXPECT_FALSE(set->font(1)->readsCard());
}

TEST_F(TtfFontSetTest, ACardReadErrorDrawsTheGlyphAsMissingAndAsksAgain) {
  SimNorFlash chip{kSdOnlyBytes};
  const auto set = openFamilies(chip, {files}, busy);
  ASSERT_NE(nullptr, set);
  files[kRegular].failReads = true;
  EXPECT_EQ(nullptr, set->font(0)->glyph(kRegular, 'R'));  // transient: not marked as missing
  files[kRegular].failReads = false;
  expectDrawnAs(reference(files[kRegular].bytes, 'R', 14), *set->font(0), kRegular, set->font(0)->glyph(kRegular, 'R'));
  EXPECT_FALSE(set->ringMode());
}

TEST_F(TtfFontSetTest, TheBodyStaysInFlashModeWhileTheAccentFamilyGoesToTheCard) {
  // 42 pages: Bookerly's and Inter's whole files (40 pages and a header) leave no room for a log; Bookerly whole
  // and Inter's tables (6 pages) leave 7.
  SimNorFlash chip{42 * kPage};
  {
    const auto set = openFamilies(chip, {files, accent}, busy);
    ASSERT_NE(nullptr, set);
    EXPECT_FALSE(set->font(0)->readsCard());
    EXPECT_TRUE(set->font(1)->readsCard());
    expectDrawnAs(reference(files[kRegular].bytes, 'Q', 14), *set->font(0), kRegular,
                  set->font(0)->glyph(kRegular, 'Q'));
    expectDrawnAs(reference(accent[kBold].bytes, 'Q', 14), *set->font(1), kBold, set->font(1)->glyph(kBold, 'Q'));
    for (MemoryFile& file : files) file.reads = 0;
    accent[kRegular].reads = 0;
    ASSERT_NE(nullptr, set->font(0)->glyph(kRegular, 'k'));
    ASSERT_NE(nullptr, set->font(1)->glyph(kRegular, 'k'));
    EXPECT_EQ(0u, files[kRegular].reads);   // the body's outlines come from flash
    EXPECT_GT(accent[kRegular].reads, 0u);  // the accent's from the card
  }
  const auto set = openFamilies(chip, {files, accent}, busy);
  ASSERT_NE(nullptr, set);
  EXPECT_EQ(1, busy);
  expectDrawnAs(reference(accent[kBold].bytes, 'Q', 14), *set->font(1), kBold, set->font(1)->glyph(kBold, 'Q'));
  EXPECT_EQ(0u, set->font(1)->renders());
  expectWrittenOnce(chip);
}

TEST_F(TtfFontSetTest, AnAccentFamilyKeepsFlashModeOnlyWhileTheLogKeepsEightPages) {
  // 47 pages: both families whole leave a log of 4 pages (the least the planner allows), Inter on the card one of
  // 10. The accent goes to the card; the body stays whole.
  SimNorFlash tight{47 * kPage};
  const auto set = openFamilies(tight, {files, accent}, busy);
  ASSERT_NE(nullptr, set);
  EXPECT_FALSE(set->font(0)->readsCard());
  EXPECT_TRUE(set->font(1)->readsCard());
  // The test's 53 pages leave both whole a log of 9: both stay in flash mode, as before SD mode.
  const auto roomy = openFamilies(flash, {files, accent}, busy);
  ASSERT_NE(nullptr, roomy);
  EXPECT_FALSE(roomy->font(0)->readsCard());
  EXPECT_FALSE(roomy->font(1)->readsCard());
}

TEST_F(TtfFontSetTest, ABodyTooLargeForFlashModeTakesTheAccentFamiliesToTheCardWithIt) {
  // 26 pages: Bookerly whole does not fit; every style's tables do (12 + 6 pages, a header and a log of 5).
  SimNorFlash chip{26 * kPage};
  const auto set = openFamilies(chip, {files, accent}, busy);
  ASSERT_NE(nullptr, set);
  EXPECT_TRUE(set->font(0)->readsCard());
  EXPECT_TRUE(set->font(1)->readsCard());
  expectDrawnAs(reference(files[kItalic].bytes, 'y', 14), *set->font(0), kItalic, set->font(0)->glyph(kItalic, 'y'));
  expectDrawnAs(reference(accent[kRegular].bytes, 'y', 14), *set->font(1), kRegular,
                set->font(1)->glyph(kRegular, 'y'));
  expectWrittenOnce(chip);
}

TEST_F(TtfFontSetTest, AnSdModeBlockThatHoldsItsTablesIsKept) {
  // Bookerly whole beside Inter's tables, then beside Inter's and Ubuntu's: the first six blocks hold what the new
  // layout puts there, the SD-mode ones included, and only Ubuntu's tables are written.
  SimNorFlash chip{46 * kPage};
  ASSERT_NE(nullptr, openFamilies(chip, {files, accent}, busy));
  const uint32_t interAt = glyphstore::kHeaderRegionBytes + 4 * 7 * kPage;
  const uint32_t before = erasesBetween(chip, glyphstore::kHeaderRegionBytes, interAt + 6 * kPage);
  MemoryFile ubuntu[4];
  ubuntu[kRegular].bytes = readFont("Ubuntu/Ubuntu-Regular.ttf");
  const auto set = openFamilies(chip, {files, accent, ubuntu}, busy, kIdentityB);
  ASSERT_NE(nullptr, set);
  EXPECT_EQ(2, busy);
  EXPECT_FALSE(set->font(0)->readsCard());
  ASSERT_TRUE(set->font(1)->readsCard());
  ASSERT_TRUE(set->font(2)->readsCard());
  EXPECT_EQ(before, erasesBetween(chip, glyphstore::kHeaderRegionBytes, interAt + 6 * kPage));  // all six kept
  expectDrawnAs(reference(accent[kBold].bytes, 'B', 14), *set->font(1), kBold, set->font(1)->glyph(kBold, 'B'));
  expectDrawnAs(reference(ubuntu[kRegular].bytes, 'B', 14), *set->font(2), kRegular,
                set->font(2)->glyph(kRegular, 'B'));
  expectWrittenOnce(chip);
}

TEST_F(TtfFontSetTest, APowerCutInAnSdModeInstallIsInstalledWholeAtTheNextLoad) {
  // Inter Regular on 10 pages: its tables (3 pages) fit, its file (6) does not. A cut at every 997th step of the
  // install (a block made from the file's tables, then the header): the next boot finds no store, or a whole one, and
  // draws from it what the file draws, without programming a byte twice.
  MemoryFile inter[4];
  inter[kRegular].bytes = accent[kRegular].bytes;
  SimNorFlash dry{10 * kPage};
  ASSERT_NE(nullptr, openFamilies(dry, {inter}, busy));
  ASSERT_TRUE(openFamilies(dry, {inter}, busy)->font(0)->readsCard());
  const Reference want = reference(inter[kRegular].bytes, 'G', 14);
  const uint64_t steps = dry.steps();
  for (const bool partial : {false, true}) {
    for (uint64_t cut = 0; cut < steps; cut += 997) {
      SimNorFlash chip{10 * kPage};
      chip.partialCut(partial);
      chip.cutAfterSteps(static_cast<int64_t>(cut));
      openFamilies(chip, {inter}, busy);
      chip.powerCycle();
      const auto set = openFamilies(chip, {inter}, busy);
      ASSERT_NE(nullptr, set) << cut;
      expectDrawnAs(want, *set->font(0), kRegular, set->font(0)->glyph(kRegular, 'G'));
      expectWrittenOnce(chip);
      if (HasFailure()) return;
    }
  }
}

TEST_F(TtfFontSetTest, AWholeReaderFontAndThreeAccentFamiliesOnTheCardFitTogether) {
  // The X4 case of phase 3: Bookerly whole as the reader's font, and a book's three accent families (Inter in two
  // styles, Noto Sans Italic, Ubuntu Bold). Whole, they need 56 of the font area's 53 pages, and phase 3 left two of
  // them out; with their tables on the card all four load, eight style blocks.
  MemoryFile script[4];
  MemoryFile code[4];
  script[kRegular].bytes = readFont("NotoSans/NotoSans-Italic.ttf");
  code[kRegular].bytes = readFont("Ubuntu/Ubuntu-Bold.ttf");
  const auto set = openFamilies(flash, {files, accent, script, code}, busy);
  ASSERT_NE(nullptr, set);
  EXPECT_EQ(4, set->familyCount());
  EXPECT_FALSE(set->font(0)->readsCard());
  for (uint8_t f = 1; f < 4; ++f) EXPECT_TRUE(set->font(f)->readsCard()) << int{f};
  const MemoryFile* const sources[] = {files, accent, script, code};
  for (uint8_t f = 0; f < 4; ++f) {
    expectDrawnAs(reference(sources[f][kRegular].bytes, 'e', 14), *set->font(f), kRegular,
                  set->font(f)->glyph(kRegular, 'e'));
  }
}

TEST_F(TtfFontSetTest, AFamilyTooLargeEvenForSdModeIsRefusedBeforeAnyErase) {
  SimNorFlash chip{16 * kPage};  // Bookerly's tables alone take 12 pages and a header
  EXPECT_EQ(nullptr, openFamilies(chip, {files}, busy));
  EXPECT_EQ(0, busy);
  EXPECT_EQ(0u, chip.totalSectorErases());
}

// --- What only a miss needs: made for the first glyph the store does not hold, in a lent arena or on the heap ------

TEST_F(TtfFontSetTest, AGlyphFromTheStoreNeedsNoMissMemory) {
  {
    const auto set = openSet();
    ASSERT_NE(nullptr, set);
    ASSERT_NE(nullptr, set->font(0)->glyph(kRegular, 'A'));
    ASSERT_NE(nullptr, set->font(1)->glyph(kRegular, 'A'));
  }
  const auto set = openSet();  // the next boot: the store holds both
  ASSERT_NE(nullptr, set);
  EXPECT_EQ(0u, set->releaseMissMemory());  // nothing made at load
  const uint32_t drawn = glyph_miss::drawnCount();
  expectDrawnAs(reference(files[kRegular].bytes, 'A', 14), *set->font(0), kRegular, set->font(0)->glyph(kRegular, 'A'));
  expectDrawnAs(reference(accent[kRegular].bytes, 'A', 14), *set->font(1), kRegular,
                set->font(1)->glyph(kRegular, 'A'));
  set->font(0)->prefetch(kRegular, "AAA");
  EXPECT_EQ(0u, set->font(0)->renders());
  EXPECT_EQ(drawn, glyph_miss::drawnCount());
  EXPECT_EQ(0u, set->releaseMissMemory());  // and nothing made for them
}

TEST_F(TtfFontSetTest, TheFirstMissMakesTheScratchBlockAndTheReleaseFreesIt) {
  const auto set = openSet();
  ASSERT_NE(nullptr, set);
  ASSERT_GT(set->scratchBytes(), 0u);  // planned at load
  EXPECT_EQ(0u, set->releaseMissMemory());
  const uint32_t drawn = glyph_miss::drawnCount();
  expectDrawnAs(reference(files[kRegular].bytes, 'Q', 14), *set->font(0), kRegular, set->font(0)->glyph(kRegular, 'Q'));
  EXPECT_EQ(drawn + 1, glyph_miss::drawnCount());
  // Held on the heap for the misses after it, in either family, until the release.
  expectDrawnAs(reference(accent[kRegular].bytes, 'Q', 14), *set->font(1), kRegular,
                set->font(1)->glyph(kRegular, 'Q'));
  EXPECT_EQ(set->scratchBytes(), set->releaseMissMemory());  // flash mode: the scratch block alone, no ring
  EXPECT_EQ(0u, set->releaseMissMemory());
  // Made again by the next miss, and the glyphs stored meanwhile need none.
  expectDrawnAs(reference(files[kRegular].bytes, 'Q', 14), *set->font(0), kRegular, set->font(0)->glyph(kRegular, 'Q'));
  EXPECT_EQ(0u, set->releaseMissMemory());
  expectDrawnAs(reference(files[kRegular].bytes, 'z', 14), *set->font(0), kRegular, set->font(0)->glyph(kRegular, 'z'));
  EXPECT_EQ(set->scratchBytes(), set->releaseMissMemory());
}

TEST_F(TtfFontSetTest, AMissInALentArenaDrawsThereAndGivesItBack) {
  const auto set = openSet();
  ASSERT_NE(nullptr, set);
  BuildArena arena(48 * 1024);
  ASSERT_TRUE(arena.valid());
  // What the arena's owner holds, as a section build holds its page block while it lays a line out.
  BuildArena::Block page = arena.reserveBlock();
  auto* line = static_cast<uint8_t*>(arena.alloc(100));
  ASSERT_NE(nullptr, line);
  std::fill_n(line, 100, uint8_t{0x5A});
  const size_t used = arena.used();
  {
    const glyph_miss::Scope misses(&arena);
    expectDrawnAs(reference(files[kRegular].bytes, 'W', 14), *set->font(0), kRegular,
                  set->font(0)->glyph(kRegular, 'W'));
    set->font(1)->prefetch(kRegular, "Wavy");
    EXPECT_EQ(4u, set->font(1)->renders());
    EXPECT_FALSE(misses.starved());
    {
      const glyph_miss::Scope heap(nullptr);  // nested: none lent
      expectDrawnAs(reference(files[kRegular].bytes, 'v', 14), *set->font(0), kRegular,
                    set->font(0)->glyph(kRegular, 'v'));
    }
    EXPECT_EQ(&arena, glyph_miss::arena());
  }
  EXPECT_EQ(nullptr, glyph_miss::arena());
  EXPECT_GE(arena.highWater(), used + set->scratchBytes());  // drawn in the arena
  EXPECT_EQ(used, arena.used());                             // and given back
  EXPECT_TRUE(std::all_of(line, line + 100, [](const uint8_t b) { return b == 0x5A; }));
  EXPECT_TRUE(arena.release(page));  // the owner's block is still the newest
  // Only the nested lookup outside the arena made its block on the heap.
  EXPECT_EQ(set->scratchBytes(), set->releaseMissMemory());
}

TEST_F(TtfFontSetTest, AnArenaWithoutRoomLeavesTheMissToTheHeap) {
  const auto set = openSet();
  ASSERT_NE(nullptr, set);
  BuildArena small(1024);
  {
    const glyph_miss::Scope misses(&small);
    expectDrawnAs(reference(files[kRegular].bytes, 'M', 14), *set->font(0), kRegular,
                  set->font(0)->glyph(kRegular, 'M'));
  }
  EXPECT_EQ(0u, small.used());
  EXPECT_EQ(0u, small.highWater());
  EXPECT_EQ(0u, small.failedAllocSize());  // never asked: its log would say its owner ran out
  EXPECT_EQ(set->scratchBytes(), set->releaseMissMemory());
}

TEST_F(TtfFontSetTest, AMissThatFindsNoMemoryIsDrawnAsMissingCountedAndAskedAgain) {
  const auto set = openSet();
  ASSERT_NE(nullptr, set);
  refuseArrayBytes = set->scratchBytes();
  const uint32_t renders = set->font(0)->renders();
  {
    const glyph_miss::Scope misses(nullptr);
    set->font(0)->prefetch(kRegular, "Kite");  // draws nothing, and says nothing: the lookups ask again
    EXPECT_FALSE(misses.starved());
    EXPECT_EQ(nullptr, set->font(0)->glyph(kRegular, 'K'));
    EXPECT_TRUE(misses.starved());
  }
  EXPECT_EQ(renders, set->font(0)->renders());
  EXPECT_EQ(0u, set->releaseMissMemory());
  // Not marked missing in the store: drawn once the memory is there.
  refuseArrayBytes = 0;
  const uint32_t starved = glyph_miss::starvedCount();
  expectDrawnAs(reference(files[kRegular].bytes, 'K', 14), *set->font(0), kRegular, set->font(0)->glyph(kRegular, 'K'));
  EXPECT_EQ(starved, glyph_miss::starvedCount());
  EXPECT_EQ(renders + 1, set->font(0)->renders());
}

TEST_F(TtfFontSetTest, WhatTheAppCanSpareIsFreedOnceBeforeAMissGoesWithout) {
  const auto set = openSet();
  ASSERT_NE(nullptr, set);
  static int reclaims = 0;
  static bool frees = false;
  reclaims = 0;
  frees = true;
  TtfFont::setHooks({nullptr, nullptr, [] {
                       ++reclaims;
                       if (frees) refuseArrayBytes = 0;  // the built-in font caches given back
                       return frees;
                     }});
  const Reference r = reference(files[kRegular].bytes, 'R', 14);  // before any refusal
  refuseArrayBytes = set->scratchBytes();
  expectDrawnAs(r, *set->font(0), kRegular, set->font(0)->glyph(kRegular, 'R'));
  EXPECT_EQ(1, reclaims);
  EXPECT_EQ(set->scratchBytes(), set->releaseMissMemory());
  // Nothing to spare: asked once, and the miss goes without.
  frees = false;
  refuseArrayBytes = set->scratchBytes();
  const uint32_t starved = glyph_miss::starvedCount();
  EXPECT_EQ(nullptr, set->font(0)->glyph(kRegular, 'S'));
  EXPECT_EQ(2, reclaims);
  EXPECT_EQ(starved + 1, glyph_miss::starvedCount());
  // The rest of the pass goes without at once, with nothing asked of the heap or the app; the next pass asks again.
  refuseArrayBytes = 0;
  EXPECT_EQ(nullptr, set->font(0)->glyph(kRegular, 'T'));
  EXPECT_EQ(2, reclaims);
  EXPECT_EQ(starved + 2, glyph_miss::starvedCount());
  EXPECT_EQ(0u, set->releaseMissMemory());
  const Reference t = reference(files[kRegular].bytes, 'T', 14);
  expectDrawnAs(t, *set->font(0), kRegular, set->font(0)->glyph(kRegular, 'T'));
}

TEST_F(TtfFontSetTest, ASetLoadedAgainWithoutAnInstallOpensTheStoreAsItIsOrNotAtAll) {
  // The ladder's heavier step unloads the set for a decode and loads the same one again: cheap, as the store holds
  // it; and never an install, which would erase the store under the page being drawn.
  {
    const auto set = openSet();
    ASSERT_NE(nullptr, set);
    ASSERT_NE(nullptr, set->font(1)->glyph(kRegular, 'A'));
  }
  ttf::FileSource sources[2][4];
  for (int s = 0; s < 4; ++s) {
    if (!files[s].bytes.empty()) sources[0][s] = files[s].source();
    if (!accent[s].bytes.empty()) sources[1][s] = accent[s].source();
  }
  const uint32_t erases = flash.totalSectorErases();
  {
    TtfFontSet again(flash);
    ASSERT_TRUE(again.load(sources, 2, kIdentitySet, [this] { ++busy; }, TtfFontSet::Install::Never));
    ASSERT_TRUE(again.font(1)->setBodySize(14));
    expectDrawnAs(reference(accent[kRegular].bytes, 'A', 14), *again.font(1), kRegular,
                  again.font(1)->glyph(kRegular, 'A'));
    EXPECT_EQ(0u, again.font(1)->renders());  // from the store
  }
  // Another set, here the body alone, is refused, and the store keeps what it holds.
  {
    TtfFontSet other(flash);
    EXPECT_FALSE(other.load(sources, 1, kIdentityA, [this] { ++busy; }, TtfFontSet::Install::Never));
  }
  EXPECT_EQ(1, busy);
  EXPECT_EQ(erases, flash.totalSectorErases());
  TtfFontSet same(flash);
  EXPECT_TRUE(same.load(sources, 2, kIdentitySet, {}, TtfFontSet::Install::Never));
}

TEST_F(TtfFontSetTest, AnSdModeMissMakesItsPageCacheWhereItMakesTheScratchBlock) {
  SimNorFlash chip{kSdOnlyBytes};
  const auto set = openFamilies(chip, {files}, busy);
  ASSERT_NE(nullptr, set);
  ASSERT_TRUE(set->readsCard());
  EXPECT_EQ(0u, set->releaseMissMemory());  // nothing made at load, the page cache neither
  // In a lent arena, the page cache with the scratch block, both given back.
  BuildArena arena(48 * 1024);
  {
    const glyph_miss::Scope misses(&arena);
    expectDrawnAs(reference(files[kRegular].bytes, 'A', 14), *set->font(0), kRegular,
                  set->font(0)->glyph(kRegular, 'A'));
  }
  EXPECT_GE(arena.highWater(), set->scratchBytes() + sizeof(ttf::SdPageCache));
  EXPECT_EQ(0u, arena.used());
  EXPECT_EQ(0u, set->releaseMissMemory());
  const uint32_t pagesRead = set->sdPageReads();
  EXPECT_GT(pagesRead, 0u);  // kept count of when the cache went
  // On the heap, until the release, the ring with them once the store stops taking glyphs.
  ASSERT_NE(nullptr, set->font(0)->glyph(kRegular, 'B'));
  chip.failNextProgram();
  ASSERT_NE(nullptr, set->font(0)->glyph(kRegular, 'C'));
  ASSERT_TRUE(set->ringMode());
  ASSERT_GT(set->ringBytes(), 0u);
  const uint32_t ring = set->ringBytes();
  EXPECT_EQ(set->scratchBytes() + sizeof(ttf::SdPageCache) + ring, set->releaseMissMemory());
  EXPECT_EQ(0u, set->ringBytes());
  EXPECT_GT(set->sdPageReads(), pagesRead);
  // Made again for the next miss, which reads its outline from the card through it.
  for (MemoryFile& file : files) file.reads = 0;
  expectDrawnAs(reference(files[kRegular].bytes, 'g', 14), *set->font(0), kRegular, set->font(0)->glyph(kRegular, 'g'));
  EXPECT_GT(files[kRegular].reads, 0u);
}
