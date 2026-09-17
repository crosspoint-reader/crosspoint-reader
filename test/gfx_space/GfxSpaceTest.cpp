#include <GfxRenderer.h>
#include <HalStorage.h>
#include <SdCardFont.h>
#include <gtest/gtest.h>

namespace {
constexpr int FONT_ID = -1;

void put16(size_t at, uint16_t value) {
  sdFontTestFile[at] = value;
  sdFontTestFile[at + 1] = value >> 8;
}

void put32(size_t at, uint32_t value) {
  put16(at, value);
  put16(at + 2, value >> 16);
}

class GfxSpaceTest : public testing::Test {
 protected:
  HalDisplay display;
  SdCardFont font;
  GfxRenderer renderer{display};

  void SetUp() override {
    // v4 font with only space: regular advances 10.5 px, bold advances zero.
    constexpr size_t DATA_OFFSET = 32 + 2 * 32;
    constexpr size_t STYLE_BYTES = sizeof(EpdUnicodeInterval) + sizeof(EpdGlyph);
    sdFontTestFile.assign(DATA_OFFSET + 2 * STYLE_BYTES, 0);
    std::memcpy(sdFontTestFile.data(), "CPFONT\0\0", 8);
    put16(8, CPFONT_VERSION);
    sdFontTestFile[12] = 2;
    for (uint8_t style = 0; style < 2; ++style) {
      const size_t toc = 32 + style * 32;
      const size_t data = DATA_OFFSET + style * STYLE_BYTES;
      sdFontTestFile[toc] = style;
      put32(toc + 4, 1);  // interval count
      put32(toc + 8, 1);  // glyph count
      sdFontTestFile[toc + 12] = 16;
      put16(toc + 13, 16);
      put32(toc + 24, data);
      put32(data, ' ');
      put32(data + 4, ' ');
      EpdGlyph glyph{};
      glyph.advanceX = style == 0 ? 168 : 0;
      std::memcpy(sdFontTestFile.data() + data + sizeof(EpdUnicodeInterval), &glyph, sizeof(glyph));
    }
    ASSERT_TRUE(font.load("fixture"));
    renderer.insertFont(FONT_ID, EpdFontFamily(font.getEpdFont(0), font.getEpdFont(1)));
    renderer.registerSdCardFont(FONT_ID, &font);
  }

  void expectSpace(int pixels, EpdFontFamily::Style style) {
    EXPECT_EQ(pixels, renderer.getSpaceWidth(FONT_ID, style));
    EXPECT_EQ(pixels, renderer.getSpaceAdvance(FONT_ID, 'a', 'b', style));
  }
};

TEST_F(GfxSpaceTest, OtherStylesTableDoesNotHideAnUncachedStyle) {
  expectSpace(11, EpdFontFamily::REGULAR);
  font.clearCache();  // require the fallback to read the glyph again
  ASSERT_EQ(0, font.buildAdvanceTable(" ", 1 << EpdFontFamily::BOLD));
  ASSERT_TRUE(font.hasAdvanceTable());
  sdFontTestReads = 0;
  expectSpace(11, EpdFontFamily::REGULAR);
  EXPECT_GT(sdFontTestReads, 0U);
}

TEST_F(GfxSpaceTest, CachedZeroIsAHitAndDoesNotReadTheGlyph) {
  ASSERT_EQ(0, font.buildAdvanceTable(" ", 1 << EpdFontFamily::BOLD));
  sdFontTestReads = 0;
  expectSpace(0, EpdFontFamily::BOLD);
  EXPECT_EQ(0U, sdFontTestReads);
}
}  // namespace
