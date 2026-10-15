// FontCacheManager::prewarmCache finds a page's font through the renderer's lookup, so a face the font resolver
// makes on demand (a TTF family's native size) is prewarmed even before anything looked it up.
#include <gtest/gtest.h>

#include <map>
#include <string>

#include "EpdFont.h"
#include "EpdFontFamily.h"
#include "FontCacheManager.h"
#include "GlyphProvider.h"

namespace {

struct CountingProvider final : GlyphProvider {
  int prefetches = 0;
  std::string text;
  const EpdGlyph* missGlyph(uint32_t) override { return nullptr; }
  void prefetch(const char* utf8Text) override {
    ++prefetches;
    text = utf8Text;
  }
};

struct Fixture : testing::Test {
  CountingProvider provider;
  EpdFontData data{};
  EpdFont font{&data};
  EpdFontFamily family{&font, nullptr, nullptr, nullptr};
  std::map<int, EpdFontFamily> fontMap;
  const std::map<int, SdCardFont*> noSdFonts;
  FontCacheManager manager{fontMap, noSdFonts, noSdFonts};
  int asked = 0;

  void SetUp() override { data.provider = &provider; }
};

}  // namespace

TEST_F(Fixture, AFontOnlyTheLookupSuppliesIsPrewarmed) {
  manager.setFontLookup(
      [](const void* ctx, const int fontId) -> const EpdFontFamily* {
        auto* self = static_cast<Fixture*>(const_cast<void*>(ctx));
        ++self->asked;
        return fontId == 4711 ? &self->family : nullptr;
      },
      this);
  manager.prewarmCache(4711, "Chapter One", 0x01);
  EXPECT_EQ(1, asked);
  EXPECT_EQ(1, provider.prefetches);
  EXPECT_EQ("Chapter One", provider.text);
  manager.prewarmCache(4712, "nothing", 0x01);  // a font nobody knows: nothing to do
  EXPECT_EQ(1, provider.prefetches);
}

TEST_F(Fixture, WithoutALookupTheFontMapServes) {
  fontMap.emplace(7, family);
  manager.prewarmCache(7, "Body", 0x01);
  EXPECT_EQ(1, provider.prefetches);
  manager.prewarmCache(4711, "Chapter One", 0x01);
  EXPECT_EQ(1, provider.prefetches);
}
