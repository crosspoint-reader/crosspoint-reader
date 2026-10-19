// FallbackFaceProvider: the CJK UI faces serving what the Inter UI faces lack, through the same
// EpdFont lookup and bitmap hand-off the renderer uses.
#include <gtest/gtest.h>

#include "EpdFont.h"
#include "FallbackFaceProvider.h"
#include "builtinFonts/cjk_ui_ja_10.h"
#include "builtinFonts/cjk_ui_ko_10.h"
#include "builtinFonts/cjk_ui_zh_10.h"
#include "builtinFonts/inter_ui_10_regular.h"

namespace {

constexpr uint32_t ZH_ONLY = 0x8BBE;      // 设: in the Chinese set, not the Japanese
constexpr uint32_t ZH_AND_JA = 0x5B9A;    // 定: in both, with each region's glyph form
constexpr uint32_t JA_ONLY = 0x66F8;      // 書
constexpr uint32_t HANGUL = 0xC124;       // 설
constexpr uint32_t IN_NO_FACE = 0x9F98;   // 龘
constexpr uint32_t GREEK_ALPHA = 0x03B1;  // in no face, and no CJK face's to answer for
constexpr uint32_t STAND_IN = 0x25A1;     // □, which every CJK UI face carries

bool claimsHan(const uint32_t cp) { return cp >= 0x4E00 && cp <= 0x9FFF; }

// A UI face as CjkUiFonts::attach() leaves it: a RAM copy of the flash table, carrying the provider.
EpdFontData withProvider(const EpdFontData& face, GlyphProvider* provider) {
  EpdFontData copy = face;
  copy.provider = provider;
  return copy;
}

// What the renderer draws for a resolved glyph (GfxRenderer::getGlyphBitmap, uncompressed path).
const uint8_t* drawnBitmap(const EpdFontData& font, const EpdGlyphRef& glyph) {
  if (font.provider && glyph.sdRecord) {
    const uint8_t* owned = nullptr;
    if (font.provider->ownsBitmap(glyph.sdRecord, owned)) return owned;
  }
  return &font.bitmap[epdGlyphBitmapOffset(&font, glyph)];
}

// The bitmap `face` holds for `cp` by its own tables.
const uint8_t* faceBitmap(const EpdFontData& face, const uint32_t cp) {
  const EpdGlyphRef glyph = EpdFont(&face).findGlyph(cp);
  return face.bitmap + epdGlyphBitmapOffset(&face, glyph);
}

class FallbackFaceProviderTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const EpdFontData* faces[] = {&cjk_ui_zh_10, &cjk_ui_ja_10, &cjk_ui_ko_10};
    ASSERT_TRUE(provider.setFaces(faces, 3));
    ui = withProvider(inter_ui_10_regular, &provider);
  }
  FallbackFaceProvider provider;
  EpdFontData ui{};
};

TEST_F(FallbackFaceProviderTest, CjkGlyphComesFromTheCjkFaceWithItsMetricsAndBitmap) {
  const EpdGlyphRef glyph = EpdFont(&ui).getGlyph(ZH_ONLY);
  ASSERT_TRUE(glyph);
  const EpdGlyphRef own = EpdFont(&cjk_ui_zh_10).findGlyph(ZH_ONLY);
  EXPECT_EQ(glyph.width, own.width);
  EXPECT_EQ(glyph.height, own.height);
  EXPECT_EQ(glyph.advanceX, own.advanceX);
  EXPECT_EQ(glyph.left, own.left);
  EXPECT_EQ(glyph.top, own.top);
  EXPECT_EQ(drawnBitmap(ui, glyph), faceBitmap(cjk_ui_zh_10, ZH_ONLY));
}

TEST_F(FallbackFaceProviderTest, LatinStaysWithTheUiFace) {
  const EpdGlyphRef glyph = EpdFont(&ui).getGlyph('A');
  ASSERT_TRUE(glyph);
  EXPECT_EQ(glyph.sdRecord, nullptr);
  EXPECT_EQ(drawnBitmap(ui, glyph), faceBitmap(inter_ui_10_regular, 'A'));
}

TEST_F(FallbackFaceProviderTest, LaterFacesServeWhatEarlierOnesLack) {
  const EpdGlyphRef kanji = EpdFont(&ui).getGlyph(JA_ONLY);
  ASSERT_TRUE(kanji);
  EXPECT_EQ(drawnBitmap(ui, kanji), faceBitmap(cjk_ui_ja_10, JA_ONLY));
  const EpdGlyphRef hangul = EpdFont(&ui).getGlyph(HANGUL);
  ASSERT_TRUE(hangul);
  EXPECT_EQ(drawnBitmap(ui, hangul), faceBitmap(cjk_ui_ko_10, HANGUL));
}

TEST_F(FallbackFaceProviderTest, TheFirstFaceWinsACharacterTwoFacesShare) {
  EXPECT_EQ(drawnBitmap(ui, EpdFont(&ui).getGlyph(ZH_AND_JA)), faceBitmap(cjk_ui_zh_10, ZH_AND_JA));

  const EpdFontData* japaneseFirst[] = {&cjk_ui_ja_10, &cjk_ui_zh_10, &cjk_ui_ko_10};
  ASSERT_TRUE(provider.setFaces(japaneseFirst, 3));
  EXPECT_EQ(drawnBitmap(ui, EpdFont(&ui).getGlyph(ZH_AND_JA)), faceBitmap(cjk_ui_ja_10, ZH_AND_JA));
}

TEST_F(FallbackFaceProviderTest, ACharacterNoFaceHasStaysMissingWithoutAStandIn) {
  // The UI face has no U+FFFD either, so the lookup ends empty: nothing is drawn, as before.
  EXPECT_FALSE(EpdFont(&ui).getGlyph(IN_NO_FACE));
}

TEST_F(FallbackFaceProviderTest, AClaimedCharacterNoFaceHasDrawsAsTheStandIn) {
  provider.setStandIn(STAND_IN, &claimsHan);
  const EpdGlyphRef glyph = EpdFont(&ui).getGlyph(IN_NO_FACE);
  ASSERT_TRUE(glyph);
  EXPECT_EQ(drawnBitmap(ui, glyph), faceBitmap(cjk_ui_zh_10, STAND_IN));
}

TEST_F(FallbackFaceProviderTest, AStandInLeavesCoveredAndUnclaimedCharactersAlone) {
  provider.setStandIn(STAND_IN, &claimsHan);
  EXPECT_EQ(drawnBitmap(ui, EpdFont(&ui).getGlyph(ZH_ONLY)), faceBitmap(cjk_ui_zh_10, ZH_ONLY));
  EXPECT_FALSE(EpdFont(&ui).getGlyph(GREEK_ALPHA));
}

TEST_F(FallbackFaceProviderTest, ARecordStaysOwnedWhileLaterMissesFillTheRestOfTheRing) {
  const EpdGlyphRef first = EpdFont(&ui).getGlyph(ZH_ONLY);
  ASSERT_TRUE(first);
  const uint8_t* expected = faceBitmap(cjk_ui_zh_10, ZH_ONLY);
  // Seven more misses fill the other slots of the eight-deep ring.
  for (const uint32_t cp : {JA_ONLY, HANGUL, ZH_AND_JA, JA_ONLY, HANGUL, ZH_AND_JA, JA_ONLY}) {
    ASSERT_TRUE(EpdFont(&ui).getGlyph(cp));
  }
  EXPECT_EQ(drawnBitmap(ui, first), expected);
}

TEST_F(FallbackFaceProviderTest, ARecordFromElsewhereIsNotOwned) {
  EpdGlyph foreign{};
  const uint8_t* bitmap = nullptr;
  EXPECT_FALSE(provider.ownsBitmap(&foreign, bitmap));
}

TEST(FallbackFaceProviderSetup, ACompressedFaceIsRefusedAndNothingIsServed) {
  FallbackFaceProvider provider;
  EpdFontData compressed = cjk_ui_zh_10;
  const EpdFontGroup group{};
  compressed.groups = &group;
  compressed.groupCount = 1;
  const EpdFontData* faces[] = {&cjk_ui_ja_10, &compressed};
  EXPECT_FALSE(provider.setFaces(faces, 2));
  EXPECT_EQ(provider.missGlyph(JA_ONLY), nullptr);
}

TEST(FallbackFaceProviderSetup, MoreFacesThanItHoldsAreRefused) {
  FallbackFaceProvider provider;
  const EpdFontData* faces[FallbackFaceProvider::MAX_FACES + 1] = {};
  for (auto& face : faces) face = &cjk_ui_zh_10;
  EXPECT_FALSE(provider.setFaces(faces, FallbackFaceProvider::MAX_FACES + 1));
  EXPECT_EQ(provider.missGlyph(ZH_ONLY), nullptr);
}

}  // namespace
