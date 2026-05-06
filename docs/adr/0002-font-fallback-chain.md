# ADR 0002 — Multi-Font Fallback Chain (`FontStack`)

| Field        | Value                                                |
| ---          | ---                                                  |
| Status       | Proposed                                             |
| Date         | 2026-05-06                                           |
| Decision by  | TBD (RFC pending upstream review)                    |
| Supersedes   | —                                                    |
| Superseded by| —                                                    |
| Depends on   | ADR 0001 (paged glyph metadata)                      |

## Context

`EpdFontFamily` (`lib/EpdFont/EpdFontFamily.h:4-25`) groups four font *style* variants — Regular, Bold, Italic, Bold-Italic — under one logical "font family". When the renderer needs a glyph it calls `EpdFontFamily::getGlyph(cp, style)` which returns `nullptr` if that codepoint is absent from the chosen style.

The renderer's current response to `nullptr` is to log an error and skip the glyph (`lib/GfxRenderer/GfxRenderer.cpp:85`):

```cpp
const EpdGlyph* glyph = fontFamily.getGlyph(cp, style);
if (!glyph) {
  LOG_ERR("GFX", "No glyph for codepoint %d", cp);
  return;
}
```

This is acceptable when one font covers everything the user can show. It breaks the moment we want **multi-script reading**:

- A Latin-serif body font has no CJK glyphs.
- A CJK font has no Hangul (well, NotoSansCJK does, but most CJK-only fonts don't).
- An emoji font has no Latin.
- An Arabic font has no Cyrillic.

In every world-class text-rendering stack, the answer is the same — **ordered fallback**. A primary font is consulted first; missing glyphs fall through to a list of fallback fonts in priority order. CSS calls this `font-family` with `unicode-range`-subsetted webfonts. The OS calls this "font fallback chain" and ships per-language stacks. KOReader calls this "fallback fonts" and lets the user reorder them.

CrossPoint's current single-font design is the outlier, not the norm.

## Decision

Replace `EpdFontFamily`'s 4-pointer struct with a `FontStack` of up to N **tiers**, where each tier is itself a (Regular, Bold, Italic, Bold-Italic) quartet. Glyph lookup walks the tiers in order, returning the first tier whose chosen style provides the codepoint.

```cpp
struct StyleSet {
  const EpdFont* regular;
  const EpdFont* bold;
  const EpdFont* italic;
  const EpdFont* boldItalic;
};

class FontStack {
 public:
  static constexpr uint8_t MAX_TIERS = 4;

  FontStack(StyleSet primary);
  FontStack& addFallback(StyleSet tier);

  // Returns first tier whose `style` slot has the codepoint.
  // Returns nullptr only if no tier covers it.
  const EpdGlyph* getGlyph(uint32_t cp, EpdFontFamily::Style style) const;

  // Returns the EpdFontData of the resolved tier so the renderer
  // can pull metrics (advanceY, ascender, baseline) from the same
  // tier as the glyph.
  const EpdFontData* resolveData(uint32_t cp, EpdFontFamily::Style style) const;

  // Returns the primary tier's EpdFontData for line-metrics queries
  // that don't depend on a specific glyph (e.g. cursor blink height).
  const EpdFontData* primaryData(EpdFontFamily::Style style) const;

 private:
  std::array<StyleSet, MAX_TIERS> tiers_;
  uint8_t tierCount_ = 1;
};
```

`MAX_TIERS = 4` is enough for the canonical reading stack:

```
tier 0: NotoSerif       (Latin + Cyrillic, primary body font)
tier 1: NotoSansCJK     (CJK)
tier 2: NotoSansSymbols (math, dingbats)
tier 3: NotoEmoji-mono  (e-paper-friendly emoji)
```

Each tier holds 4 `const EpdFont*` (32 B per tier × 4 tiers = 128 B per FontStack — value-typed, no heap allocations).

### Lookup behaviour

```
for tier in tiers_[0..tierCount_]:
  font = tier.fontFor(style)
  if font has cp:
    return (font, tier.fontData)
return primary.fontFor(style).getGlyph(.notdef)  // tofu, drawn with primary metrics
```

When all tiers miss, the primary font's `.notdef` glyph is drawn so vertical metrics and baseline stay consistent with surrounding text. This matches CSS's "tofu" rendering rather than skipping the codepoint entirely.

### Style fallback within a tier

Before falling through to the next tier, a tier with no Bold variant should try its own Regular (matching CSS `font-style` synthesis). This avoids "primary's Regular → fallback's Bold" weight inversions that would happen with a naive cross-tier walk:

```
for tier in tiers_:
  for style in [requested, regular_synth_of(requested)]:
    font = tier.fontFor(style)
    if font and font has cp:
      return ...
```

### Renderer integration

The renderer's dispatch site (`lib/GfxRenderer/GfxRenderer.cpp:10-24` — `getGlyphBitmap`) takes `(EpdFontData*, EpdGlyph*)` pairs that are *already resolved* by the FontStack. **No renderer-internal change** is needed: the FontStack does the resolution one level up, in `renderCharImpl` (`GfxRenderer.cpp:80-169`).

The seven sites that currently do `font->glyph[idx]` (`lib/EpdFont/EpdFont.cpp:170`, `lib/EpdFont/FontDecompressor.cpp:91, 98, 296, 371, 398, 450`) move to a new accessor `EpdFontData::glyphAt(idx)` that returns `const EpdGlyph*`. For resident-array fonts this is `&glyph[idx]`; for paged fonts (ADR 0001) it consults the metadata page LRU. The transition is mechanical and atomic.

### Plug-in registration

A new `FontRegistry` (proposed for Phase 4 of the roadmap) scans `/fonts/*.cpf` at boot, parses each header, and exposes the discovered fonts to the user as named handles. The user assigns each *role* — Body, Heading, Mono, UI — its own ordered FontStack via Settings, and the choices persist in NVS:

```
SETTINGS.fontStack.body = ["NotoSerifTC-14", "NotoSansCJK-14", "NotoSansSymbols-14"]
```

Drop a new `.cpf` on SD, reboot, the font appears in the chooser. **No firmware rebuild.**

## Consequences

### Positive

- **All-language reading is finally possible** without per-language firmware builds. The user assembles their reading stack from `.cpf` files on SD.
- **Mirrors web platform standards** (CSS Fonts Module Level 3) — reviewers familiar with web typography immediately understand the design.
- **No renderer change**: the fallback logic lives entirely in `FontStack` and `EpdFontData::glyphAt`. `GfxRenderer.cpp:10-24` is unchanged.
- **Style synthesis is built-in**: CJK fonts that lack italic variants don't break italic text — the layer falls back to Regular within the same tier, preserving font identity.
- **Resident cost is bounded**: 128 B per FontStack × small number of stacks (Body, UI, …) = ~1 KB total. Well under any budget.

### Negative / risks

- **Performance**: glyph lookup is O(tiers × intervals) instead of O(intervals). For 4 tiers with binary-searched intervals this is ~4 × log₂(50) ≈ 22 comparisons; negligible vs the SD I/O on cache miss.
- **Glyph-metadata heterogeneity**: the resolved tier's `EpdFontData::ascender/descender` may differ slightly from the primary's, causing baseline jitter mid-line. Mitigated by **always using the primary tier's vertical metrics for line layout**, only the tier's bitmap and horizontal advance for the glyph itself. Documented contract.
- **Cross-tier kerning is unsupported**: kerning tables are intra-font; primary→fallback transitions skip kerning. Acceptable cost; web rendering does the same.
- **More API surface**: `FontStack` adds methods over the simple `EpdFontFamily`. Mitigated by keeping `EpdFontFamily` as a thin wrapper for single-tier use, so existing call sites that don't need fallback can keep using the old API name.

## Alternatives considered

### A1. "One mega-font with everything baked in"

Generate a single `.cpf` containing Latin + CJK + Symbols + Arabic + … Eliminates fallback machinery; the renderer just looks up. **Rejected**: combinatorial blow-up. Every new language requires re-running the toolchain and re-flashing or re-distributing one giant font file. Plug-and-play user experience disappears. Resident metadata grows linearly with coverage even with paging (ADR 0001) — paging spreads the cost over SD I/O, not eliminates it.

### A2. Per-codepoint font dispatch table

Build a `cp → fontIndex` lookup table at boot covering all loaded fonts. O(1) lookup, no fallback walk. **Rejected**: a 0x10000 BMP-coverage table costs 64 KB just for indices; sparse codepoints waste most of it. Hash tables solve the sparsity but add per-lookup overhead and complexity. The tier-walk's 22 comparisons are cheaper than a hash probe on the C3.

### A3. Fallback as a renderer concern (not a font concern)

Push the fallback logic into `GfxRenderer::renderCharImpl` directly: try font A, fail, try font B, … **Rejected**: violates separation of concerns. The renderer should ask one question — "give me the glyph and bitmap" — and get one answer. Fallback policy is a property of the *font collection*, not the rendering pipeline.

### A4. CSS-style `unicode-range` per-tier subset specification

Let each tier declare which Unicode ranges it claims, and skip tiers that don't claim the codepoint. **Deferred to future work**: useful when many fallback fonts cover overlapping ranges (e.g., emoji and CJK both have arrows). For our current 4-tier reading stack, simple "first tier with the codepoint wins" is adequate. Re-entry trigger: > 4 tiers, > 30% coverage overlap.

## References

- **CSS3 Fonts Module Level 3** § 4.3 ("Default fonts and font matching") — W3C Recommendation, defines the cascading fallback model.
- **W3C `unicode-range` descriptor**: https://drafts.csswg.org/css-fonts/#unicode-range-desc — the canonical web-platform pattern.
- **Apple Core Text Font Cascade Lists**: documented in Apple Developer's Core Text Programming Guide. macOS/iOS rendering pipeline since 2007.
- **Microsoft DirectWrite Font Fallback**: https://learn.microsoft.com/en-us/windows/win32/api/dwrite/nn-dwrite-idwritefontfallback — Windows shipping API, mirrors CSS.
- **KOReader Font Fallback** (`koreader/koreader`): user-orderable fallback font list, persisted per-language.
- **Pango / FontConfig**: GNOME stack — substring-matching fallback resolver. Linux-class but widely studied.

## Implementation tracking

`FontStack` lands in Phase 2 of the roadmap (`docs/architecture/font-system-v3.md`). The 7 `glyph[idx]` callsites' migration to `glyphAt(idx)` lands as part of the same PR so the abstraction is consistent. `FontRegistry` plugin discovery lands in Phase 4 alongside the prefetcher.
