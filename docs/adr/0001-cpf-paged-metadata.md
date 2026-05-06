# ADR 0001 — `.cpf` Paged Glyph Metadata

| Field        | Value                                                |
| ---          | ---                                                  |
| Status       | Proposed                                             |
| Date         | 2026-05-06                                           |
| Decision by  | TBD (RFC pending upstream review)                    |
| Supersedes   | —                                                    |
| Superseded by| —                                                    |

## Context

CrossPoint Reader currently supports 23 European languages plus Chinese as a UI translation. The reader-side fonts (`NotoSerif`, `NotoSans`, `OpenDyslexic` at 12–18 pt) ship as `static const` Flash arrays of ~1070 glyphs covering Latin, Cyrillic, and common symbols. This works because the worst-case 23-language coverage stays within ~1000 glyph metadata entries (16 B each = ~17 KB resident per font), which fits comfortably in Flash.

**CJK changes the regime, not just the language.** Reading common Traditional Chinese requires ~5000 glyph metadata entries; covering the four classical novels in the project's test corpus (三国演义, 水浒传, 红楼梦, 西游记) requires **7830 unique CJK codepoints** including Simplified-to-Traditional variants. At 16 B per `EpdGlyph` row this is ~125 KB resident **per font style**, before any bitmap data. Instantiate four styles (Regular/Bold/Italic/BoldItalic) and the metadata alone overshoots the ESP32-C3's 380 KB total RAM after subtracting the 48 KB framebuffer, FreeRTOS overhead, and activity workspace.

The companion exploration in `lib/EpdFont/SdFontLoader.{h,cpp}` already streams *bitmap* data from SD via a 32 KB LRU group cache, but **leaves glyph metadata fully resident** (`SdFont::epdGlyphs` vector). That single decision caps practical multi-language fallback chains at "one CJK font in RAM at a time", precluding the goal of dropping any TTF on SD and getting all-language coverage.

## Decision

Add a backward-compatible cpf v2.1 extension that **pages glyph metadata to SD** behind a small in-memory page index, keeping resident metadata cost at ~1 KB per font regardless of glyph count.

### Format change

The existing `cpf2::Header.flags` field (`lib/EpdFont/CpfV2Format.h:82`) reserves **bit 1 = "paged metadata"**. When set, the file layout becomes:

```
[Header 64 B]
[Interval Table  12 B × intervalCount]    ← stays fully resident (~600 B for CJK)
[GlyphPageIndex  4 B × ⌈glyphCount/256⌉]   ← stays fully resident (~80 B for 5000 CJK glyphs)
[Glyph Pages     4 KB × pageCount]         ← demand-loaded into a metadata page LRU
[Group Table     16 B × groupCount]
[Compressed Glyph Groups]
```

Each glyph page holds 256 `cpf2::Glyph` rows (= 4 KB = exactly one SD sector × 8). Lookup of glyph index `idx` becomes:

1. `pageId = idx >> 8`
2. `pageOffset = GlyphPageIndex[pageId]`
3. Fetch page from metadata page LRU (size: 4 pages × 4 KB = 16 KB, two-way at minimum)
4. `&page[(idx & 0xFF) × 16]`

Files without bit 1 keep the current resident-array layout, so existing `.cpf` files are not broken.

### Resident memory accounting (CJK font, ~5000 glyphs, ~50 intervals)

| Component                        | Pre-paged | Paged (this ADR) |
| ---                              | ---       | ---              |
| Interval Table                   | 600 B     | 600 B            |
| GlyphPageIndex                   | —         | 80 B             |
| Glyph metadata                   | 80 KB     | 0 (paged)        |
| **Per-font resident**            | **~80 KB** | **~700 B**     |
| Shared metadata page LRU         | —         | 16 KB (one-time) |

A four-font fallback chain (Latin Serif primary + CJK Sans + Symbols + Emoji) drops from ~320 KB resident to ~3 KB + 16 KB shared LRU = **20× reduction** in resident metadata cost.

## Consequences

### Positive

- **Multi-font fallback becomes feasible** within the C3's RAM budget; this ADR is the prerequisite for ADR 0002 (`FontStack`).
- **Backward compatible**: existing `.cpf` files keep loading. Authoring tool (`scripts/export_fonts_v2.py`) gains a `--paged-metadata` flag; users opt in per-font.
- **First-page latency stays bounded**: a typical reader page touches ~30 unique codepoints, each in its own 256-row page on average ≤ 2 unique pages × 1 SD sector read = ~5 ms additional vs the resident-array case.
- **Aligns with the existing bitmap LRU pattern** in `SdDecompressor`. Same caching philosophy, same lock primitives (`HalStorage::StorageLock`), same eviction policy. No new architectural concept introduced.

### Negative / risks

- **Cold cache hit**: the very first glyph access after font registration always misses the page LRU. Mitigated by the prefetcher proposed in ADR 0002 (warming "first page after open" synchronously).
- **Format-version drift**: future cpf changes must respect the bit-flag pattern to stay compatible. We document this convention in the format header and keep a changelog in `docs/file-formats.md`.
- **Verifier complexity**: the round-trip verifier in `scripts/export_fonts_v2.py` must learn the paged layout, doubling its test surface.
- **Loader paths fork**: `SdFontLoader` carries two code paths (resident vs paged) selected by the flag; the abstraction is `EpdFontData::glyphAt(idx)` returning `const EpdGlyph*`, which keeps the caller-side complexity at zero.

## Alternatives considered

### A1. "Fix it in fontconvert" — pre-shrink the glyph struct

Reduce `EpdGlyph` from 16 B → 12 B by clipping `left`/`top` to int8 and dropping unused fields. Saves ~25%, brings 5000-glyph CJK from 80 KB → 60 KB. Still over budget for multi-font, and now the format diverges from `EpdFontData.h`'s public API. **Rejected**: a 25% reduction is a partial fix; this ADR's paged layout achieves >99% reduction and preserves API compatibility.

### A2. Runtime TTF rasterisation (LVGL Tiny TTF / stb_truetype)

The web's standard answer for "all languages from one font file". `lvgl/lvgl#tiny_ttf` and KOReader's FreeType+HarfBuzz stack both do this. Both **assume the entire TTF resides in RAM**: subset NotoSansCJK is 1.9 MB, full is 9 MB; neither fits on the C3 (380 KB total). Without a streaming TTF parser (which would need to be written from scratch — neither stb_truetype nor FreeType supports section-on-demand reads), this approach is not viable on the C3.

**Re-entry trigger**: hardware variant adopts ESP32-S3 with ≥ 2 MB PSRAM. Until then, pre-rasterised + paged metadata is the only realistic path. Documented in ADR 0003.

### A3. Compress the glyph metadata table itself

Run-length or DEFLATE the metadata table and decompress on access. Saves ~50% on Latin (lots of repeated zero `left`/`top`), ~30% on CJK (more variation). Even at 50% saving the multi-font case is over budget, and the lookup hot-path now has decompression overhead. **Rejected**: paging gives better compression (resident → near-zero) and adds zero per-glyph compute.

### A4. Two-tier "hot" + "cold" metadata

Keep top-N most-frequently-used glyphs resident, page the rest. Saves a roundtrip for the hot 1000 glyphs but doubles the loader complexity. **Rejected**: the OS sector cache (`SdFat`) already provides this benefit transparently — frequently-fetched pages stay in the SDFat cache; we don't need a second tier.

## References

- **Web platform**: CSS3 Fonts Module Level 3 — `unicode-range` font subsetting (W3C Recommendation, 2018) — establishes the precedent that font metadata can be partitioned and lazily loaded.
- **LVGL Tiny TTF docs**: https://docs.lvgl.io/9.1/libs/tiny_ttf.html — the PSRAM-assumption alternative explicitly discussed above.
- **KOReader / crengine**: https://github.com/koreader/koreader — Linux-class precedent for runtime rasterisation; assumes ≥ 32 MB RAM.
- **Pebble fontgen** (`pebble/sdk`): closest prior art for pre-rasterised font subsets on Cortex-M-class hardware, but no metadata paging.
- **Bitbank "Building a Better Bitmap Font System" (2025)**: https://bitbanksoftware.blogspot.com/2025/07/building-better-bitmap-font-system.html — Latin-only embedded bitmap font design, validates per-glyph compression but doesn't address CJK metadata scale.

## Implementation tracking

This ADR's implementation plan lives in `docs/architecture/font-system-v3.md`. Phase 3 of that roadmap is the on-device land of paged metadata behind the `--paged-metadata` build flag, gated on Phase 2 (fallback chain) being merged.
