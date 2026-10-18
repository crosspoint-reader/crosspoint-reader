// ESP32-C3 font-path benchmark
//
// Measures the per-glyph cost of the font layer on real hardware, split the way the flash levers
// split it (see docs/future_work/font-flash.md):
//
//   * UI faces are UNCOMPRESSED 1-bit. `getGlyphBitmap()` is a direct flash read, so their cost
//     is glyph lookup + kerning + reading the bitmap through the instruction cache. Nothing in
//     the firmware instruments this today — UI screens never prewarm, so the FontDecompressor
//     counters are all zero for them and the only signal is whole-frame compose time.
//   * Reader faces are 2-bit per-group DEFLATE. Their cost has two regimes that differ by three
//     orders of magnitude: a prewarmed page-slot hit, and a fallback that streams the group.
//
// Why this exists: every remaining font-flash lever (narrower glyph records, brotli, a per-glyph
// codec) trades bytes for glyph-path CPU, and crosspoint-reader#3083 stalled precisely because
// there was no way to measure that trade. `glyph_lookup` is the number a narrower glyph record
// moves; `ui_bitmap_fetch` is the number that decides whether a UI face may ever be compressed.
//
// Deliberately does NOT go through GfxRenderer: a framebuffer and a panel would put e-ink
// refresh (hundreds of ms) on top of microsecond measurements and drown them.
//
// Build & flash:  pio run -e bench_font -t upload
// Monitor:        pio device monitor -e bench_font

#include <Arduino.h>
#include <EpdFont.h>
#include <FontDecompressor.h>
#include <builtinFonts/bookerly_18_bolditalic.h>
#include <builtinFonts/inter_ui_12_regular.h>
#include <builtinFonts/inter_ui_14_regular.h>
#include <builtinFonts/notosans_10_regular.h>
#include <builtinFonts/notosans_12_regular.h>
#include <builtinFonts/notosans_14_regular.h>
#include <builtinFonts/notosans_16_regular.h>
#include <builtinFonts/notosans_18_regular.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>

// ---------------------------------------------------------------------------
// Timing / heap
// ---------------------------------------------------------------------------

static int64_t t0;
static void timerStart() { t0 = esp_timer_get_time(); }
static int64_t timerElapsedUs() { return esp_timer_get_time() - t0; }
static size_t snapHeap() { return esp_get_free_heap_size(); }

// Every benchmark folds its result into this. Reading it at the end is what stops the compiler
// deleting the work: a glyph lookup whose result is unused is a dead store, and -O2 will remove
// the entire loop. `volatile` on the accumulator would instead add a store per iteration and
// measure that; folding into a plain global and printing it once does not.
static uint32_t sink = 0;

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

// A menu row: what a list screen actually measures and draws, once per visible row.
static const char* kMenuRow = "Reading statistics";
// A settings row with its value, i.e. the longest single line the UI lays out.
static const char* kSettingsRow = "Inline footnote previews        Enabled";
// A page of body text. Length matters more than content: prewarm cost scales with the number of
// DISTINCT glyphs, and a page of English prose saturates that at ~70 within a sentence or two.
static const char* kReaderPage =
    "The sun had not yet risen. The sea was indistinguishable from the sky, except that the sea "
    "was slightly creased as if a cloth had wrinkles in it. Gradually as the sky whitened a dark "
    "line lay on the horizon dividing the sea from the sky and the grey cloth became barred with "
    "thick strokes moving, one after another, beneath the surface, following each other, pursuing "
    "each other, perpetually.";

// The codepoints a glyph-lookup sweep should cover, weighted the way real text is: mostly ASCII,
// a few accented Latin, and one Cyrillic run. Interval lookup is a binary search over the
// font's interval table, so WHERE a codepoint sits changes its cost — an ASCII-only sweep would
// flatter the measurement.
// clang-format off
static const uint32_t kSweep[] = {
    'e', 't', 'a', 'o', 'i', 'n', 's', 'h', 'r', 'd', 'l', 'u', 'c', 'm', 'f', 'w', 'y', 'p', 'g',
    'b', 'v', 'k', 'x', 'j', 'q', 'z', 'E', 'T', 'A', 'O', 'I', 'N', ' ', '.', ',', '-',
    0x0027 /* ' */, 0x0022 /* " */,
    0x00E9 /* é */, 0x00FC /* ü */, 0x00F1 /* ñ */, 0x00E5 /* å */, 0x0107 /* ć */, 0x0142 /* ł */,
    0x0410 /* А */, 0x0431 /* б */, 0x0432 /* в */, 0x0435 /* е */, 0x043E /* о */, 0x044F /* я */,
};
// clang-format on
static constexpr int kSweepCount = sizeof(kSweep) / sizeof(kSweep[0]);

static FontDecompressor decompressor;

// ---------------------------------------------------------------------------
// 1. Glyph lookup — interval binary search + glyph record read.
//
// This is the unit a narrower glyph record changes: 8 bytes instead of 16 halves what the
// instruction cache has to pull in per lookup, so this number should go DOWN, not up.
// ---------------------------------------------------------------------------

static void benchGlyphLookup(const char* label, const EpdFontData* data) {
  const EpdFont font(data);
  constexpr int REPS = 200;

  // Warm the flash cache so the first pass does not pay for everyone.
  for (int i = 0; i < kSweepCount; ++i) sink += font.getGlyph(kSweep[i]).advanceX;

  timerStart();
  for (int r = 0; r < REPS; ++r) {
    for (int i = 0; i < kSweepCount; ++i) {
      const EpdGlyphRef g = font.getGlyph(kSweep[i]);
      if (g) sink += g.width + g.height + g.advanceX;
    }
  }
  const int64_t us = timerElapsedUs();
  const int32_t ops = REPS * kSweepCount;

  Serial.printf("BENCH glyph_lookup      %-18s total=%6lldus  per_lookup=%4lldns  ops=%ld\n", label, us,
                (us * 1000) / ops, (long)ops);
}

// ---------------------------------------------------------------------------
// 2. Text measurement — lookup + kerning for a whole string.
//
// The layout hot path. Every row is measured before it is drawn, and truncatedText() re-measures
// prefix by prefix, so this runs several times per visible row.
// ---------------------------------------------------------------------------

static void benchTextMeasure(const char* label, const EpdFontData* data, const char* text) {
  const EpdFont font(data);
  constexpr int REPS = 200;
  int w = 0, h = 0;

  font.getTextDimensions(text, &w, &h);  // warm
  timerStart();
  for (int r = 0; r < REPS; ++r) {
    font.getTextDimensions(text, &w, &h);
    sink += (uint32_t)w;
  }
  const int64_t us = timerElapsedUs();

  Serial.printf("BENCH text_measure      %-18s total=%6lldus  per_call=%5lldus  w=%dpx  chars=%u\n", label, us,
                us / REPS, w, (unsigned)strlen(text));
}

// ---------------------------------------------------------------------------
// 2b. Kerning alone.
//
// Isolated because arithmetic over the table sizes said it, not the glyph lookup, is the biggest
// single term in text measurement -- ~9.5 binary-search probes for the left class plus ~9.5 for
// the right against 744-entry tables, against ~6 for the interval search. That was inference from
// symbol sizes; this measures it. If it holds, kerning is where a measurement optimisation should
// go, and the glyph record is not.
//
// Walks adjacent pairs of the sweep so the class lookups hit a realistic spread of codepoints
// rather than one cached row.
// ---------------------------------------------------------------------------

static void benchKerning(const char* label, const EpdFontData* data) {
  const EpdFont font(data);
  constexpr int REPS = 200;

  for (int i = 1; i < kSweepCount; ++i) sink += (uint32_t)font.getKerning(kSweep[i - 1], kSweep[i]);  // warm

  timerStart();
  for (int r = 0; r < REPS; ++r) {
    for (int i = 1; i < kSweepCount; ++i) {
      sink += (uint32_t)(int32_t)font.getKerning(kSweep[i - 1], kSweep[i]);
    }
  }
  const int64_t us = timerElapsedUs();
  const int32_t ops = REPS * (kSweepCount - 1);

  Serial.printf("BENCH kerning           %-18s total=%6lldus  per_pair=%4lldns  ops=%ld\n", label, us,
                (us * 1000) / ops, (long)ops);
}

// ---------------------------------------------------------------------------
// 3. UI bitmap fetch — the uncompressed path, start to finish.
//
// Sums the glyph's actual bitmap bytes rather than just taking its address: the address is
// arithmetic, and the cost being measured is pulling those bytes through the flash cache. That
// read is what a compressed UI face would replace with a decode, which is the whole question.
// ---------------------------------------------------------------------------

static void benchUiBitmapFetch(const char* label, const EpdFontData* data, const char* text) {
  const EpdFont font(data);
  constexpr int REPS = 200;

  if (data->groups != nullptr) {
    Serial.printf("BENCH ui_bitmap_fetch   %-18s SKIPPED (font is compressed)\n", label);
    return;
  }

  int glyphs = 0;
  uint32_t bytes = 0;
  timerStart();
  for (int r = 0; r < REPS; ++r) {
    for (const char* p = text; *p; ++p) {  // ASCII fixtures, so byte == codepoint
      const EpdGlyphRef g = font.getGlyph((uint32_t)(uint8_t)*p);
      // Derived, not stored: EpdGlyphRef deliberately omits dataLength so the measurement path
      // does not pay a multiply it never reads. See EpdGlyphPacked.
      const uint16_t len = g ? glyphDataBytes(g.width, g.height, data->is2Bit) : 0;
      if (!g || len == 0) continue;
      // Also derived: an uncompressed face keeps its offsets in a side table rather than a
      // per-glyph field, for the same reason. This IS the UI fetch path, so it goes through the
      // same accessor GfxRenderer::getGlyphBitmap() uses.
      const uint8_t* bm = &data->bitmap[epdGlyphBitmapOffset(data, g)];
      for (uint16_t b = 0; b < len; ++b) sink += bm[b];
      if (r == 0) {
        glyphs++;
        bytes += len;
      }
    }
  }
  const int64_t us = timerElapsedUs();

  Serial.printf("BENCH ui_bitmap_fetch   %-18s total=%6lldus  per_row=%5lldus  glyphs=%d  bitmap=%luB\n", label, us,
                us / REPS, glyphs, (unsigned long)bytes);
}

// ---------------------------------------------------------------------------
// 4. Reader bitmap fetch, COLD — no prewarm, so every glyph streams its group.
//
// The regime a UI screen would land in if its face were compressed, because UI screens never
// prewarm. Kept to one pass: it is thousands of microseconds per glyph and the point is the
// order of magnitude, not a tight average.
// ---------------------------------------------------------------------------

static void benchReaderBitmapCold(const char* label, const EpdFontData* data) {
  if (data->groups == nullptr) {
    Serial.printf("BENCH reader_cold       %-18s SKIPPED (font is uncompressed)\n", label);
    return;
  }
  const EpdFont font(data);
  decompressor.clearCache();
  decompressor.resetStats();

  int glyphs = 0;
  timerStart();
  for (int i = 0; i < kSweepCount; ++i) {
    const EpdGlyphRef g = font.getGlyph(kSweep[i]);
    if (!g) continue;
    // The index used to be recovered by pointer subtraction; the resolved form carries it.
    const uint8_t* bm = decompressor.getBitmap(data, g, g.index);
    if (bm) {
      sink += bm[0];
      glyphs++;
    }
  }
  const int64_t us = timerElapsedUs();
  const auto& st = decompressor.getStats();

  Serial.printf(
      "BENCH reader_cold       %-18s total=%6lldus  per_glyph=%5lldus  glyphs=%d  streamed=%luB/%luB  peak_ring=%luB\n",
      label, us, glyphs ? us / glyphs : 0, glyphs, (unsigned long)st.streamedBytes, (unsigned long)st.groupBytes,
      (unsigned long)st.peakTempBytes);
}

// ---------------------------------------------------------------------------
// 5. Reader prewarm + WARM fetch — the path a real page turn takes.
//
// Reports the prewarm itself (once per page) separately from the per-glyph hit it buys (once per
// glyph drawn), because a codec change moves those two in opposite directions.
// ---------------------------------------------------------------------------

static void benchReaderPrewarm(const char* label, const EpdFontData* data) {
  if (data->groups == nullptr) {
    Serial.printf("BENCH reader_prewarm    %-18s SKIPPED (font is uncompressed)\n", label);
    return;
  }
  const EpdFont font(data);
  decompressor.clearCache();
  decompressor.resetStats();

  const size_t heapBefore = snapHeap();
  timerStart();
  const int missed = decompressor.prewarmCache(data, kReaderPage);
  const int64_t prewarmUs = timerElapsedUs();
  const size_t heapAfter = snapHeap();
  const auto& pw = decompressor.getStats();

  Serial.printf("BENCH reader_prewarm    %-18s prewarm=%6lldus  heap=%+dB  peak_ring=%luB  missed=%d\n", label,
                prewarmUs, (int)heapBefore - (int)heapAfter, (unsigned long)pw.peakTempBytes, missed);

  // Warm hits. 20 reps because a single pass over one page is only ~70 lookups and the per-glyph
  // cost here is around a microsecond.
  constexpr int REPS = 20;
  decompressor.resetStats();
  int glyphs = 0;
  timerStart();
  for (int r = 0; r < REPS; ++r) {
    for (const char* p = kReaderPage; *p; ++p) {
      const EpdGlyphRef g = font.getGlyph((uint32_t)(uint8_t)*p);
      if (!g) continue;
      const uint8_t* bm = decompressor.getBitmap(data, g, g.index);
      if (bm) {
        sink += bm[0];
        if (r == 0) glyphs++;
      }
    }
  }
  const int64_t us = timerElapsedUs();
  const auto& st = decompressor.getStats();
  const uint32_t total = st.cacheHits + st.cacheMisses;

  Serial.printf("BENCH reader_warm       %-18s total=%6lldus  per_glyph=%4lldns  draws=%d  hit_rate=%.1f%%\n", label,
                us, glyphs ? (us * 1000) / (glyphs * REPS) : 0, glyphs, total ? 100.0f * st.cacheHits / total : 0.0f);

  decompressor.clearCache();
}

// ---------------------------------------------------------------------------
// 6. The widest glyph in the whole coverage, fetched through the FALLBACK path.
//
// Not a timing: a correctness check that only hardware can make. FontDecompressor compacts a
// missed glyph into a fixed FallbackSlot of HOT_GLYPH_BUF_SIZE bytes and gives up if it does not
// fit -- and "gives up" means the glyph renders BLANK, silently, on any page whose prewarm missed
// it. fontconvert.py refuses to generate a font that would overflow the constant, but that check
// only fires at generation time and has been wrong twice by being guessed rather than measured.
//
// U+01C4 (DZ digraph) in bookerly_18_bolditalic at 54x38 packs to 513 B and is the glyph that
// sets the constant. If this prints FAIL, HOT_GLYPH_BUF_SIZE is too small.
// ---------------------------------------------------------------------------

static void checkWidestGlyph(const char* label, const EpdFontData* data, const uint32_t cp) {
  const EpdFont font(data);
  const EpdGlyphRef g = font.getGlyph(cp);
  if (!g) {
    Serial.printf("CHECK widest_glyph      %-18s FAIL (U+%04lX not in coverage)\n", label, (unsigned long)cp);
    return;
  }
  const uint16_t need = glyphDataBytes(g.width, g.height, data->is2Bit);

  // Cold on purpose: an empty cache is what forces the fallback slot rather than a page hit.
  decompressor.clearCache();
  const uint8_t* bm = decompressor.getBitmap(data, g, g.index);

  Serial.printf("CHECK widest_glyph      %-18s U+%04lX %ux%u needs=%uB buf=%uB  %s\n", label, (unsigned long)cp,
                (unsigned)g.width, (unsigned)g.height, (unsigned)need, (unsigned)FontDecompressor::HOT_GLYPH_BUF_SIZE,
                bm ? "PASS" : "FAIL (renders blank on a prewarm miss)");
  if (bm) sink += bm[0];
  decompressor.clearCache();
}

// ---------------------------------------------------------------------------
// 7 + 8. Glyph resampling: what it costs, and how close it lands.
//
// The reader can render a glyph at an arbitrary scale (GfxRenderer::renderCharAtScale), which is
// how headings, per-word CSS sizes and every size an SD font does not ship are drawn. That raises
// the obvious question about the built-in size ladder: how many of those sizes need to be real
// faces at all, when 48 reader faces are ~2.3 MB of the image?
//
// Answering it needs two numbers nobody had: what a resample costs per glyph, and how far a
// resampled glyph is from the real face at that size.
//
// IMPORTANT, so the figures are not over-read: the resampler below MIRRORS the production rule --
// area-weighted coverage when enlarging, point sampling when reducing -- but is a SEPARATE
// implementation. The production one is a static function that writes into a framebuffer or a
// 1-bit mask, and this benchmark deliberately links no panel. So read the timing as the cost of
// the arithmetic, and the fidelity as a property of the METHOD rather than a byte-exact audit of
// that function.
// ---------------------------------------------------------------------------

// Built-in glyphs are at most 61 wide and 46 tall (18 pt); round up and allow headroom for an
// upscale target (x1.44 for 26 pt off the 18 pt master).
static constexpr int kMaxGlyphDim = 112;
static uint8_t srcCov[kMaxGlyphDim * kMaxGlyphDim];
static uint8_t dstCov[kMaxGlyphDim * kMaxGlyphDim];
static uint8_t refCov[kMaxGlyphDim * kMaxGlyphDim];

// Unpack a glyph's bitmap into one coverage byte per pixel (0..3 for 2-bit, 0/1 for 1-bit).
// Bitmaps are a CONTINUOUS bit stream in glyph order with no row stride -- the same property that
// lets dataLength and dataOffset both be derived rather than stored.
//
// MSB-first, 4 pixels per byte at 2 bpp. Equivalent to GfxRenderer's get2BitPixel(), which reads
// `(bitmap[pos >> 2] >> ((3 - (pos & 3)) * 2)) & 0x3` -- the generalised form below produces the
// same shift for every index (0->6, 1->4, 2->2, 3->0, 4->6...), and the same MSB-first order
// bitmapExtract() uses at 1 bpp. Getting this backwards would not crash, it would quietly
// invert the fidelity numbers, so it is worth checking against those two rather than trusting.
static bool unpackGlyph(const uint8_t* bm, const bool is2Bit, const int w, const int h, uint8_t* out) {
  if (!bm || w <= 0 || h <= 0 || w > kMaxGlyphDim || h > kMaxGlyphDim) return false;
  const int bpp = is2Bit ? 2 : 1;
  for (int i = 0; i < w * h; ++i) {
    const int bit = i * bpp;
    const uint8_t byte = bm[bit >> 3];
    const int shift = 8 - bpp - (bit & 7);
    out[i] = static_cast<uint8_t>((byte >> shift) & (is2Bit ? 0x03 : 0x01));
  }
  return true;
}

// Resample srcCov (sw x sh) into dstCov (dw x dh).
//
// FIXED POINT, mirroring GfxRenderer::emitScaledGlyphPixels, and that is the whole point of this
// rewrite. The first version of this benchmark used floats and reported 21.7 ms per glyph for an
// upscale against 110 us for a downscale -- a 200x gap that says nothing about resampling and
// everything about the ESP32-C3 being RV32IMC with NO FPU, so every float op is a soft-float
// call. Production had already been there: its comment notes fixed point is "an order of
// magnitude cheaper than one soft-float op" and that the historical float path was replaced and
// verified pixel-equivalent. Measuring the mistake production had already fixed produced a number
// that would have argued against synthesised sizes for entirely the wrong reason.
//
// The two remaining differences from production are deliberate and neither touches the pixels: it
// writes into a 1-bit mask or a framebuffer through a lambda, and it folds the draw-mask decision
// into the same loop. This writes coverage levels so they can be compared against a real face.
constexpr int FP_SHIFT = 16;
constexpr int32_t FP_ONE = 1 << FP_SHIFT;

// Area-weighted coverage. Works in BOTH directions -- production only takes this path when
// enlarging, but running it while reducing is how we tell whether downscaling is inherently less
// faithful or whether that is the price of point sampling's crispness.
static void resampleArea(const int sw, const int sh, const int dw, const int dh, const uint8_t maxLevel) {
  const int32_t invW = static_cast<int32_t>((static_cast<int64_t>(sw) << FP_SHIFT) / dw);
  const int32_t invH = static_cast<int32_t>((static_cast<int64_t>(sh) << FP_SHIFT) / dh);
  const int64_t areaFP = static_cast<int64_t>(invW) * invH;
  int32_t sy0FP = 0;
  for (int y = 0; y < dh; ++y, sy0FP += invH) {
    const int32_t sy1FP = sy0FP + invH;
    const int ya = sy0FP >> FP_SHIFT;
    const int yb = (sy1FP - 1) >> FP_SHIFT < sh - 1 ? (sy1FP - 1) >> FP_SHIFT : sh - 1;
    int32_t sx0FP = 0;
    for (int x = 0; x < dw; ++x, sx0FP += invW) {
      const int32_t sx1FP = sx0FP + invW;
      const int xa = sx0FP >> FP_SHIFT;
      const int xb = (sx1FP - 1) >> FP_SHIFT < sw - 1 ? (sx1FP - 1) >> FP_SHIFT : sw - 1;
      int64_t covered = 0;
      for (int sy = ya; sy <= yb; ++sy) {
        const int32_t loY = sy0FP > (sy << FP_SHIFT) ? sy0FP : (sy << FP_SHIFT);
        const int32_t hiY = sy1FP < ((sy + 1) << FP_SHIFT) ? sy1FP : ((sy + 1) << FP_SHIFT);
        const int32_t hOv = hiY - loY;
        if (hOv <= 0) continue;
        for (int sx = xa; sx <= xb; ++sx) {
          const uint8_t raw = srcCov[sy * sw + sx];
          if (raw == 0) continue;  // production skips blank source pixels too; most of a glyph is blank
          const int32_t loX = sx0FP > (sx << FP_SHIFT) ? sx0FP : (sx << FP_SHIFT);
          const int32_t hiX = sx1FP < ((sx + 1) << FP_SHIFT) ? sx1FP : ((sx + 1) << FP_SHIFT);
          const int32_t wOv = hiX - loX;
          if (wOv <= 0) continue;
          covered += static_cast<int64_t>(raw) * hOv * wOv;
        }
      }
      const int64_t lvl = (covered + areaFP / 2) / areaFP;
      dstCov[y * dw + x] = static_cast<uint8_t>(lvl < maxLevel ? lvl : maxLevel);
    }
  }
}

// Point sampling, incrementally stepped so there is no divide in the loop. This is what production
// uses when reducing, for crispness rather than fidelity.
static void resamplePoint(const int sw, const int sh, const int dw, const int dh) {
  const int32_t invW = static_cast<int32_t>((static_cast<int64_t>(sw) << FP_SHIFT) / dw);
  const int32_t invH = static_cast<int32_t>((static_cast<int64_t>(sh) << FP_SHIFT) / dh);
  int32_t syFP = 0;
  for (int y = 0; y < dh; ++y, syFP += invH) {
    const uint8_t* srow = &srcCov[(syFP >> FP_SHIFT) * sw];
    int32_t sxFP = 0;
    for (int x = 0; x < dw; ++x, sxFP += invW) dstCov[y * dw + x] = srow[sxFP >> FP_SHIFT];
  }
}

// Polyphase resample for an EXACT rational ratio p/q, i.e. dst = src * p / q.
//
// Jens' observation: scale up by a factor, then down by another, and the arithmetic stays
// integer. That is the rational decomposition, and it is not an approximation of area weighting --
// it IS area weighting, with the overlap weights expressed as small integers instead of 16.16
// fractions. The equivalence is checked below rather than asserted.
//
// Why it is worth a separate path when the fixed-point version already has no floats: that
// version still carries, per destination pixel, an int64 multiply-accumulate (overlaps reach
// 65536, so raw * hOv * wOv needs 64 bits) and a 64-bit divide to requantise. Both vanish here.
//
// Work in units of 1/p of a source pixel. Destination pixel x spans [x*q, x*q + q); source pixel
// k spans [k*p, k*p + p). Every bound is an integer, the overlaps are integers summing to q, and
// with raw <= 3 the 2D accumulator peaks at 3*q*q -- 243 for q=9, so 16 bits is plenty and the
// multiplies are single-cycle 32-bit on RV32IMC.
//
// And the pattern is PERIODIC with period p, because (x*q) mod p cycles with that period once
// p/q is in lowest terms. So the per-axis weights are a p-entry table built once, and the inner
// loop is a lookup plus a couple of multiply-adds. That periodicity is only available because the
// ladder's ratios are ours to choose and are exact small rationals (20/18 = 10/9, 22/18 = 11/9,
// 24/18 = 4/3, 26/18 = 13/9). An arbitrary CSS size is not a nice rational and keeps the general
// path.
static constexpr int kMaxPhase = 16;  // p for the ladder ratios: 10, 11, 4, 13
struct Phase {
  int16_t src0;  // first source pixel this phase touches, relative to the period's base
  uint8_t w[2];  // enlargement touches at most two source pixels per axis (q < p)
  uint8_t n;
};

static bool buildPhases(const int p, const int q, Phase* out) {
  if (p > kMaxPhase || q >= p) return false;  // enlargement only; reduction touches more pixels
  for (int x = 0; x < p; ++x) {
    const int lo = x * q;  // in 1/p-source-pixel units
    const int hi = lo + q;
    const int k0 = lo / p, k1 = (hi - 1) / p;
    out[x].src0 = static_cast<int16_t>(k0);
    out[x].n = static_cast<uint8_t>(k1 - k0 + 1);
    if (out[x].n > 2) return false;
    for (int k = k0; k <= k1; ++k) {
      const int a = lo > k * p ? lo : k * p;
      const int b = hi < (k + 1) * p ? hi : (k + 1) * p;
      out[x].w[k - k0] = static_cast<uint8_t>(b - a);
    }
  }
  return true;
}

static bool resamplePolyphase(const int sw, const int sh, const int dw, const int dh, const int p, const int q,
                              const uint8_t maxLevel) {
  static Phase px[kMaxPhase], py[kMaxPhase];
  if (!buildPhases(p, q, px) || !buildPhases(p, q, py)) return false;
  const int area = q * q;
  for (int y = 0; y < dh; ++y) {
    const Phase& fy = py[y % p];
    const int sy0 = (y / p) * q + fy.src0;
    for (int x = 0; x < dw; ++x) {
      const Phase& fx = px[x % p];
      const int sx0 = (x / p) * q + fx.src0;
      int acc = 0;
      for (int iy = 0; iy < fy.n; ++iy) {
        const int syy = sy0 + iy;
        if (syy >= sh) break;
        const uint8_t* row = &srcCov[syy * sw];
        const int wy = fy.w[iy];
        for (int ix = 0; ix < fx.n; ++ix) {
          const int sxx = sx0 + ix;
          if (sxx >= sw) break;
          acc += row[sxx] * wy * fx.w[ix];
        }
      }
      const int lvl = (acc + area / 2) / area;
      dstCov[y * dw + x] = static_cast<uint8_t>(lvl < maxLevel ? lvl : maxLevel);
    }
  }
  return true;
}

// Does polyphase compute the SAME pixels as the 16.16 area path, or merely similar ones?
//
// The glyph comparison cannot answer that, and I wrongly claimed it did: there dw is
// truncate(sw * p / q), so the area path resamples at its real ratio sw/dw while polyphase assumes
// q/p. For 'e' at 18 pt that is 17/18 = 0.944 against 10/11 = 0.909, a 4% drift accumulating to
// most of a pixel across the glyph -- which is what the 9-11% of differing pixels showed.
//
// So: both paths on a source whose dimensions are MULTIPLES OF q, where dw = sw*p/q is exact and
// the ratio is identical. Pseudo-random coverage, because a smooth field could agree by accident.
//
// MEASURED (2026-09-20): 6/5 and 4/3 come out bit-identical; 11/10 and 13/10 differ by one level
// on ~2% of pixels. That pattern acquits the weights -- wrong weights would be wrong at every q --
// and points at requantisation ties: with q=10 the divisor is 100, so far more accumulated values
// land on a rounding boundary than with 25 or 9.
//
// And the tie breaks in polyphase's favour. The area path derives invW = (sw << 16) / dw by
// TRUNCATION -- for 20 -> 22 that is 59578 against an exact 59578.18 -- so its cell boundaries
// creep by a fraction of a pixel across the glyph. Polyphase sums exact integer overlaps over
// q*q. Where they disagree, polyphase is the more accurate of the two, not the deviant one.
static void checkPolyphaseAlgebra(const int p, const int q) {
  const int sw = q * 2, sh = q * 2;
  const int dw = sw * p / q, dh = sh * p / q;
  if (dw > kMaxGlyphDim || dh > kMaxGlyphDim) return;
  uint32_t s = 0x12345678u;
  for (int i = 0; i < sw * sh; ++i) {
    s = s * 1664525u + 1013904223u;
    srcCov[i] = static_cast<uint8_t>((s >> 24) & 0x03);
  }
  resampleArea(sw, sh, dw, dh, 3);
  for (int i = 0; i < dw * dh; ++i) refCov[i] = dstCov[i];
  if (!resamplePolyphase(sw, sh, dw, dh, p, q, 3)) return;
  int differing = 0, maxDelta = 0;
  for (int i = 0; i < dw * dh; ++i) {
    const int d = static_cast<int>(dstCov[i]) - static_cast<int>(refCov[i]);
    const int ad = d < 0 ? -d : d;
    if (ad) ++differing;
    if (ad > maxDelta) maxDelta = ad;
  }
  Serial.printf("CHECK poly_algebra      %2d/%-2d on a %dx%d exact-ratio source: differs=%d/%d px (max %d level)  %s\n",
                p, q, sw, sh, differing, dw * dh, maxDelta,
                differing == 0 ? "bit-identical"
                               : (maxDelta <= 1 ? "off-by-one at requantisation ties (polyphase is the exact side)"
                                                : "MORE than a tie -- check the weights"));
}

// The rule production applies: area-weighted when enlarging, point-sampled when reducing.
static void resample(const int sw, const int sh, const int dw, const int dh, const uint8_t maxLevel) {
  if (dw >= sw)
    resampleArea(sw, sh, dw, dh, maxLevel);
  else
    resamplePoint(sw, sh, dw, dh);
}

static bool loadCoverage(const EpdFontData* data, const uint32_t cp, int* w, int* h, uint8_t* out) {
  const EpdFont font(data);
  const EpdGlyphRef g = font.getGlyph(cp);
  if (!g || g.width == 0 || g.height == 0) return false;
  decompressor.clearCache();
  const uint8_t* bm = decompressor.getBitmap(data, g, g.index);
  if (!bm) return false;
  *w = g.width;
  *h = g.height;
  return unpackGlyph(bm, data->is2Bit, g.width, g.height, out);
}

static void benchGlyphResample(const char* label, const EpdFontData* data, const float scale) {
  // 'e' is the fairest single glyph for a cost measure: mid-sized, curved, with a counter.
  int sw = 0, sh = 0;
  if (!loadCoverage(data, 'e', &sw, &sh, srcCov)) {
    Serial.printf("BENCH glyph_resample    %-18s SKIPPED (no 'e')\n", label);
    return;
  }
  const int dw = static_cast<int>(sw * scale + 0.5f), dh = static_cast<int>(sh * scale + 0.5f);
  if (dw > kMaxGlyphDim || dh > kMaxGlyphDim) {
    Serial.printf("BENCH glyph_resample    %-18s SKIPPED (%dx%d over buffer)\n", label, dw, dh);
    return;
  }
  const uint8_t maxLevel = data->is2Bit ? 3 : 1;

  constexpr int REPS = 200;
  timerStart();
  for (int r = 0; r < REPS; ++r) resampleArea(sw, sh, dw, dh, maxLevel);
  const int64_t areaUs = timerElapsedUs();
  sink += dstCov[0];

  timerStart();
  for (int r = 0; r < REPS; ++r) resamplePoint(sw, sh, dw, dh);
  const int64_t pointUs = timerElapsedUs();
  sink += dstCov[0];

  // Both methods at every ratio, and a per-DESTINATION-PIXEL rate as well as the per-glyph cost:
  // the per-glyph figure scales with glyph area, so comparing sizes needs the rate, and the rate
  // is what multiplies out to a page of ~400 glyphs.
  Serial.printf(
      "BENCH glyph_resample    %-18s x%.2f  %2dx%-2d -> %2dx%-2d  area=%4lldus  point=%4lldus  "
      "area_per_px=%3lldns\n",
      label, scale, sw, sh, dw, dh, areaUs / REPS, pointUs / REPS, (areaUs * 1000) / (REPS * dw * dh));
}

// Polyphase against the 16.16 area path at the same ratio: how much faster, and -- first --
// whether it computes the same pixels. A speedup from a different answer is not a speedup.
static void benchPolyphase(const char* label, const EpdFontData* data, const int p, const int q) {
  int sw = 0, sh = 0;
  if (!loadCoverage(data, 'e', &sw, &sh, srcCov)) {
    Serial.printf("BENCH polyphase         %-18s SKIPPED (no 'e')\n", label);
    return;
  }
  const int dw = sw * p / q, dh = sh * p / q;
  if (dw > kMaxGlyphDim || dh > kMaxGlyphDim) {
    Serial.printf("BENCH polyphase         %-18s SKIPPED (%dx%d over buffer)\n", label, dw, dh);
    return;
  }
  const uint8_t maxLevel = data->is2Bit ? 3 : 1;

  // Area path first, stashed so the polyphase output can be diffed against it.
  resampleArea(sw, sh, dw, dh, maxLevel);
  for (int i = 0; i < dw * dh; ++i) refCov[i] = dstCov[i];
  if (!resamplePolyphase(sw, sh, dw, dh, p, q, maxLevel)) {
    Serial.printf("BENCH polyphase         %-18s SKIPPED (%d/%d not a supported ratio)\n", label, p, q);
    return;
  }
  int differing = 0, maxDelta = 0;
  for (int i = 0; i < dw * dh; ++i) {
    const int d = static_cast<int>(dstCov[i]) - static_cast<int>(refCov[i]);
    const int ad = d < 0 ? -d : d;
    if (ad) ++differing;
    if (ad > maxDelta) maxDelta = ad;
  }

  constexpr int REPS = 200;
  timerStart();
  for (int r = 0; r < REPS; ++r) resampleArea(sw, sh, dw, dh, maxLevel);
  const int64_t areaUs = timerElapsedUs();
  sink += dstCov[0];
  timerStart();
  for (int r = 0; r < REPS; ++r) resamplePolyphase(sw, sh, dw, dh, p, q, maxLevel);
  const int64_t polyUs = timerElapsedUs();
  sink += dstCov[0];

  Serial.printf(
      "BENCH polyphase         %-18s %2d/%-2d %5.3fx  area=%4lldus  poly=%4lldus  x%.1f faster  "
      "poly_per_px=%3lldns   differs=%d/%d px (max %d level)\n",
      label, p, q, static_cast<double>(p) / q, areaUs / REPS, polyUs / REPS,
      polyUs ? static_cast<double>(areaUs) / polyUs : 0.0, (polyUs * 1000) / (REPS * dw * dh), differing, dw * dh,
      maxDelta);
}

// How close a resampled master lands to the real face at that size.
//
// Resampled to the REAL glyph's dimensions on purpose: that isolates SHAPE fidelity from the
// separate question of whether a scaled advance rounds to the same box, which is reported
// alongside as dim= so the two are not conflated.
static void checkResampleFidelity(const char* label, const EpdFontData* src, const EpdFontData* ref, const char* chars,
                                  const bool forceArea = false) {
  const uint8_t maxLevel = ref->is2Bit ? 3 : 1;
  Serial.printf("-- resample fidelity: %s --\n", label);
  long totalAbs = 0, totalPx = 0;
  for (const char* p = chars; *p; ++p) {
    const uint32_t cp = static_cast<uint32_t>(static_cast<uint8_t>(*p));
    int sw = 0, sh = 0, rw = 0, rh = 0;
    if (!loadCoverage(ref, cp, &rw, &rh, refCov) || !loadCoverage(src, cp, &sw, &sh, srcCov)) {
      Serial.printf("   '%c'  SKIPPED (glyph missing or empty)\n", *p);
      continue;
    }
    if (rw > kMaxGlyphDim || rh > kMaxGlyphDim) continue;
    if (forceArea)
      resampleArea(sw, sh, rw, rh, maxLevel);
    else
      resample(sw, sh, rw, rh, maxLevel);

    long absSum = 0, refInk = 0, gotInk = 0, exact = 0;
    for (int i = 0; i < rw * rh; ++i) {
      const int d = static_cast<int>(dstCov[i]) - static_cast<int>(refCov[i]);
      absSum += (d < 0 ? -d : d);
      refInk += refCov[i];
      gotInk += dstCov[i];
      if (d == 0) ++exact;
    }
    const int px = rw * rh;
    totalAbs += absSum;
    totalPx += px;
    // The box a naive scale would have produced, for comparison with the real one.
    const float sc = static_cast<float>(rh) / static_cast<float>(sh);
    const int naiveW = static_cast<int>(sw * sc + 0.5f);
    Serial.printf("   '%c'  %2dx%-2d -> %2dx%-2d  MAD=%4.1f%%  exact=%3d%%  ink=%+5.1f%%  dim=%+d\n", *p, sw, sh, rw,
                  rh, 100.0f * absSum / (px * maxLevel), static_cast<int>(100L * exact / px),
                  refInk ? 100.0f * (gotInk - refInk) / refInk : 0.0f, naiveW - rw);
  }
  if (totalPx) {
    Serial.printf("   OVERALL mean absolute coverage error = %.1f%% of full range\n",
                  100.0f * totalAbs / (totalPx * maxLevel));
  }
}

// ---------------------------------------------------------------------------
// 9. Fidelity as a function of RATIO — how far one master can be stretched.
//
// The question this answers is not "can sizes be synthesised" but "how many masters do we need
// so that every synthesised size is still good". Those are different: a single master covering
// 10-20 pt asks for ratios from 0.5x to 2.0x, while a master every other size asks for 0.86x to
// 1.17x, and the flash difference between those two answers is about a megabyte.
//
// Every pair of shipped faces gives a real answer, because the TARGET face exists to compare
// against. 5 sizes -> 20 ordered pairs -> ratios from 0.56 to 1.80, all measured rather than
// interpolated from the two points the earlier run happened to cover. (With 24 pt, and later 20 pt,
// still shipped, the same sweep reached 2.40 and 2.00.)
//
// Area-weighted in BOTH directions on purpose: production point-samples when reducing (for
// crispness), which measured 15.3% against area weighting's 6.6% at the same ratio. If sizes are
// going to be synthesised, area weighting is the mode that would be used, so it is the mode to
// characterise.
// ---------------------------------------------------------------------------

struct SizedFace {
  uint8_t pt;
  const EpdFontData* data;
};

static const SizedFace kFaces[] = {
    {10, &notosans_10_regular}, {12, &notosans_12_regular}, {14, &notosans_14_regular},
    {16, &notosans_16_regular}, {18, &notosans_18_regular},
};
static constexpr int kFaceCount = sizeof(kFaces) / sizeof(kFaces[0]);

// Returns mean absolute coverage error in tenths of a percent, and fills the worst character.
static int resampleErrorTenths(const EpdFontData* src, const EpdFontData* ref, const char* chars, char* worstChar,
                               int* worstTenths) {
  const uint8_t maxLevel = ref->is2Bit ? 3 : 1;
  long totalAbs = 0, totalPx = 0;
  *worstChar = '?';
  *worstTenths = 0;
  for (const char* p = chars; *p; ++p) {
    const uint32_t cp = static_cast<uint32_t>(static_cast<uint8_t>(*p));
    int sw = 0, sh = 0, rw = 0, rh = 0;
    if (!loadCoverage(ref, cp, &rw, &rh, refCov) || !loadCoverage(src, cp, &sw, &sh, srcCov)) continue;
    if (rw > kMaxGlyphDim || rh > kMaxGlyphDim) continue;
    resampleArea(sw, sh, rw, rh, maxLevel);
    long absSum = 0;
    for (int i = 0; i < rw * rh; ++i) {
      const int d = static_cast<int>(dstCov[i]) - static_cast<int>(refCov[i]);
      absSum += (d < 0 ? -d : d);
    }
    const int px = rw * rh;
    const int tenths = static_cast<int>(1000L * absSum / (px * maxLevel));
    if (tenths > *worstTenths) {
      *worstTenths = tenths;
      *worstChar = *p;
    }
    totalAbs += absSum;
    totalPx += px;
  }
  return totalPx ? static_cast<int>(1000L * totalAbs / (totalPx * maxLevel)) : -1;
}

static void sweepResampleFidelity(const char* chars) {
  Serial.println("-- fidelity vs ratio (area-weighted; every shipped face pair) --");
  Serial.println("   from  to   ratio   MAD   worst");

  // Ordered pairs, ascending by ratio so the quality gradient reads straight down the column --
  // which is exactly what choosing master spacing needs. 80 bytes of table beats the stateful
  // selection sort this started as.
  struct Pair {
    uint8_t i, j;
    uint16_t ratioMilli;  // j/i * 1000, integer so the sort has no float comparisons
  };
  Pair pairs[kFaceCount * (kFaceCount - 1)];
  int n = 0;
  for (int i = 0; i < kFaceCount; ++i) {
    for (int j = 0; j < kFaceCount; ++j) {
      if (i == j) continue;
      pairs[n++] = {static_cast<uint8_t>(i), static_cast<uint8_t>(j),
                    static_cast<uint16_t>(1000u * kFaces[j].pt / kFaces[i].pt)};
    }
  }
  for (int a = 1; a < n; ++a) {  // insertion sort; n is 20
    const Pair key = pairs[a];
    int b = a - 1;
    while (b >= 0 && pairs[b].ratioMilli > key.ratioMilli) {
      pairs[b + 1] = pairs[b];
      --b;
    }
    pairs[b + 1] = key;
  }

  for (int k = 0; k < n; ++k) {
    char worst = '?';
    int worstTenths = 0;
    const int tenths =
        resampleErrorTenths(kFaces[pairs[k].i].data, kFaces[pairs[k].j].data, chars, &worst, &worstTenths);
    if (tenths < 0) continue;
    Serial.printf("   %2u -> %2u  %5.3f  %4.1f%%  '%c' %4.1f%%\n", kFaces[pairs[k].i].pt, kFaces[pairs[k].j].pt,
                  pairs[k].ratioMilli / 1000.0f, tenths / 10.0f, worst, worstTenths / 10.0f);
  }
}

// ---------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(2000);

  Serial.println("\n=== Font path ESP32-C3 benchmark ===");
  Serial.printf("CPU: %u MHz   free heap: %u B\n", (unsigned)getCpuFrequencyMhz(), (unsigned)esp_get_free_heap_size());
  Serial.printf("glyph record: EpdGlyphPacked %u B (built-in), EpdGlyph %u B (.cpfont)\n\n",
                (unsigned)sizeof(EpdGlyphPacked), (unsigned)sizeof(EpdGlyph));

  if (!decompressor.init()) {
    Serial.println("FATAL: FontDecompressor::init() failed");
    return;
  }

  Serial.println("-- UI faces (uncompressed 1-bit; UI screens never prewarm) --");
  benchGlyphLookup("inter_ui_12", &inter_ui_12_regular);
  benchGlyphLookup("inter_ui_14", &inter_ui_14_regular);
  benchKerning("inter_ui_12", &inter_ui_12_regular);
  benchKerning("inter_ui_14", &inter_ui_14_regular);
  benchTextMeasure("inter_ui_12/row", &inter_ui_12_regular, kMenuRow);
  benchTextMeasure("inter_ui_14/row", &inter_ui_14_regular, kMenuRow);
  benchTextMeasure("inter_ui_12/set", &inter_ui_12_regular, kSettingsRow);
  benchTextMeasure("inter_ui_14/set", &inter_ui_14_regular, kSettingsRow);
  benchUiBitmapFetch("inter_ui_12/row", &inter_ui_12_regular, kMenuRow);
  benchUiBitmapFetch("inter_ui_14/row", &inter_ui_14_regular, kMenuRow);

  Serial.println("\n-- Reader face (2-bit, per-group DEFLATE) --");
  benchGlyphLookup("notosans_14", &notosans_14_regular);
  benchKerning("notosans_14", &notosans_14_regular);
  benchTextMeasure("notosans_14/page", &notosans_14_regular, kReaderPage);
  benchReaderBitmapCold("notosans_14", &notosans_14_regular);
  benchReaderPrewarm("notosans_14", &notosans_14_regular);

  // The largest real face, and the master every larger size is drawn from. Worth its own run rather
  // than assuming it scales from 14 pt: its glyphs are ~1.7x the area, so the groups are larger and
  // both the streamed bytes and the ring the decoder has to hold grow with them. If a bigger size
  // were going to cost something, the cold and prewarm lines here are where it would appear -- and
  // 20-26 pt pay exactly these, since their glyphs ARE these before the resample.
  Serial.println("\n-- Reader face at the 18 pt master --");
  benchGlyphLookup("notosans_18", &notosans_18_regular);
  benchKerning("notosans_18", &notosans_18_regular);
  benchTextMeasure("notosans_18/page", &notosans_18_regular, kReaderPage);
  benchReaderBitmapCold("notosans_18", &notosans_18_regular);
  benchReaderPrewarm("notosans_18", &notosans_18_regular);

  Serial.println("\n-- Fallback-slot sizing (correctness, not timing) --");
  checkWidestGlyph("bookerly_18_bi", &bookerly_18_bolditalic, 0x01C4);
  checkWidestGlyph("notosans_18", &notosans_18_regular, 0x0489);

  // What a synthesised size costs per glyph, at the ratios the ladder actually uses: 18 pt is the
  // top real rung, and 20/22/24/26 come off it at x1.11, x1.22, x1.33 and x1.44.
  Serial.println();
  Serial.println("-- Resampling cost (would a synthesised size be affordable?) --");
  benchGlyphResample("notosans_18", &notosans_18_regular, 20.0f / 18.0f);
  benchGlyphResample("notosans_18", &notosans_18_regular, 22.0f / 18.0f);
  benchGlyphResample("notosans_18", &notosans_18_regular, 24.0f / 18.0f);
  benchGlyphResample("notosans_18", &notosans_18_regular, 26.0f / 18.0f);
  // And one reduction, which is what the middle rungs would need if they were ever synthesised.
  benchGlyphResample("notosans_18", &notosans_18_regular, 16.0f / 18.0f);

  // And how close it lands. Characters chosen for what resampling is worst at rather than for
  // being common: curves and counters ('e', 'o', 'a'), thin stems and a detached dot ('i', 'l'),
  // diagonals ('W', 'x'), dense joins ('M'), a descender ('g') and a feature only a few pixels
  // across ('.'), where losing one pixel is a large relative error.
  //
  // Fidelity needs the TARGET face to exist, so the ladder's own synthesis sizes cannot be scored
  // -- nothing real to compare 22-from-18 against. These are the nearest measurable proxies: 16->18
  // is x1.125, just over the x1.111 of the ladder's smallest step, 12->16 is x1.333, exactly the
  // 24 pt ratio, and 12->18 is x1.5, beyond the x1.444 of its largest. Between them they bracket
  // every ratio that ships.
  Serial.println();
  checkResampleFidelity("notosans 16 -> 18 UPSCALE (x1.125) vs the real 18 pt face", &notosans_16_regular,
                        &notosans_18_regular, "eoaMWilxg.");
  Serial.println();
  checkResampleFidelity("notosans 12 -> 16 UPSCALE (x1.333) vs the real 16 pt face", &notosans_12_regular,
                        &notosans_16_regular, "eoaMWilxg.");
  Serial.println();
  checkResampleFidelity("notosans 12 -> 18 UPSCALE (x1.500) vs the real 18 pt face", &notosans_12_regular,
                        &notosans_18_regular, "eoaMWilxg.");
  Serial.println();
  checkResampleFidelity("notosans 18 -> 16 DOWNSCALE, point-sampled (production rule)", &notosans_18_regular,
                        &notosans_16_regular, "eoaMWilxg.");
  // The same reduction, area-weighted. Production point-samples when reducing for CRISPNESS, which
  // is a different goal from matching the real face -- so if this scores much better, "downscaling
  // is less faithful" is a statement about the sampling choice, not about reducing.
  Serial.println();
  checkResampleFidelity("notosans 18 -> 16 DOWNSCALE, area-weighted (for comparison)", &notosans_18_regular,
                        &notosans_16_regular, "eoaMWilxg.", /*forceArea=*/true);

  // The ladder's own ratios, as exact rationals, against the general fixed-point path. These are
  // the four sizes synthesised from the real 18 pt master: 20, 22, 24, 26.
  Serial.println();
  Serial.println("-- polyphase vs the general path, at the ladder's exact ratios --");
  // Off the real 18 pt master, which is what the ladder scales from.
  benchPolyphase("notosans_18", &notosans_18_regular, 10, 9);  // 20 <- 18
  benchPolyphase("notosans_18", &notosans_18_regular, 11, 9);  // 22 <- 18
  benchPolyphase("notosans_18", &notosans_18_regular, 4, 3);   // 24 <- 18
  benchPolyphase("notosans_18", &notosans_18_regular, 13, 9);  // 26 <- 18

  // Whether the two paths agree once the ratio is exact -- the claim the glyph rows cannot test.
  // 4/3 doubles as the control: it came out bit-identical when first measured, so if it ever stops
  // matching, the phase tables have regressed.
  checkPolyphaseAlgebra(10, 9);
  checkPolyphaseAlgebra(11, 9);
  checkPolyphaseAlgebra(4, 3);
  checkPolyphaseAlgebra(13, 9);

  // How far ONE master can be stretched before it stops being good enough -- the number that
  // decides how many real faces to ship, rather than whether to ship any.
  Serial.println();
  sweepResampleFidelity("eoaMWilxg.");

  Serial.printf("\nfree heap after: %u B   minimum ever: %u B\n", (unsigned)esp_get_free_heap_size(),
                (unsigned)esp_get_minimum_free_heap_size());
  Serial.printf("(sink=%lu — printed only so the optimiser keeps the work above)\n", (unsigned long)sink);
  Serial.println("=== done ===");
  Serial.println();
  Serial.println("This sketch is the font benchmark ONLY -- no display, no UI, no reader. The");
  Serial.println("device now sits idle and unresponsive; that is the end of the run, not a hang.");
  Serial.println("Reflash the firmware to get the reader back:");
  Serial.println("    pio run -e default -t upload");
  Serial.println("Whole-screen frame times ([GFX] Time = N ms from clearScreen to displayBuffer)");
  Serial.println("come from THAT build, not this one.");
}

void loop() {
  // Nothing left to do, but keep saying so: the first run of this sketch read as a crash purely
  // because the port went silent after the results.
  delay(30000);
  Serial.println("[idle] benchmark finished; reflash the reader with: pio run -e default -t upload");
}
