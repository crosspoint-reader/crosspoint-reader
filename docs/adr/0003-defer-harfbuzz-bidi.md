# ADR 0003 — Defer Complex-Script Shaping (HarfBuzz / bidi / Indic / Vector Rasterisation)

| Field        | Value                                                |
| ---          | ---                                                  |
| Status       | Proposed                                             |
| Date         | 2026-05-06                                           |
| Decision by  | TBD (RFC pending upstream review)                    |
| Supersedes   | —                                                    |
| Superseded by| —                                                    |

## Context

ADRs 0001 and 0002 enable CrossPoint to render arbitrary user-supplied `.cpf` fonts in an ordered fallback chain. That covers **all scripts whose written form is a sequence of independent, left-to-right glyphs with at most local ligatures and kerning** — i.e., everything currently in our 23-language i18n list plus CJK, Japanese kana, Hangul, and a number of African / Pacific Latin-derived scripts.

It explicitly does **not** cover four classes of "complex" script behaviour:

1. **Cursive shaping** (Arabic, N'Ko, Syriac, Mongolian) — a glyph's visible form depends on neighbours: isolated, initial, medial, final.
2. **Reordering and conjunct formation** (Devanagari, Tamil, Bengali, Khmer, Thai, …) — the visual order of glyphs differs from logical text order; sub-cluster reordering is required.
3. **Bidirectional layout** (Hebrew, Arabic, mixed-direction text) — runs of right-to-left text must be reversed at the line-layout level, and embedding levels managed per Unicode Bidirectional Algorithm (UAX #9).
4. **Word-segmentation-dependent line breaking** (Thai, Lao, Khmer, Burmese) — these scripts don't use spaces; line breaks require dictionary or ML-based segmentation.

Solving 1–3 in a renderer is the job of a **shaping engine**: HarfBuzz is the de facto open-source standard. Solving 4 is the job of **libthai** / **libunibreak** with appropriate dictionaries.

## Decision

**These four capabilities are out of scope for the v3 font architecture and any predecessor.** The v3 design is engineered to be a clean *floor* under which these can be added later, not a regression that has to be unwound.

Each deferral has an explicit re-entry trigger documented below. When a trigger fires, the next architecture cycle revisits the deferred item; until then, content requiring these capabilities renders **visually wrong but does not crash**.

### Specific deferrals

#### D1. HarfBuzz shaping (cursive scripts + advanced typography)

- **Cost to ship**: HarfBuzz core is ~200 KB compiled; per-shape-run buffers are 2–10 KB; depends on FreeType for glyph outlines (another ~150 KB).
- **Why deferred**: total firmware Flash budget is 16 MB and currently 89% used; we cannot fit HarfBuzz + FreeType. RAM headroom on C3 (~98 KB free heap) cannot support per-run shaping buffers.
- **Re-entry trigger**: hardware variant of the device adopts ESP32-S3 with **≥ 2 MB PSRAM**. Until then, cursive scripts render with **isolated-form glyphs only** — the user can read transliterated content but actual Arabic ebooks will look like disconnected letters.
- **Workaround for v3**: pre-shape at PC authoring time into a font that bakes the shaped joining-form glyphs as separate codepoints in the Private Use Area. Documented in the cpf-mkfont CLI's future "shape-in-place" mode (Phase 5+).

#### D2. Bidirectional layout (RTL text)

- **Cost to ship**: `fribidi` is ~30 KB compiled, but the algorithmic complexity is in the line-breaking and selection layer, which would need significant rework. Our `LineLayout` (currently in `lib/Epub/Epub/parsers/`) assumes left-to-right runs throughout.
- **Why deferred**: rewriting the line layout to be bidi-aware is a multi-week effort touching a path that's already CJK-fragile and SC→TC-fragile. Risk too high relative to current user demand (CrossPoint's i18n list has zero RTL languages).
- **Re-entry trigger**: a translator commits the first Hebrew or Arabic `chinese.yaml`-equivalent translation file *and* there is a user request for RTL ebook reading. Both gates required: i18n alone is satisfiable with isolated forms; ebook reading needs full bidi.
- **Workaround for v3**: Hebrew / Arabic content displays in logical (storage) order, which to a fluent reader is "backwards". UI strings in those languages would look reversed; we therefore decline to accept Hebrew / Arabic translations in i18n until this ADR is revisited.

#### D3. Indic conjuncts and reordering

- **Cost to ship**: depends on D1 (HarfBuzz). Conjuncts are Indic's specific shaping tables.
- **Why deferred**: subset of D1.
- **Re-entry trigger**: same as D1.
- **Workaround for v3**: Indic content displays as a sequence of base + matra glyphs without conjunct formation. Native readers will recognise it as malformed but legible-ish.

#### D4. Word-segmenting line breaking (Thai, Lao, Khmer, Burmese)

- **Cost to ship**: `libthai` is ~50 KB code + 200 KB dictionary; libunibreak is smaller (~20 KB) but does not handle Thai well. Modern alternatives (ICU-LE, ML segmenters) are larger.
- **Why deferred**: dictionary is the bulk of the cost and lives best on SD; the runtime cost is in the EPUB parser path, which already runs at the edge of timing budget for CJK chapters.
- **Re-entry trigger**: Thai / Khmer translation contributed to i18n *and* the EPUB indexing path is no longer the bottleneck for chapter open time.
- **Workaround for v3**: these scripts wrap on any whitespace (rare in their text), producing one extremely long line that scrolls off-screen. Renders correctly otherwise.

#### D5. Runtime vector outline rasterisation (FreeType / stb_truetype)

- **Cost to ship**: FreeType core ~150 KB + 50–100 KB per loaded face's working memory; stb_truetype is smaller (~15 KB code) but expects the entire TTF resident in RAM, and the smallest CJK TTFs are 1.9 MB after subsetting.
- **Why deferred**: cannot fit a full CJK TTF on the C3's 380 KB total RAM. Streaming-TTF parsers (per-table on-demand reads) do not exist as off-the-shelf libraries; building one would be 1–2 weeks of work and add a parsing hot path on every glyph access.
- **Re-entry trigger**: hardware variant adopts ESP32-S3 with ≥ 2 MB PSRAM **and** there is user demand for "drop a TTF on SD, render at any size on demand". Pre-rasterised `.cpf` covers all current size needs (12, 14, 16, 18 pt) at lower runtime cost.
- **Workaround for v3**: users author `.cpf` files at their preferred size via the `cpf-mkfont` CLI; runtime size adjustment is constrained to the pre-generated set.

## Consequences

### Positive

- **The v3 architecture stays small and ships within the C3's budget.** Every line of code in the v3 plan is justified against the resident memory and Flash budget; nothing speculative is included.
- **Re-entry triggers are concrete and observable.** Future maintainers know exactly what to watch for: hardware refresh to S3+PSRAM, contributed RTL translations, etc.
- **`IDecompressor` (`lib/EpdFont/IDecompressor.h`) already abstracts the glyph-data source.** A future `TtfDecompressor` (D5) can plug in alongside `FontDecompressor` and `SdDecompressor` without renderer changes — the architectural exit ramp is built in.
- **Honest about scope**: explicitly listing what's *not* solved spares both contributors and end-users from debugging mismatches between expectation and delivery. ADR text doubles as a user-facing FAQ.

### Negative / risks

- **Complex-script communities will see CrossPoint as "not for them"** — accurate today but may slow contribution in those areas. Mitigated by the explicit re-entry triggers being public, so interested parties can advocate for the hardware refresh.
- **Pre-shaped Arabic in the Private Use Area is a hack**: it works but breaks search and copy-paste correctness. Mitigated by clearly labelling such fonts as "display-only" in the cpf-mkfont CLI.

## References

- **Unicode Standard Annex #9** — UBA (Unicode Bidirectional Algorithm). The canonical specification deferred by D2.
- **HarfBuzz**: https://harfbuzz.github.io/ — reference shaping engine; Linux/Android/Chrome/Firefox use it. Memory profile documented in their wiki.
- **FreeType**: https://freetype.org/ — reference TTF rasteriser. Used by KOReader, Linux desktop, Android.
- **fribidi**: https://github.com/fribidi/fribidi — reference UBA implementation, ~30 KB compiled.
- **libthai**: https://linux.thai.net/projects/libthai — Thai dictionary-based segmenter.
- **libunibreak**: https://github.com/adah1972/libunibreak — Unicode line-breaking; CrossPoint already uses similar logic in `lib/Epub/Epub/parsers/`.

## Implementation tracking

This ADR is informational; nothing is built. Each deferral's re-entry trigger, when it fires, will spawn a new ADR (ADR 0004 onwards) that supersedes the relevant section of this document.
