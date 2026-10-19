// FallbackFaceProvider: the CJK UI faces serving what the Inter UI faces lack, through the same
// EpdFont lookup and bitmap hand-off the renderer uses.
//
// The CJK faces ship compressed (compress_ui_face.py), so the provider decodes their glyphs through
// a FontDecompressor; and the active UI language's faces are served flat, out of the language pack
// slot (CjkUiFonts). Both are exercised: a "flat" face here is a RAM copy of a compressed one that
// points at its inflated bitmap, which is exactly what CjkUiFonts builds over the mapped slot.
#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "EpdFont.h"
#include "FallbackFaceProvider.h"
#include "FontDecompressor.h"
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

using Bytes = std::vector<uint8_t>;

bool claimsHan(const uint32_t cp) { return cp >= 0x4E00 && cp <= 0x9FFF; }

// A UI face as CjkUiFonts::attach() leaves it: a RAM copy of the flash table, carrying the provider.
EpdFontData withProvider(const EpdFontData& face, GlyphProvider* provider) {
  EpdFontData copy = face;
  copy.provider = provider;
  return copy;
}

// What the renderer draws for a resolved glyph (GfxRenderer::getGlyphBitmap, uncompressed path),
// copied out at once: a decoded glyph lives only until the next decode.
Bytes drawn(const EpdFontData& font, const EpdGlyphRef& glyph) {
  const uint8_t* bitmap = &font.bitmap[epdGlyphBitmapOffset(&font, glyph)];
  if (font.provider && glyph.sdRecord) {
    const uint8_t* owned = nullptr;
    if (font.provider->ownsBitmap(glyph.sdRecord, owned)) bitmap = owned;
  }
  if (!bitmap) return {};
  return Bytes(bitmap, bitmap + glyphDataBytes(glyph.width, glyph.height, font.is2Bit));
}

// The bitmap compressed `face` holds for `cp` by its own tables, through a decompressor of its own.
Bytes faceBytes(const EpdFontData& face, const uint32_t cp) {
  static FontDecompressor reference;
  const EpdGlyphRef glyph = EpdFont(&face).findGlyph(cp);
  const uint8_t* bitmap = reference.getBitmap(&face, glyph, glyph.index);
  if (!bitmap) return {};
  return Bytes(bitmap, bitmap + glyphDataBytes(glyph.width, glyph.height, face.is2Bit));
}

// A compressed face inflated to the flat form the language pack slot holds: bitmap plus offsets.
struct FlatFace {
  Bytes bitmap;
  std::vector<uint16_t> offsets;
  EpdFontData data{};
};

bool appendTo(void* ctx, const uint8_t* bytes, const uint32_t len) {
  auto* out = static_cast<Bytes*>(ctx);
  out->insert(out->end(), bytes, bytes + len);
  return true;
}

void flatten(const EpdFontData& face, FlatFace& out) {
  Bytes ring(FontDecompressor::maxRingBytes(&face));
  Bytes glyph(FontDecompressor::HOT_GLYPH_BUF_SIZE);
  ASSERT_TRUE(FontDecompressor::inflateFace(&face, ring.data(), static_cast<uint32_t>(ring.size()), glyph.data(),
                                            &appendTo, &out.bitmap));
  const EpdFontGroup& last = face.groups[face.groupCount - 1];
  uint32_t offset = 0;
  for (uint32_t i = 0; i < last.firstGlyphIndex + last.glyphCount; ++i) {
    out.offsets.push_back(static_cast<uint16_t>(offset));
    const EpdGlyphRef g = epdResolveGlyph(&face, i);
    offset += glyphDataBytes(g.width, g.height, false);
  }
  out.data = face;
  out.data.bitmap = out.bitmap.data();
  out.data.bitmapOffsets = out.offsets.data();
  out.data.groups = nullptr;
  out.data.groupCount = 0;
}

class FallbackFaceProviderTest : public ::testing::Test {
 protected:
  void SetUp() override {
    provider.setDecompressor(&decompressor);
    const EpdFontData* faces[] = {&cjk_ui_zh_10, &cjk_ui_ja_10, &cjk_ui_ko_10};
    ASSERT_TRUE(provider.setFaces(faces, 3));
    ui = withProvider(inter_ui_10_regular, &provider);
  }
  FontDecompressor decompressor;
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
  const Bytes bitmap = drawn(ui, glyph);
  ASSERT_FALSE(bitmap.empty());
  EXPECT_EQ(bitmap, faceBytes(cjk_ui_zh_10, ZH_ONLY));
}

TEST_F(FallbackFaceProviderTest, LatinStaysWithTheUiFace) {
  const EpdGlyphRef glyph = EpdFont(&ui).getGlyph('A');
  ASSERT_TRUE(glyph);
  EXPECT_EQ(glyph.sdRecord, nullptr);
  const EpdGlyphRef own = EpdFont(&inter_ui_10_regular).findGlyph('A');
  const uint8_t* expected = inter_ui_10_regular.bitmap + epdGlyphBitmapOffset(&inter_ui_10_regular, own);
  EXPECT_EQ(drawn(ui, glyph), Bytes(expected, expected + glyphDataBytes(own.width, own.height, false)));
}

TEST_F(FallbackFaceProviderTest, LaterFacesServeWhatEarlierOnesLack) {
  const EpdGlyphRef kanji = EpdFont(&ui).getGlyph(JA_ONLY);
  ASSERT_TRUE(kanji);
  EXPECT_EQ(drawn(ui, kanji), faceBytes(cjk_ui_ja_10, JA_ONLY));
  const EpdGlyphRef hangul = EpdFont(&ui).getGlyph(HANGUL);
  ASSERT_TRUE(hangul);
  EXPECT_EQ(drawn(ui, hangul), faceBytes(cjk_ui_ko_10, HANGUL));
}

TEST_F(FallbackFaceProviderTest, TheFirstFaceWinsACharacterTwoFacesShare) {
  EXPECT_EQ(drawn(ui, EpdFont(&ui).getGlyph(ZH_AND_JA)), faceBytes(cjk_ui_zh_10, ZH_AND_JA));

  const EpdFontData* japaneseFirst[] = {&cjk_ui_ja_10, &cjk_ui_zh_10, &cjk_ui_ko_10};
  ASSERT_TRUE(provider.setFaces(japaneseFirst, 3));
  EXPECT_EQ(drawn(ui, EpdFont(&ui).getGlyph(ZH_AND_JA)), faceBytes(cjk_ui_ja_10, ZH_AND_JA));
  // The two regions' forms really differ, or the line above proves nothing.
  EXPECT_NE(faceBytes(cjk_ui_ja_10, ZH_AND_JA), faceBytes(cjk_ui_zh_10, ZH_AND_JA));
}

TEST_F(FallbackFaceProviderTest, ACharacterNoFaceHasStaysMissingWithoutAStandIn) {
  // The UI face has no U+FFFD either, so the lookup ends empty: nothing is drawn, as before.
  EXPECT_FALSE(EpdFont(&ui).getGlyph(IN_NO_FACE));
}

TEST_F(FallbackFaceProviderTest, AClaimedCharacterNoFaceHasDrawsAsTheStandIn) {
  provider.setStandIn(STAND_IN, &claimsHan);
  const EpdGlyphRef glyph = EpdFont(&ui).getGlyph(IN_NO_FACE);
  ASSERT_TRUE(glyph);
  EXPECT_EQ(drawn(ui, glyph), faceBytes(cjk_ui_zh_10, STAND_IN));
}

TEST_F(FallbackFaceProviderTest, AStandInLeavesCoveredAndUnclaimedCharactersAlone) {
  provider.setStandIn(STAND_IN, &claimsHan);
  EXPECT_EQ(drawn(ui, EpdFont(&ui).getGlyph(ZH_ONLY)), faceBytes(cjk_ui_zh_10, ZH_ONLY));
  EXPECT_FALSE(EpdFont(&ui).getGlyph(GREEK_ALPHA));
}

TEST_F(FallbackFaceProviderTest, ARecordStaysOwnedWhileLaterMissesFillTheRestOfTheRing) {
  const EpdGlyphRef first = EpdFont(&ui).getGlyph(ZH_ONLY);
  ASSERT_TRUE(first);
  const Bytes expected = faceBytes(cjk_ui_zh_10, ZH_ONLY);
  // Seven more misses fill the other slots of the eight-deep ring.
  for (const uint32_t cp : {JA_ONLY, HANGUL, ZH_AND_JA, JA_ONLY, HANGUL, ZH_AND_JA, JA_ONLY}) {
    ASSERT_TRUE(EpdFont(&ui).getGlyph(cp));
  }
  EXPECT_EQ(drawn(ui, first), expected);
}

TEST_F(FallbackFaceProviderTest, ARecordFromElsewhereIsNotOwned) {
  EpdGlyph foreign{};
  const uint8_t* bitmap = nullptr;
  EXPECT_FALSE(provider.ownsBitmap(&foreign, bitmap));
}

TEST_F(FallbackFaceProviderTest, AnIdeographicSpaceIsOwnedAndHasNoBitmap) {
  const EpdGlyphRef space = EpdFont(&ui).getGlyph(0x3000);
  ASSERT_TRUE(space);
  EXPECT_GT(space.advanceX, 0);
  const uint8_t* bitmap = reinterpret_cast<const uint8_t*>(1);
  EXPECT_TRUE(provider.ownsBitmap(space.sdRecord, bitmap));
  EXPECT_EQ(bitmap, nullptr);
}

TEST_F(FallbackFaceProviderTest, MeasuringDecodesNothing) {
  // Layout resolves every glyph of every row and draws none of them: only a drawn glyph may cost a
  // group decode.
  decompressor.resetStats();
  for (const uint32_t cp : {ZH_ONLY, JA_ONLY, HANGUL, ZH_AND_JA}) ASSERT_TRUE(EpdFont(&ui).getGlyph(cp));
  EXPECT_EQ(decompressor.getStats().getBitmapCalls, 0u);
}

TEST_F(FallbackFaceProviderTest, AFlatFaceIsServedStraightFromItsBitmap) {
  // The active UI language's face as CjkUiFonts serves it from the language pack slot.
  FlatFace zh;
  ASSERT_NO_FATAL_FAILURE(flatten(cjk_ui_zh_10, zh));
  const EpdFontData* faces[] = {&zh.data, &cjk_ui_ja_10, &cjk_ui_ko_10};
  ASSERT_TRUE(provider.setFaces(faces, 3));

  decompressor.resetStats();
  const EpdGlyphRef glyph = EpdFont(&ui).getGlyph(ZH_ONLY);
  ASSERT_TRUE(glyph);
  const uint8_t* owned = nullptr;
  ASSERT_TRUE(provider.ownsBitmap(glyph.sdRecord, owned));
  const EpdGlyphRef own = EpdFont(&zh.data).findGlyph(ZH_ONLY);
  EXPECT_EQ(owned, zh.bitmap.data() + zh.offsets[own.index]) << "a flat face is read in place";
  EXPECT_EQ(decompressor.getStats().getBitmapCalls, 0u) << "and costs no decode";
  EXPECT_EQ(drawn(ui, glyph), faceBytes(cjk_ui_zh_10, ZH_ONLY));
  // A face behind it still decodes.
  EXPECT_EQ(drawn(ui, EpdFont(&ui).getGlyph(JA_ONLY)), faceBytes(cjk_ui_ja_10, JA_ONLY));
}

TEST_F(FallbackFaceProviderTest, AGlyphThatCannotBeDecodedIsOwnedAndDrawsNothing) {
  // A stream that ends at once: what a decode looks like to the provider when it fails. It must
  // still claim the record, or the renderer would read the UI face's own bitmap at this offset.
  EpdFontData broken = cjk_ui_zh_10;
  std::vector<EpdFontGroup> groups(cjk_ui_zh_10.groups, cjk_ui_zh_10.groups + cjk_ui_zh_10.groupCount);
  for (EpdFontGroup& g : groups) g.compressedSize = 1;
  broken.groups = groups.data();
  const EpdFontData* faces[] = {&broken};
  ASSERT_TRUE(provider.setFaces(faces, 1));

  const EpdGlyphRef glyph = EpdFont(&ui).getGlyph(ZH_ONLY);
  ASSERT_TRUE(glyph);
  const uint8_t* bitmap = reinterpret_cast<const uint8_t*>(1);
  EXPECT_TRUE(provider.ownsBitmap(glyph.sdRecord, bitmap));
  EXPECT_EQ(bitmap, nullptr);
}

TEST(FallbackFaceProviderSetup, ACompressedFaceIsRefusedWithoutADecompressor) {
  FallbackFaceProvider provider;
  const EpdFontData* faces[] = {&cjk_ui_ja_10};
  EXPECT_FALSE(provider.setFaces(faces, 1));
  EXPECT_EQ(provider.missGlyph(JA_ONLY), nullptr);
}

TEST(FallbackFaceProviderSetup, MoreFacesThanItHoldsAreRefused) {
  FontDecompressor decompressor;
  FallbackFaceProvider provider;
  provider.setDecompressor(&decompressor);
  const EpdFontData* faces[FallbackFaceProvider::MAX_FACES + 1] = {};
  for (auto& face : faces) face = &cjk_ui_zh_10;
  EXPECT_FALSE(provider.setFaces(faces, FallbackFaceProvider::MAX_FACES + 1));
  EXPECT_EQ(provider.missGlyph(ZH_ONLY), nullptr);
}

}  // namespace
