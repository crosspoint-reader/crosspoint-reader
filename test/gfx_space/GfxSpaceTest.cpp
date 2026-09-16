#include <GfxRenderer.h>
#include <HalStorage.h>
#include <SdCardFont.h>
#include <gtest/gtest.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <new>

namespace {
int failArrayAfter = -1;
}  // namespace

// Fail one checked allocation during metrics preparation, then allow fallback.
void* operator new[](size_t size) {
  void* allocation = std::malloc(size == 0 ? 1 : size);
  if (!allocation) {
    std::fputs("Unexpected host OOM in GfxSpaceTest\n", stderr);
    std::exit(EXIT_FAILURE);
  }
  return allocation;
}

void* operator new[](size_t size, const std::nothrow_t&) noexcept {
  if (failArrayAfter >= 0 && failArrayAfter-- == 0) return nullptr;
  return std::malloc(size == 0 ? 1 : size);
}

void operator delete[](void* allocation) noexcept { std::free(allocation); }
void operator delete[](void* allocation, size_t) noexcept { std::free(allocation); }
void operator delete[](void* allocation, const std::nothrow_t&) noexcept { std::free(allocation); }

namespace {
constexpr int FONT_ID = -1;
constexpr uint32_t FIRST = 0x4E00;
constexpr uint32_t CACHE_ENTRIES = 768;
constexpr uint32_t ASCII_GLYPHS = 95;
constexpr uint32_t GLYPHS = ASCII_GLYPHS + CACHE_ENTRIES + 1;
constexpr size_t STYLE_BYTES = 36 + GLYPHS * sizeof(EpdGlyph);

void put16(size_t at, uint16_t value) {
  sdFontTestFile[at] = value;
  sdFontTestFile[at + 1] = value >> 8;
}

void put32(size_t at, uint32_t value) {
  put16(at, value);
  put16(at + 2, value >> 16);
}

// Adapted from the audit font probe: valid v4 font, no kerning/ligatures,
// enough non-space glyphs to fill the advance cache, and distinct style metrics.
void makeFont(uint16_t regularSpace = 128, uint16_t boldSpace = 168) {
  constexpr size_t DATA_OFFSET = 32 + 2 * 32;
  sdFontTestFile.assign(DATA_OFFSET + 2 * STYLE_BYTES, 0);
  std::memcpy(sdFontTestFile.data(), "CPFONT\0\0", 8);
  put16(8, CPFONT_VERSION);
  sdFontTestFile[12] = 2;
  for (uint8_t style = 0; style < 2; ++style) {
    const size_t toc = 32 + style * 32;
    const size_t data = DATA_OFFSET + style * STYLE_BYTES;
    sdFontTestFile[toc] = style;
    put32(toc + 4, 3);
    put32(toc + 8, GLYPHS);
    sdFontTestFile[toc + 12] = 16;
    put16(toc + 13, 16);
    put32(toc + 24, data);
    put32(data, ' ');
    put32(data + 4, '~');
    put32(data + 12, FIRST);
    put32(data + 16, FIRST + CACHE_ENTRIES - 1);
    put32(data + 20, ASCII_GLYPHS);
    put32(data + 24, 0xFFFD);
    put32(data + 28, 0xFFFD);
    put32(data + 32, GLYPHS - 1);
    for (uint32_t i = 0; i < GLYPHS; ++i) {
      EpdGlyph glyph{};
      glyph.advanceX = i == 0 ? (style == 0 ? regularSpace : boldSpace) : 128;
      std::memcpy(sdFontTestFile.data() + data + 36 + i * sizeof(glyph), &glyph, sizeof(glyph));
    }
  }
}

std::string fullCacheText() {
  std::string text;
  text.reserve(CACHE_ENTRIES * 3);
  for (uint32_t cp = FIRST; cp < FIRST + CACHE_ENTRIES; ++cp) {
    text.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    text.push_back(static_cast<char>(0x80 | ((cp >> 6) & 63)));
    text.push_back(static_cast<char>(0x80 | (cp & 63)));
  }
  return text;
}

class GfxSpaceTest : public testing::Test {
 protected:
  HalDisplay display;
  GfxRenderer renderer{display};
  SdCardFont font;

  void load(uint16_t regularSpace = 128, uint16_t boldSpace = 168) {
    makeFont(regularSpace, boldSpace);
    ASSERT_TRUE(font.load("fixture"));
    renderer.insertFont(FONT_ID, EpdFontFamily(font.getEpdFont(0), font.getEpdFont(1)));
    renderer.registerSdCardFont(FONT_ID, &font);
  }

  void TearDown() override { failArrayAfter = -1; }

  void expectSpace(int pixels, EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
    EXPECT_EQ(pixels, renderer.getSpaceWidth(FONT_ID, style));
    EXPECT_EQ(pixels, renderer.getSpaceAdvance(FONT_ID, 'a', 'b', style));
  }

  // Representative greedy wrapping using the same word + contextual gap
  // measurements as ParsedText. Records the last word index of each line.
  std::vector<size_t> lineEnds(int lineWidth, EpdFontFamily::Style style) {
    constexpr std::array<const char*, 5> WORDS = {"aa", "bb", "cc", "dd", "ee"};
    std::vector<size_t> ends;
    ends.reserve(WORDS.size());
    int width = 0;
    for (size_t i = 0; i < WORDS.size(); ++i) {
      const int word = renderer.getTextAdvanceX(FONT_ID, WORDS[i], style);
      const int gap = i == 0 ? 0 : renderer.getSpaceAdvance(FONT_ID, WORDS[i - 1][1], WORDS[i][0], style);
      if (i != 0 && width + gap + word > lineWidth) {
        ends.push_back(i - 1);
        width = word;
      } else {
        width += gap + word;
      }
    }
    ends.push_back(WORDS.size() - 1);
    return ends;
  }
};

TEST_F(GfxSpaceTest, FullCacheWithoutSpaceMatchesUncachedMeasurementsAndWrapping) {
  load();
  expectSpace(8);
  const auto uncachedLines = lineEnds(36, EpdFontFamily::REGULAR);
  EXPECT_EQ((std::vector<size_t>{0, 1, 2, 3, 4}), uncachedLines);
  font.clearCache();  // discard overflow glyphs so fallback must read SD
  ASSERT_EQ(0, font.buildAdvanceTable(fullCacheText().c_str(), 1));
  ASSERT_TRUE(font.hasAdvanceTable());
  sdFontTestReads = 0;
  renderer.ensureSdCardFontReady(FONT_ID, " ", 1);
  ASSERT_EQ(0U, sdFontTestReads);  // full table cannot accept space
  ASSERT_EQ(0, font.getAdvance(' ', 0));
  expectSpace(8);
  EXPECT_GT(sdFontTestReads, 0U);
  EXPECT_EQ(uncachedLines, lineEnds(36, EpdFontFamily::REGULAR));
}

TEST_F(GfxSpaceTest, OtherStylesTableDoesNotHideAnUncachedStyle) {
  load();
  expectSpace(11, EpdFontFamily::BOLD);  // 10.5 px rounds up
  const auto uncachedLines = lineEnds(40, EpdFontFamily::BOLD);
  font.clearCache();
  ASSERT_EQ(0, font.buildAdvanceTable(" ", 1));
  sdFontTestReads = 0;
  expectSpace(11, EpdFontFamily::BOLD);
  EXPECT_GT(sdFontTestReads, 0U);
  EXPECT_EQ(uncachedLines, lineEnds(40, EpdFontFamily::BOLD));
}

TEST_F(GfxSpaceTest, FailedMetricsPreparationFallsBackWithAnotherStyleCached) {
  load();
  ASSERT_EQ(0, font.buildAdvanceTable(" ", 1));
  // Scratch, mappings, staging and merge allocation failures are independent.
  for (int allocation = 0; allocation < 4; ++allocation) {
    SCOPED_TRACE(allocation);
    font.clearCache();
    failArrayAfter = allocation;
    renderer.ensureSdCardFontReady(FONT_ID, " ", 2);
    ASSERT_EQ(-1, failArrayAfter);
    ASSERT_EQ(0, font.getAdvance(' ', 1));
    sdFontTestReads = 0;
    expectSpace(11, EpdFontFamily::BOLD);
    EXPECT_GT(sdFontTestReads, 0U);
    EXPECT_EQ((std::vector<size_t>{0, 1, 2, 3, 4}), lineEnds(40, EpdFontFamily::BOLD));
  }
}

TEST_F(GfxSpaceTest, FailedInitialPreparationMatchesUncachedSpace) {
  load();
  failArrayAfter = 0;
  renderer.ensureSdCardFontReady(FONT_ID, " ", 1);
  ASSERT_EQ(-1, failArrayAfter);
  ASSERT_FALSE(font.hasAdvanceTable());
  expectSpace(8);
}

TEST_F(GfxSpaceTest, MissingStyleUsesTheSameResolvedVariantOnHitsAndMisses) {
  load();
  // No italic face: bold-italic resolves to bold, italic to regular.
  for (auto style : {EpdFontFamily::ITALIC, EpdFontFamily::BOLD_ITALIC}) {
    const int expected = style == EpdFontFamily::ITALIC ? 8 : 11;
    expectSpace(expected, style);
    font.clearCache();
    ASSERT_EQ(0, font.buildAdvanceTable("a", 3));
    expectSpace(expected, style);
    renderer.ensureSdCardFontReady(FONT_ID, " ", 1 << style);
    sdFontTestReads = 0;
    expectSpace(expected, style);
    EXPECT_EQ(0U, sdFontTestReads);
  }
}

TEST_F(GfxSpaceTest, CachedZeroIsAHitAndDoesNotReadTheGlyph) {
  load(0);
  ASSERT_EQ(0, font.buildAdvanceTable(" ", 1));
  sdFontTestReads = 0;
  expectSpace(0);
  EXPECT_EQ(0U, sdFontTestReads);
}

TEST_F(GfxSpaceTest, FractionalSpaceRoundsTheSameWithAndWithoutCaching) {
  for (uint16_t advance : {127, 128, 135, 136, 143, 144}) {
    SCOPED_TRACE(advance);
    renderer.removeFont(FONT_ID);
    load(advance);
    const int expected = fp4::toPixel(advance);
    expectSpace(expected);
    ASSERT_EQ(0, font.buildAdvanceTable("a", 1));
    expectSpace(expected);
    ASSERT_EQ(0, font.buildAdvanceTable(" ", 1));
    sdFontTestReads = 0;
    expectSpace(expected);
    EXPECT_EQ(0U, sdFontTestReads);
  }
}
}  // namespace
