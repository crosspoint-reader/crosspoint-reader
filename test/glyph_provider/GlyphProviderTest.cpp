// EpdFont's side of GlyphProvider: who is asked for a glyph the tables lack, and for kerning.
#include <gtest/gtest.h>

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "EpdFont.h"
#include "EpdFontFamily.h"
#include "GlyphProvider.h"

namespace {

class FakeProvider final : public GlyphProvider {
 public:
  std::map<uint32_t, EpdGlyph> glyphs;
  std::vector<uint32_t> asked;
  std::vector<std::pair<uint32_t, uint32_t>> kernAsked;
  int8_t kernValue = 0;

  const EpdGlyph* missGlyph(const uint32_t codepoint) override {
    asked.push_back(codepoint);
    const auto it = glyphs.find(codepoint);
    return it == glyphs.end() ? nullptr : &it->second;
  }
  int8_t kerning(const uint32_t leftCp, const uint32_t rightCp) override {
    kernAsked.emplace_back(leftCp, rightCp);
    return kernValue;
  }
  std::vector<std::string> prefetched;
  void prefetch(const char* utf8Text) override { prefetched.emplace_back(utf8Text); }
};

EpdGlyph makeGlyph(const uint8_t width, const uint8_t height, const uint16_t advanceX) {
  EpdGlyph g{};
  g.width = width;
  g.height = height;
  g.advanceX = advanceX;
  g.left = 1;
  g.top = 12;
  g.dataLength = glyphDataBytes(width, height, true);
  g.dataOffset = 40;
  return g;
}

// No interval table, no glyph array, no kern tables: every lookup has to go to the provider.
EpdFontData providerOnly(GlyphProvider* provider) {
  EpdFontData d{};
  d.advanceY = 20;
  d.ascender = 16;
  d.descender = -4;
  d.is2Bit = true;
  d.provider = provider;
  return d;
}

constexpr uint32_t kReplacement = 0xFFFD;

}  // namespace

TEST(GlyphProvider, EveryLookupGoesToTheProviderWhenTheFontHasNoIntervals) {
  FakeProvider provider;
  provider.glyphs['A'] = makeGlyph(9, 12, 10 << 4);
  const EpdFontData data = providerOnly(&provider);
  const EpdFont font(&data);

  const EpdGlyphRef glyph = font.getGlyph('A');

  ASSERT_TRUE(glyph);
  EXPECT_EQ(&provider.glyphs['A'], glyph.sdRecord);
  EXPECT_EQ(10 << 4, glyph.advanceX);
  EXPECT_EQ(9, glyph.width);
  EXPECT_EQ(12, glyph.height);
  EXPECT_EQ(1, glyph.left);
  EXPECT_EQ(12, glyph.top);
  EXPECT_EQ(40u, epdGlyphBitmapOffset(&data, glyph));
  EXPECT_EQ(std::vector<uint32_t>{'A'}, provider.asked);
}

TEST(GlyphProvider, AMissingGlyphFallsBackToTheReplacementThroughTheProvider) {
  FakeProvider provider;
  provider.glyphs[kReplacement] = makeGlyph(8, 8, 9 << 4);
  const EpdFontData data = providerOnly(&provider);
  const EpdFont font(&data);

  const EpdGlyphRef glyph = font.getGlyph('Z');

  ASSERT_TRUE(glyph);
  EXPECT_EQ(&provider.glyphs[kReplacement], glyph.sdRecord);
  ASSERT_FALSE(provider.asked.empty());
  EXPECT_EQ('Z', provider.asked.front());
  EXPECT_EQ(kReplacement, provider.asked.back());
}

TEST(GlyphProvider, NoGlyphAnywhereGivesAnInvalidRef) {
  FakeProvider provider;
  const EpdFontData data = providerOnly(&provider);
  const EpdFont font(&data);

  EXPECT_FALSE(font.getGlyph('Z'));
}

TEST(GlyphProvider, KerningAsksTheProviderWhenTheFontHasNoKernTables) {
  FakeProvider provider;
  provider.kernValue = -3;
  const EpdFontData data = providerOnly(&provider);
  const EpdFont font(&data);

  EXPECT_EQ(-3, font.getKerning('A', 'V'));
  ASSERT_EQ(1u, provider.kernAsked.size());
  EXPECT_EQ(std::make_pair(uint32_t{'A'}, uint32_t{'V'}), provider.kernAsked.front());
}

TEST(GlyphProvider, KerningUsesTablesWhenTheFontHasThem) {
  static const EpdKernClassEntry kLeft[] = {{'A', 1}};
  static const EpdKernClassEntry kRight[] = {{'V', 1}};
  static const int8_t kMatrix[] = {-5};
  FakeProvider provider;
  provider.kernValue = 7;
  EpdFontData data = providerOnly(&provider);
  data.kernLeftClasses = kLeft;
  data.kernRightClasses = kRight;
  data.kernMatrix = kMatrix;
  data.kernLeftEntryCount = 1;
  data.kernRightEntryCount = 1;
  data.kernLeftClassCount = 1;
  data.kernRightClassCount = 1;
  const EpdFont font(&data);

  EXPECT_EQ(-5, font.getKerning('A', 'V'));
  EXPECT_EQ(0, font.getKerning('V', 'A'));  // not in the tables: 0, NOT the provider's 7
  EXPECT_TRUE(provider.kernAsked.empty());
}

// CJK text is set on a grid: no pair touching a CJK character kerns, whatever the font's tables or
// provider say, and the provider is never asked (a TTF family would fill its pair store with zeros).
TEST(GlyphProvider, NoPairTouchingCjkKerns) {
  static const EpdKernClassEntry kLeft[] = {{0x6F22, 1}};   // 漢
  static const EpdKernClassEntry kRight[] = {{0x5B57, 1}};  // 字
  static const int8_t kMatrix[] = {-5};
  FakeProvider provider;
  provider.kernValue = -3;
  const EpdFontData providerData = providerOnly(&provider);
  const EpdFont providerFont(&providerData);
  EXPECT_EQ(0, providerFont.getKerning(0x6F22, 0x5B57));
  EXPECT_EQ(0, providerFont.getKerning('A', 0x3002));  // A。
  EXPECT_TRUE(provider.kernAsked.empty());
  EXPECT_EQ(-3, providerFont.getKerning('A', 'V'));  // Latin pairs still ask

  EpdFontData tableData = providerOnly(nullptr);
  tableData.kernLeftClasses = kLeft;
  tableData.kernRightClasses = kRight;
  tableData.kernMatrix = kMatrix;
  tableData.kernLeftEntryCount = 1;
  tableData.kernRightEntryCount = 1;
  tableData.kernLeftClassCount = 1;
  tableData.kernRightClassCount = 1;
  const EpdFont tableFont(&tableData);
  EXPECT_EQ(0, tableFont.getKerning(0x6F22, 0x5B57));  // the table has the pair; it is not used
}

TEST(GlyphProvider, AFontWithoutAProviderBehavesAsBefore) {
  const EpdFontData data{};
  const EpdFont font(&data);

  EXPECT_FALSE(font.getGlyph('A'));
  EXPECT_EQ(0, font.getKerning('A', 'V'));
}

TEST(GlyphProvider, PrefetchAsksEachStylesFontOnceAfterFallback) {
  FakeProvider regularProvider;
  FakeProvider boldProvider;
  const EpdFontData regularData = providerOnly(&regularProvider);
  const EpdFontData boldData = providerOnly(&boldProvider);
  const EpdFont regular(&regularData);
  const EpdFont bold(&boldData);
  const EpdFontFamily family(&regular, &bold);  // italic falls back to regular, bold italic to bold

  family.prefetch("Wolf", 0x0F);

  EXPECT_EQ(std::vector<std::string>{"Wolf"}, regularProvider.prefetched);
  EXPECT_EQ(std::vector<std::string>{"Wolf"}, boldProvider.prefetched);
}

TEST(GlyphProvider, PrefetchAsksOnlyTheStylesInTheMask) {
  FakeProvider regularProvider;
  FakeProvider boldProvider;
  const EpdFontData regularData = providerOnly(&regularProvider);
  const EpdFontData boldData = providerOnly(&boldProvider);
  const EpdFont regular(&regularData);
  const EpdFont bold(&boldData);
  const EpdFontFamily family(&regular, &bold);

  family.prefetch("Wolf", 1u << EpdFontFamily::BOLD);

  EXPECT_TRUE(regularProvider.prefetched.empty());
  EXPECT_EQ(std::vector<std::string>{"Wolf"}, boldProvider.prefetched);
}

TEST(GlyphProvider, PrefetchSkipsFontsWithoutAProvider) {
  FakeProvider boldProvider;
  const EpdFontData builtIn{};  // a built-in font has no provider
  const EpdFontData boldData = providerOnly(&boldProvider);
  const EpdFont regular(&builtIn);
  const EpdFont bold(&boldData);
  const EpdFontFamily family(&regular, &bold);

  family.prefetch("Wolf", 0x0F);

  EXPECT_EQ(std::vector<std::string>{"Wolf"}, boldProvider.prefetched);
}
