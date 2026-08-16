// Host tests for UiGlyphCodec (ctx3 + binary range coder) and UiGlyphPool
// (bounded second-chance glyph cache).
//
// The corpus is synthetic: deterministic pseudo-glyphs spanning densities,
// stroke-like structure, and packing edge cases (odd widths, non-multiple-of-4
// pixel counts). Codec correctness does not depend on real font shapes; the
// compression-ratio measurements against real CJK fonts live with the feature
// PR notes, not here.

#include <gtest/gtest.h>

#include <cstring>
#include <map>
#include <vector>

#include "UiGlyphCodec.h"
#include "UiGlyphPool.h"

namespace {

struct TestGlyph {
  uint8_t w = 0, h = 0;
  std::vector<uint8_t> src2;  // packed 2-bit source (4 px/byte, MSB-first)
  std::vector<uint8_t> ref1;  // expected packed 1-bit conversion
};

uint32_t lcgState = 0x12345678;
uint32_t lcg() { return lcgState = lcgState * 1664525u + 1013904223u; }

// Build a glyph from a per-pixel 2-bit value generator.
template <typename Fn>
TestGlyph makeGlyph(uint8_t w, uint8_t h, Fn value) {
  TestGlyph g;
  g.w = w;
  g.h = h;
  const uint32_t pixels = static_cast<uint32_t>(w) * h;
  g.src2.assign((pixels + 3) / 4, 0);
  g.ref1.assign(UiGlyphCodec::packed1BitBytes(pixels), 0);
  for (uint32_t i = 0; i < pixels; i++) {
    const uint8_t v = value(i % w, i / w) & 0x3;
    g.src2[i >> 2] |= v << ((3 - (i & 3)) * 2);
    if (v != 0) g.ref1[i >> 3] |= 0x80 >> (i & 7);
  }
  return g;
}

std::vector<TestGlyph> syntheticCorpus() {
  std::vector<TestGlyph> corpus;
  // Stroke-like: vertical/horizontal bars with antialiased (gray) edges —
  // the structure the ctx3 model is trained for.
  for (uint8_t size : {7, 12, 13, 15, 24}) {
    corpus.push_back(makeGlyph(size, size, [](int x, int y) -> int {
      const bool barV = (x % 5) == 2;
      const bool barH = (y % 6) == 3;
      if (barV || barH) return 3;
      const bool edge = (x % 5) == 1 || (x % 5) == 3;
      return edge ? 1 : 0;
    }));
  }
  // Densities via deterministic noise, including incompressible worst cases.
  for (int densityPct : {3, 25, 50, 75, 97}) {
    corpus.push_back(makeGlyph(13, 15, [densityPct](int, int) -> int {
      return (lcg() % 100) < static_cast<uint32_t>(densityPct) ? (lcg() % 3) + 1 : 0;
    }));
  }
  // Edges: solid, empty, single pixel, extreme aspect ratios, odd sizes.
  corpus.push_back(makeGlyph(11, 14, [](int, int) { return 3; }));
  corpus.push_back(makeGlyph(9, 9, [](int, int) { return 0; }));
  corpus.push_back(makeGlyph(1, 1, [](int, int) { return 2; }));
  corpus.push_back(makeGlyph(1, 25, [](int, int y) { return y % 2 ? 3 : 0; }));
  corpus.push_back(makeGlyph(25, 1, [](int x, int) { return x % 3 ? 0 : 1; }));
  corpus.push_back(makeGlyph(3, 5, [](int x, int y) { return (x + y) % 2 ? 3 : 0; }));
  return corpus;
}

}  // namespace

TEST(UiGlyphCodec, RoundtripPixelExact) {
  uint8_t enc[UiGlyphPool::MAX_ENTRY_BYTES];
  uint8_t dec[UiGlyphPool::MAX_ENTRY_BYTES];
  int encoded = 0;
  for (const auto& g : syntheticCorpus()) {
    const uint16_t pixels = static_cast<uint16_t>(g.w) * g.h;
    const uint16_t rawLen = UiGlyphCodec::packed1BitBytes(pixels);
    ASSERT_LE(rawLen, sizeof(dec));
    const uint16_t encLen = UiGlyphCodec::encode(g.src2.data(), true, g.w, g.h, enc, rawLen ? rawLen - 1 : 0);
    if (encLen == 0) continue;  // stored raw by the pool — nothing to decode
    ASSERT_LT(encLen, rawLen);  // capacity contract: strictly smaller than raw
    memset(dec, 0xAA, sizeof(dec));
    UiGlyphCodec::decode(enc, encLen, g.w, g.h, dec);
    EXPECT_EQ(0, memcmp(dec, g.ref1.data(), rawLen)) << "glyph " << int(g.w) << "x" << int(g.h);
    encoded++;
  }
  EXPECT_GT(encoded, 5);  // structured glyphs must actually compress
}

TEST(UiGlyphCodec, OneBitSourceRoundtrip) {
  // 1-bit .cpfont sources take the srcIs2Bit=false path.
  const auto g = makeGlyph(13, 13, [](int x, int y) { return (x * y) % 4 == 1 ? 3 : 0; });
  uint8_t enc[64];
  uint8_t dec[32];
  const uint16_t rawLen = UiGlyphCodec::packed1BitBytes(13 * 13);
  const uint16_t encLen = UiGlyphCodec::encode(g.ref1.data(), false, 13, 13, enc, sizeof(enc));
  ASSERT_GT(encLen, 0);
  UiGlyphCodec::decode(enc, encLen, 13, 13, dec);
  EXPECT_EQ(0, memcmp(dec, g.ref1.data(), rawLen));
}

TEST(UiGlyphCodec, ConvertMatchesThreshold) {
  const auto g = makeGlyph(10, 7, [](int x, int y) { return (x + y) % 4; });
  uint8_t out[16];
  UiGlyphCodec::convertTo1Bit(g.src2.data(), true, 10, 7, out);
  EXPECT_EQ(0, memcmp(out, g.ref1.data(), UiGlyphCodec::packed1BitBytes(70)));
}

TEST(UiGlyphPool, InsertFindDecode) {
  UiGlyphPool pool;
  ASSERT_TRUE(pool.init(4096, 64));
  const auto corpus = syntheticCorpus();
  uint8_t dec[UiGlyphPool::MAX_ENTRY_BYTES];
  for (size_t i = 0; i < corpus.size(); i++) {
    const auto& g = corpus[i];
    UiGlyphPool::Metrics m{g.w, g.h, static_cast<uint16_t>(g.w << 4), -1, static_cast<int8_t>(g.h), 0, 0};
    ASSERT_TRUE(pool.insert(0, 0, 0xAC00 + i, m, g.src2.data(), true));
  }
  for (size_t i = 0; i < corpus.size(); i++) {
    const auto& g = corpus[i];
    const int32_t hnd = pool.find(0, 0, 0xAC00 + i);
    ASSERT_GE(hnd, 0);
    const auto& m = pool.metricsOf(hnd);
    EXPECT_EQ(m.width, g.w);
    EXPECT_EQ(m.height, g.h);
    const uint16_t rawLen = UiGlyphCodec::packed1BitBytes(static_cast<uint16_t>(g.w) * g.h);
    if (rawLen == 0) continue;
    ASSERT_TRUE(pool.copyBitmap(hnd, dec, sizeof(dec)));
    EXPECT_EQ(0, memcmp(dec, g.ref1.data(), rawLen));
  }
}

TEST(UiGlyphPool, EvictionFuzzServesExactContent) {
  UiGlyphPool pool;
  ASSERT_TRUE(pool.init(1024, 24));  // tiny: constant eviction
  const auto corpus = syntheticCorpus();
  std::map<uint32_t, size_t> shadow;
  uint8_t dec[UiGlyphPool::MAX_ENTRY_BYTES];
  for (int op = 0; op < 50000; op++) {
    const uint32_t r = lcg();
    const uint8_t inst = (r >> 8) % 3;
    const uint32_t cp = 0xAC00 + ((r >> 10) % 60);
    const uint32_t key = cp | (inst << 24);
    if ((r >> 28) == 15) {
      pool.reset();
      shadow.clear();
      continue;
    }
    if (r & 1) {
      const auto& g = corpus[r % corpus.size()];
      UiGlyphPool::Metrics m{g.w, g.h, 0, 0, 0, 0, 0};
      if (pool.find(inst, 0, cp) < 0 && pool.insert(inst, 0, cp, m, g.src2.data(), true)) {
        shadow[key] = r % corpus.size();
      }
    } else {
      const int32_t hnd = pool.find(inst, 0, cp);
      if (hnd >= 0) {
        ASSERT_TRUE(shadow.count(key));
        const auto& g = corpus[shadow[key]];
        ASSERT_EQ(pool.metricsOf(hnd).width, g.w);
        const uint16_t rawLen = UiGlyphCodec::packed1BitBytes(static_cast<uint16_t>(g.w) * g.h);
        if (rawLen > 0) {
          ASSERT_TRUE(pool.copyBitmap(hnd, dec, sizeof(dec)));
          ASSERT_EQ(0, memcmp(dec, g.ref1.data(), rawLen));
        }
      }
    }
    ASSERT_LE(pool.stats().storedBytes, 1024u);
  }
  EXPECT_GT(pool.stats().evictions, 0u);
  EXPECT_GT(pool.stats().rescues, 0u);
}

TEST(UiGlyphPool, SameStringFillSurvivesFullPool) {
  UiGlyphPool pool;
  ASSERT_TRUE(pool.init(1024, 32));
  const auto corpus = syntheticCorpus();
  // Fill with referenced entries, then fill a 8-glyph "string": all its
  // glyphs must be simultaneously resident for the blit phase.
  for (int i = 0; i < 30; i++) {
    const auto& g = corpus[(i * 5) % corpus.size()];
    UiGlyphPool::Metrics m{g.w, g.h, 0, 0, 0, 0, 0};
    pool.insert(1, 0, 0xB000 + i, m, g.src2.data(), true);
    pool.find(1, 0, 0xB000 + i);
  }
  for (int i = 0; i < 8; i++) {
    const auto& g = corpus[i % corpus.size()];
    UiGlyphPool::Metrics m{g.w, g.h, 0, 0, 0, 0, 0};
    if (pool.find(2, 0, 0xAC00 + i) < 0) {
      ASSERT_TRUE(pool.insert(2, 0, 0xAC00 + i, m, g.src2.data(), true));
    }
  }
  for (int i = 0; i < 8; i++) {
    EXPECT_GE(pool.find(2, 0, 0xAC00 + i), 0) << "string glyph " << i << " evicted before blit";
  }
}

TEST(UiGlyphPool, KernPairCache) {
  UiGlyphPool pool;
  ASSERT_TRUE(pool.init(512, 16));
  int8_t v = 99;
  EXPECT_FALSE(pool.kernPairLookup(0, 0, 23, 30, &v));
  pool.kernPairInsert(0, 0, 23, 30, -22);
  pool.kernPairInsert(0, 0, 50, 4, 0);    // zero value must be cached
  pool.kernPairInsert(1, 0, 23, 30, -7);  // same classes, other instance
  pool.kernPairInsert(0, 0, 23, 30, 42);  // duplicate: first value wins
  ASSERT_TRUE(pool.kernPairLookup(0, 0, 23, 30, &v));
  EXPECT_EQ(v, -22);
  ASSERT_TRUE(pool.kernPairLookup(0, 0, 50, 4, &v));
  EXPECT_EQ(v, 0);
  ASSERT_TRUE(pool.kernPairLookup(1, 0, 23, 30, &v));
  EXPECT_EQ(v, -7);
  EXPECT_FALSE(pool.kernPairLookup(0, 1, 23, 30, &v));  // other style: miss
  // Drop-when-full must never corrupt existing entries.
  for (int i = 0; i < 300; i++) {
    pool.kernPairInsert(2, 1, static_cast<uint8_t>(1 + i % 200), static_cast<uint8_t>(1 + i / 200), -3);
  }
  ASSERT_TRUE(pool.kernPairLookup(0, 0, 23, 30, &v));
  EXPECT_EQ(v, -22);
}

TEST(UiGlyphPool, PeekHasNoSideEffects) {
  UiGlyphPool pool;
  ASSERT_TRUE(pool.init(512, 16));
  const auto g = makeGlyph(8, 8, [](int x, int) { return x % 2 ? 3 : 0; });
  UiGlyphPool::Metrics m{g.w, g.h, 0, 0, 0, 23, 30};
  ASSERT_TRUE(pool.insert(0, 0, 0x41, m, g.src2.data(), true));
  const uint32_t hits = pool.stats().hits;
  const uint32_t misses = pool.stats().misses;
  const int32_t hnd = pool.peek(0, 0, 0x41);
  ASSERT_GE(hnd, 0);
  EXPECT_EQ(pool.metricsOf(hnd).kernLeftClass, 23);
  EXPECT_EQ(pool.metricsOf(hnd).kernRightClass, 30);
  EXPECT_LT(pool.peek(0, 0, 0x9999), 0);
  EXPECT_EQ(pool.stats().hits, hits);
  EXPECT_EQ(pool.stats().misses, misses);
}

TEST(UiGlyphPool, ReleaseAndReinit) {
  UiGlyphPool pool;
  ASSERT_TRUE(pool.init(1024, 16));
  const auto g = makeGlyph(8, 8, [](int, int y) { return y % 2 ? 2 : 0; });
  UiGlyphPool::Metrics m{g.w, g.h, 0, 0, 0, 0, 0};
  ASSERT_TRUE(pool.insert(0, 0, 0x41, m, g.src2.data(), true));
  pool.release();
  EXPECT_FALSE(pool.isReady());
  EXPECT_LT(pool.find(0, 0, 0x41), 0);  // no crash, clean miss
  ASSERT_TRUE(pool.reinit());
  EXPECT_TRUE(pool.isReady());
  EXPECT_LT(pool.find(0, 0, 0x41), 0);  // entries did not survive release
  ASSERT_TRUE(pool.insert(0, 0, 0x41, m, g.src2.data(), true));
  EXPECT_GE(pool.find(0, 0, 0x41), 0);
}
