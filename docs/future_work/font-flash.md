# Font flash and glyph-path speed

Status: open items as of 2026-10-19. Collected from the font-flash analysis of 2026-09-19 (removed in the 2026-10-07 docs cleanup; in git history).

Fonts are the largest part of the firmware image, so they are where flash headroom comes from. Every lever below except the first trades bytes for glyph-path CPU or RAM; measure that trade before changing a format (last item).

Already shipped, so nobody redoes it: the built-in glyph record went from 16 to 8 bytes by deriving `dataLength` and narrowing `left`/`top` (`f66c1a467`, −384 KB), then to 6 bytes by dropping `dataOffset` (`7779e1936`, −97,716 B); lookups return `EpdGlyphRef` by value and `epdResolveGlyph()` is the one place that tells built-in and `.cpfont` records apart. Size-invariant tables are shared across a family's sizes (`51707caae`, −226,560 B). Kerning is stored sparsely (`b16b4cb28`, −414,710 B) with split class maps (`30a3689e7`). Groups decode through a ring sized to their longest back-reference instead of whole (`bf39ecfcd`, ~4 KB peak for +2.1 % font data). Non-English UI languages ship compressed in a flash language pack (`e06754e18`, −232 KB). `GfxRenderer::truncatedText` truncates in one pass (`8b54d1c78`). The bitmap recompression survey found every alternative but brotli dead: zopfli tuning (−0.2 % for 30× the encode time), lzma2 (+1.7 %), zstd (−0.8 %), shared or trained preset dictionaries (+3.6 % to +6.2 %), byte-delta and bitplane pre-transforms (+32 %, +35 %), and per-glyph DEFLATE at any dictionary size (best case +41 %). The six CJK UI faces ship as 1-bit DEFLATE groups (`compress_ui_face.py`, −78.8 KB of font data, −75,238 B of image), the active UI language's inflated into its language pack slot so no UI screen decodes them (`docs/i18n.md`).

## `.cpfont` glyph record 16 → 10 bytes

**Open.** SD-card fonts still store the 16-byte `EpdGlyph` (`CPFONT_VERSION = 4`, `fontconvert_sdcard.py`). The built-in repack's two tricks apply: `dataLength` is derivable and `left`/`top` fit `int8_t`. `dataOffset` cannot shrink to 16 bits, because in a `.cpfont` it is an offset into a per-style bitmap section of up to ~170 KB, so it needs 24 bits.

**Why it matters.** Measured on Noto Sans 14, four styles (743,382 B): the glyph records are 112,448 B. A 10-byte record (24-bit offset) saves 42,168 B (5.7 %) per file; a 12-byte record with a 32-bit offset saves 28,112 B (3.8 %). That is SD space and download size, and less flash-cache partition per family.

**Where.** `SdCardFont`: the `static_assert(sizeof(EpdGlyph) == 16)`, the in-place read of glyph records from the flash mapping (`miniData.glyph` pointing at `mmapDataBase_ + glyphsFileOffset`, which relies on 16-byte records keeping each style's section 4-byte aligned), the per-page `miniGlyphs` reads and the overflow glyph read. `epdResolveGlyph()` and the `sdRecord` field of `EpdGlyphRef`.

**Constraints.** A narrower in-memory record cannot be mapped in place; copying it to the heap would cost ~8 KB per font on a device that reads at ~39 KB free. Read the 10-byte records with `memcpy` through `epdResolveGlyph()` instead. The format becomes version 5: `build-sd-fonts.py` regenerates the hosted families and the font download manager fetches them again, but hand-converted fonts would break, so the loader must keep accepting version 4 (today it rejects any version other than `CPFONT_VERSION`).

**Next step.** Teach the loader to accept both versions, add the version-5 record to `fontconvert_sdcard.py`, and check `glyph_lookup` and `reader_warm` in `env:bench_font` stay flat.

## A packed-glyph cache that outlives one page

**Open.** Decoded glyphs live in per-page slots that each prewarm refills, plus a 4-slot fallback cache (`FontDecompressor::FALLBACK_CACHE_SLOTS`). UI screens never prewarm, so a UI face that missed would thrash those four slots on every repaint. That is why the Inter UI faces ship uncompressed, and why the CJK UI faces, which are compressed, are read from the language pack slot for the active language instead of decoded.

**Why it matters.** The cache was the biggest win in crosspoint-reader PR #3083 (681 → 385 ms a page, pages decoding nothing at all), and it is independent of how the bitmaps are stored: it would speed up today's DEFLATE groups and SD fonts, where a thrashing glyph ring is a measured multi-second cost on the font preview screen. Made global rather than reader-scoped, it is the precondition for compressing the UI faces, for brotli on the UI path, or for a per-glyph codec. A compressed UI face without it would sit in the cold regime: 2,796 µs a glyph against 3.7 µs warm (X3, 2026-09-19), about 84 ms of extra compose on a menu screen.

**Where.** `FontDecompressor` (page slots, fallback slots), `FontCacheManager`.

**Cost.** About 8–10 KB of RAM resident for the session. As a permanent block it has to be allocated at a stable point before anything is released (see [Memory Allocation Strategy §4](../memory-allocation-strategy.md#4-where-each-consumer-lands)); on the X3, which reads at 30–41 KB free, that is the hard part.

**Next step.** Decide whether 8–10 KB of permanent heap is affordable on the X3. If it is, prototype it allocated at boot and measure `reader_cold`, `reader_warm` and Home/Settings compose time.

## Compressing the Inter and Noto Sans 8 UI faces

**Open.** The six Inter UI faces and Noto Sans 8 still ship flat: 226,405 B of bitmaps and offsets. Measured (2026-10-19) with the CJK faces' encoding, 1-bit rows byte-aligned in DEFLATE groups:

| cap | Inter (6 faces) | Noto Sans 8 |
| --- | ---: | ---: |
| 512 B | −89.5 KB | −4.8 KB |
| 1 KB | −107.0 KB | −6.0 KB |
| 2 KB | −116.3 KB | −6.8 KB |
| 4 KB | −123.8 KB | −7.5 KB |

**Why it is not done.** Speed. uzlib decodes these streams at ~2.5 µs a byte on the C3 plus ~0.4 ms per group start (host-scaled from `reader_cold`), so a glyph decoded alone costs ~2.5 ms at 2 KB groups (the CJK faces measure 1.9 ms on the X3), and every UI screen in every language would pay it: Latin glyphs are on every screen. The CJK faces escaped that by moving the active language's faces into the language pack slot, which only works because a CJK UI needs one language's faces; Inter is needed by all of them at once, at 198 KB flat, past what the slot can hold.

**A hybrid that might pay.** ASCII and Latin-1 are 16 % of Inter's bitmap bytes. Kept flat, with the rest (Latin Extended, Cyrillic, Vietnamese, punctuation and arrows beyond Latin-1) compressed, an English, German or French UI decodes nothing, and the saving is still ~90 KB. A Russian or Vietnamese UI would decode its letters, ~40-80 glyphs a screen across the faces in use: too slow without either a per-language slot (Cyrillic is ~9 KB a face) or a glyph cache that outlives a screen (above). Check the punctuation every UI draws (the truncation ellipsis, `«` `»` in the button hints) stays flat.

**Next step.** Decide between a per-language Inter extension in the language pack (the CJK mechanism, sized per language) and the persistent glyph cache, then measure a Russian Settings page on the X3.

## Brotli for glyph bitmaps

**Open.** Brotli at quality 11 with a 64 KB window (`lgwin=16`) is the only bitmap recompression that beat zopfli DEFLATE: −95 KB gross. The decoder costs ~25–40 KB of flash, so the net gain is −55 to −70 KB, for a second decoder on the font path and more CPU per cold glyph.

**Why it matters.** Flash headroom; the image was at 94.8 % of the app partition after `7779e1936` (2026-09-20).

**Ruled out.** Porting crosspoint-reader PR #3083's GlyphStream codec (per-glyph range coding, ~−650 KB on our bitmaps): it was still a draft, its render cost was +107 % before caching, and the Home screen regressed from 14 to 70 ms because its cache was reader-scoped. It would need the global glyph cache first.

**Next step.** Only after the global glyph cache, or for reader faces only: build a decode-only brotli, measure its real flash cost, and compare `reader_cold` and `reader_prewarm` against DEFLATE.

## Kerning lookups are a cache problem

**Open.** On the X3 a kern lookup costs about 2.8 glyph lookups: 3,052 ns a pair on `inter_ui_12`, 63 % of UI text measurement (2026-09-19). The cost tracks the kern table's working set (10 KB for Inter UI against the C3's 16 KB flash cache), not the number of binary-search probes.

**Why it matters.** Little, today. The panel is 92–95 % of every screen update (compose 22–40 ms against a 435 ms FAST refresh), so the whole font path is a few ms of a ~475 ms frame. It would matter if compose ever ran at the low-power CPU clock, where 22 ms becomes ~350 ms.

**Options, none built.** Memoize the codepoint → kern class lookups per font (512 B of `.bss`; judged not worth a permanent cut of the heap ceiling for 2–3 ms of a 475 ms frame); restructure the tables to stay cache-resident (generator work, unknown flash cost); skip kerning for UI text (a visible change, and `drawText()` kerns too, so both paths would change together).

**Next step.** None until compose time is visible. If it becomes so, start with the memo and measure `kerning` and `text_measure` in `env:bench_font`.

## Measure before changing the format

**Open.** Any lever above trades bytes for CPU, and crosspoint-reader PR #3083 stalled because nobody could measure that trade on a device. The instruments exist; the baselines are thin.

**Where.**
- `env:bench_font` (`bench/font_main.cpp`): the font layer on a C3 with no framebuffer or panel. `glyph_lookup`, `kerning`, `text_measure`, `ui_bitmap_fetch`, `reader_cold`, `reader_prewarm` and `reader_warm` for `inter_ui_12`, `inter_ui_14` and `notosans_14`, and `cjk_cold` and `cjk_inflate` for the compressed Chinese UI faces. `pio run -e bench_font -t upload`, then `pio device monitor -e bench_font`. It replaces the firmware; flash `env:default` back afterwards.
- The reader's render benchmark (`EpubReaderActivity::runRenderBenchmark`, reader menu "Render Benchmark"): ten page turns each way with per-phase timings and font cache counters. Compiled out unless `-DENABLE_BENCHMARKS=1`.
- UI screens: only the `Time = … ms from clearScreen to displayBuffer` debug line, which is the whole compose and cannot separate the font path.

**Baselines.** X3 only: the 2026-09-19 capture before the repack, and the after-repack numbers in the message of `f66c1a467`. No X4 or S3 numbers.

**Next step.** Before starting any lever here, run `env:bench_font` on the X3 and the X4 at current master and put the numbers in the PR next to the after-change run, together with Home and Settings compose times.
