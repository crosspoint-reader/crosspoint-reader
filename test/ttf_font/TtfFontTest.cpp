// TtfFont: a TrueType family installed into the glyph store, its glyphs rendered once and then read from flash.
#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "EpdFontFamily.h"
#include "SimNorFlash.h"
#include "TtfBuilder.h"
#include "TtfFont.h"
#include "TtfFontTestUtil.h"
#include "TtfTestUtil.h"

using namespace ttf_test;

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

Reference reference(const std::vector<uint8_t>& file, const uint32_t cp, const uint8_t points) {
  FontBytes bytes(file.data(), static_cast<uint32_t>(file.size()));
  ttf::Face face;
  EXPECT_EQ(ttf::FaceStatus::Ok, ttf::parseFace(bytes, &face));
  ttf::Rasterizer rasterizer;
  EXPECT_TRUE(rasterizer.allocate(ttf::planScratch(face, ttf::ppemFromPoints(26), ttf::kPoolBytesFlash)));
  Reference ref;
  EXPECT_EQ(ttf::Status::Ok, rasterizer.render(bytes, face, ttf::glyphIndex(bytes, face, cp),
                                               ttf::ppemFromPoints(points), &ref.metrics));
  ref.bitmap.assign(rasterizer.output(), rasterizer.output() + ref.metrics.dataLength);
  return ref;
}

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
    EXPECT_EQ(0u, flash.violations());
    EXPECT_EQ(0u, flash.reprograms());
  }

  /// A TtfFont over the test's flash, loaded and set to `points`: what each boot sees. Null when it fails.
  std::unique_ptr<TtfFont> open(const uint8_t* identity = kIdentityA, const uint8_t points = 14) {
    auto font = std::make_unique<TtfFont>(flash);
    ttf::FileSource sources[4];
    for (int s = 0; s < 4; ++s) {
      if (!files[s].bytes.empty()) sources[s] = files[s].source();
    }
    if (!font->load(sources, identity, [this] { ++busy; })) return nullptr;
    if (points != 0 && !font->setBodySize(points)) return nullptr;
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

TEST_F(TtfFontTest, AFamilyWithoutARegularStyleIsRefused) {
  files[0].bytes.clear();
  EXPECT_EQ(nullptr, open());
  EXPECT_EQ(0u, flash.totalSectorErases());
}

TEST_F(TtfFontTest, AFamilyTooLargeForThePartitionIsRefusedBeforeAnyErase) {
  SimNorFlash small{1024 * 1024};
  TtfFont font(small);
  ttf::FileSource sources[4];
  for (int s = 0; s < 4; ++s) sources[s] = files[s].source();
  EXPECT_FALSE(font.load(sources, kIdentityA, [this] { ++busy; }));
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
  EXPECT_TRUE(font->wipeIfPending([this] { ++busy; }));
  EXPECT_EQ(1, busy);
  EXPECT_FALSE(font->ringMode());
  EXPECT_FALSE(font->wipeIfPending());  // nothing pending now
  const uint32_t renders = font->renders();
  const EpdGlyph* a = font->glyph(kRegular, 'A');  // the wipe emptied the store: rendered and stored again
  EXPECT_EQ(renders + 1, font->renders());
  const uint8_t* bitmap = nullptr;
  EXPECT_FALSE(font->ownsBitmap(a, bitmap));
  expectDrawnAs(reference(files[kRegular].bytes, 'A', 14), *font, kRegular, a);
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
  EXPECT_EQ(TtfFont::kPrefetchGlyphs, font->renders());
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
