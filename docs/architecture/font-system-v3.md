# Font System v3 — Multi-Language Plug-in Architecture

> **Status**: Request for Comments (RFC). This document accompanies the draft PR
> "RFC: Multi-language plug-in font architecture (v3)" against `master`.
> No code changes ship with this PR — the goal is to socialise the design before
> implementation begins.

## Table of contents

1. [Why this exists](#why-this-exists)
2. [Scope and goals](#scope-and-goals)
3. [Constraints (ESP32-C3, no PSRAM)](#constraints-esp32-c3-no-psram)
4. [Industry survey](#industry-survey)
5. [Proposed architecture](#proposed-architecture)
6. [Memory budget](#memory-budget)
7. [Phased implementation roadmap](#phased-implementation-roadmap)
8. [Verification plan](#verification-plan)
9. [Out of scope (with re-entry triggers)](#out-of-scope-with-re-entry-triggers)
10. [Appendix — measured numbers from the test corpus](#appendix--measured-numbers-from-the-test-corpus)

---

## Why this exists

CrossPoint Reader currently supports 23 European languages plus Chinese as a UI translation. The reader-side fonts are pre-rasterised bitmap arrays in Flash, sized for European-language coverage (~1070 glyphs each at 14 pt). This works because European-language coverage stays inside ~1000 codepoints; the metadata + bitmaps fit in Flash without paging.

CJK reading breaks the assumption. A typical Traditional Chinese book uses **~5000 unique codepoints**; covering the four classical novels in our test corpus (三国演义, 水浒传, 红楼梦, 西游记) requires **7830 unique codepoints** including Simplified-to-Traditional variants. The current `EpdFontFamily` (`lib/EpdFont/EpdFontFamily.h:4-25`) loads all glyph metadata into RAM (16 B per `EpdGlyph`) and has only four style slots (Regular / Bold / Italic / BoldItalic) — no fallback chain when a codepoint is missing from the chosen style.

A working interim solution exists on a development branch: `.cpf v2` SD-streaming bitmaps via `SdFontLoader` and `SdDecompressor` with a 32 KB LRU group cache. That solves *bitmap* streaming but leaves **glyph metadata fully resident** (~144 KB just for one CJK font) and provides **no multi-font fallback**. Drop a Hebrew TTF on the SD card and the reader displays nothing for the Hebrew codepoints.

This document proposes the next step: a re-architecture that removes those two limitations so any user can drop any pre-rasterised font on SD and read any script the bitmap pipeline can render. It is designed to be **a clean floor under future complex-script work** (HarfBuzz shaping, bidi, Indic conjuncts), not a regression that has to be unwound.

## Scope and goals

### In scope

- **Plug-in fonts**: any user can drop a `.cpf` file on the SD card's `/fonts/` directory and the firmware picks it up at boot, with no rebuild.
- **Multi-script reading**: an ordered fallback chain ("font stack") is consulted on glyph misses; users assemble their reading stack from available `.cpf` files via Settings.
- **Any script representable as a sequence of independent left-to-right glyphs**, including all 23 currently-supported European languages plus CJK, Japanese kana, Hangul, Cyrillic, Greek, IPA, math symbols, and emoji (as monochrome bitmaps).
- **First-page latency target**: 95th percentile under 150 ms after opening a book on a cold SD cache.
- **Smooth flipping**: a background prefetcher warms 5–10 pages worth of glyphs ahead, so subsequent page flips stay under 60 ms p95.
- **Authoring tools** so end users (not just developers) can convert their own TTFs to `.cpf` with a single CLI command.

### Out of scope (rationale: ADR 0003)

- HarfBuzz shaping (Arabic cursive forms, Indic conjuncts, advanced typography)
- Bidirectional layout (Hebrew, Arabic ebook reading)
- Word-segmenting line breaking for spaceless scripts (Thai, Khmer)
- Runtime vector outline rasterisation (FreeType / stb_truetype) — pre-rasterised `.cpf` covers all current size needs

Each has a documented re-entry trigger tied to a hardware milestone (typically: ESP32-S3 with ≥ 2 MB PSRAM).

## Constraints (ESP32-C3, no PSRAM)

The CrossPoint Reader hardware is the **Xteink X4** with an **ESP32-C3** at 160 MHz RISC-V, **380 KB total RAM**, **0 PSRAM**, **16 MB Flash** (currently 89% used in `default` build), **single-buffer 800 × 480 e-paper** (48 KB framebuffer), SD card via SPI.

This rules out the standard "industry" approaches. Both LVGL Tiny TTF and KOReader's FreeType+HarfBuzz stack assume the entire TTF resides in RAM (subset NotoSansCJK is 1.9 MB; full is 9 MB). On the C3 we cannot fit either. That hardware reality forces a **pre-rasterise + stream** architecture rather than a runtime-rasteriser one.

The numerical budget the v3 design must hit:

| Resource          | Total       | Already used (baseline) | Available    |
| ---               | ---         | ---                     | ---          |
| Flash             | 16 MB       | 5.84 MB (89%)           | 720 KB       |
| RAM (heap)        | 228 KB      | ~130 KB (FreeRTOS, network, activities) | ~98 KB |
| RAM (min-free)    | —           | observed 22 KB during EPUB indexing | — |
| SD                | 16 GB       | varies                  | gigabytes    |

The v3 design targets ≤ 60 KB resident RAM for *all* loaded fonts in a four-tier fallback stack, leaving the rest for activity workspaces. ADR 0001 (paged metadata) is what makes this achievable.

## Industry survey

| Approach                              | Where it lives                        | Why it doesn't fit C3                                               |
| ---                                   | ---                                   | ---                                                                 |
| **LVGL Tiny TTF** (stb_truetype)      | LVGL libs, ESP32-S3-friendly         | Whole TTF in RAM; 256-glyph default cache; needs PSRAM for CJK.     |
| **KOReader / crengine**               | Linux-class e-readers (Kobo, Kindle)  | FreeType + HarfBuzz; expects ≥ 32 MB RAM; mmap from filesystem.     |
| **Bitbank Group5 / Group4 fax**       | Embedded, ATMega-class                | Latin-only RLE; no metadata-paging; no fallback chain.              |
| **Pebble fontgen** (`pebble/sdk`)     | Cortex-M smartwatches                 | Pre-rasterised .pfo with subset selection — closest prior art.      |
| **Adafruit GFX**                      | Arduino-class displays                | Per-font header file, fully resident; single-byte codepoints.       |
| **CrossPoint v2 (`.cpf` on SD)**      | This repo's `feature/...` branch      | Bitmaps streamed, metadata resident — partial solution.             |
| **CrossPoint v3 (this RFC)**          | This document                         | Bitmaps + metadata both paged + multi-font fallback chain.          |

References for each entry are linked in [ADR 0001](../adr/0001-cpf-paged-metadata.md), [ADR 0002](../adr/0002-font-fallback-chain.md), and [ADR 0003](../adr/0003-defer-harfbuzz-bidi.md).

The CrossPoint v3 architecture is **not a folk solution** — it mirrors the web platform's CSS3 `unicode-range` + font-fallback model faithfully (ADR 0002) and the metadata-paging pattern is how Pebble handled CJK on similar Cortex-M-class hardware. The deviation from LVGL/KOReader is forced by hardware (no PSRAM), not preference.

## Proposed architecture

```
┌──────────────────────────────────────────────────────────────────────────────┐
│                              GfxRenderer                                     │
│  (renderCharImpl, no behaviour change — calls FontStack::getGlyph)           │
└──────────────────────────────────────────────────────────────────────────────┘
                       │
                       ▼
┌──────────────────────────────────────────────────────────────────────────────┐
│                        FontStack (ADR 0002)                                  │
│  tier 0: NotoSerif (Latin)   ─┐                                              │
│  tier 1: NotoSansCJK         ─┤  ordered, missing-glyph falls through        │
│  tier 2: NotoSansSymbols     ─┤                                              │
│  tier 3: NotoEmoji-mono      ─┘                                              │
└──────────────────────────────────────────────────────────────────────────────┘
                       │ (per-tier)
                       ▼
┌──────────────────────────────────────────────────────────────────────────────┐
│                   EpdFontData::glyphAt(idx) (ADR 0001)                       │
│  resident-array fonts → &glyph[idx]                                          │
│  paged-metadata fonts → metadata page LRU (16 KB shared)                     │
└──────────────────────────────────────────────────────────────────────────────┘
                       │
                       ▼
┌──────────────────────────────────────────────────────────────────────────────┐
│                        IDecompressor (existing)                              │
│  FontDecompressor (Flash fonts)  │  SdDecompressor (.cpf SD streaming)      │
│                                  │  + 32 KB bitmap LRU                      │
│                                  │  + warmGroup() called by FontPrefetcher  │
└──────────────────────────────────────────────────────────────────────────────┘
                                                      ▲
                                                      │ (warm)
                       ┌──────────────────────────────┴────┐
                       │       FontPrefetcher              │
                       │   FreeRTOS task, 4 KB stack,      │
                       │   warming next 5–10 pages of      │
                       │   glyphs while user reads current │
                       └───────────────────────────────────┘
                       
┌──────────────────────────────────────────────────────────────────────────────┐
│                         FontRegistry                                         │
│  Boot scan of /fonts/*.cpf → named handles → user assigns to roles           │
│  (Body, Heading, Mono, UI). NVS-persisted per-role FontStack ordering.       │
└──────────────────────────────────────────────────────────────────────────────┘
```

### Layer responsibilities

| Layer                  | Responsibility                                         | Owner / file (proposed)                                |
| ---                    | ---                                                    | ---                                                    |
| `FontStack`            | Ordered fallback resolution (ADR 0002)                 | `lib/EpdFont/FontStack.{h,cpp}`                        |
| `FontRegistry`         | Boot-scan `/fonts/*.cpf`, NVS-persisted role mapping    | `lib/EpdFont/FontRegistry.{h,cpp}` (new)               |
| `FontPrefetcher`       | FreeRTOS task warming next pages                       | `lib/EpdFont/FontPrefetcher.{h,cpp}` (new)             |
| `EpdFontData::glyphAt` | Indirection over resident vs paged metadata (ADR 0001) | `lib/EpdFont/EpdFontData.h`                            |
| `cpf v2.1`             | `flags` bit 1 = paged metadata                         | `lib/EpdFont/CpfV2Format.h`                            |
| `SdFontLoader`         | Load `.cpf`, populate paged or resident metadata       | `lib/EpdFont/SdFontLoader.{h,cpp}`                     |
| `SdDecompressor`       | Bitmap LRU + new metadata page LRU                     | `lib/EpdFont/SdDecompressor.{h,cpp}`                   |
| `IDecompressor`        | Backend abstraction (no change)                        | `lib/EpdFont/IDecompressor.h`                          |
| `cpf-mkfont` CLI       | TTF → `.cpf` for end users                             | `scripts/cpf-mkfont` (new), `scripts/export_fonts_v2.py` (extended) |

`IDecompressor` already exists in upstream and provides the clean abstraction point at `lib/GfxRenderer/GfxRenderer.cpp:10-24`. A future `TtfDecompressor` (PSRAM-equipped hardware variant) could plug in alongside `FontDecompressor` and `SdDecompressor` with **no renderer changes** — the architectural exit ramp for ADR 0003 D5 is built in.

## Memory budget

Resident-RAM accounting at Phase 4 completion (worst case: 4-tier reading stack + 1-tier UI stack, all CJK-sized):

| Component                           | Size       |
| ---                                 | ---        |
| Resident metadata, primary font     | 1 KB       |
| Resident metadata, 3× fallback      | 3 KB       |
| Resident metadata, 1× UI            | 1 KB       |
| 32 KB LRU group cache               | 32 KB      |
| Metadata page LRU (4 × 4 KB shared) | 16 KB      |
| Prefetcher task stack               | 4 KB       |
| FontRegistry + FontStack tiers      | < 1 KB     |
| **Total resident**                  | **~58 KB** |
| **Headroom remaining (of 98 KB)**   | **~40 KB** |

Compare to the pre-paged equivalent (ADR 0001 not applied, one CJK font in RAM): **~190 KB → over budget**. ADR 0001 is what makes the multi-font case fit.

## Phased implementation roadmap

Each phase is an independently mergeable PR. Phases later than 0 do not block one another *technically* — they are ordered for review-cost minimisation.

### Phase 0 — This RFC + ADRs (no code)

- **Goal**: socialise the architecture with `crosspoint-reader/crosspoint-reader` upstream before any code lands.
- **Deliverables**: this document, [ADR 0001](../adr/0001-cpf-paged-metadata.md), [ADR 0002](../adr/0002-font-fallback-chain.md), [ADR 0003](../adr/0003-defer-harfbuzz-bidi.md), README "Font System Roadmap" section.
- **PR title**: "RFC: Multi-language plug-in font architecture (v3)".
- **Test**: maintainer review feedback collected; ADRs accepted or amended.
- **Scope**: docs only. ~1500 lines of markdown.

### Phase 1 — Branch hygiene of existing development work

- **Goal**: prepare the existing Phase 2 development work (currently on `feature/dark-theme-per-theme`) for independent upstream review.
- **Action**: cherry-pick or re-create the relevant Phase 2 commits onto branches off `master`, one focused PR each:
  - PR-1.A: extract `IDecompressor` interface
  - PR-1.B: `cpf v2` format + `SdFontLoader` skeleton
  - PR-1.C: `SdDecompressor` + `main.cpp` integration (the SD-streaming reader font loader)
- **Test**: each PR builds and passes `pio check` + `pio run` standalone.
- **Scope**: no new code; mechanical rebase / cherry-pick.

### Phase 2 — `FontStack` fallback chain (ADR 0002)

- **Goal**: missing glyph in primary font falls through to next font in stack.
- **Files changed**:
  - `lib/EpdFont/EpdFontFamily.h/.cpp` → `FontStack` (rename + extend)
  - `lib/EpdFont/EpdFont.cpp` (1 site)
  - `lib/EpdFont/FontDecompressor.cpp` (6 sites — `font->glyph[idx]` → `font->glyphAt(idx)`)
  - `lib/EpdFont/EpdFontData.h` (add `glyphAt(idx)` shim — default impl returns `&glyph[idx]`)
  - `lib/GfxRenderer/FontCacheManager.h/.cpp` (multi-font cache key)
  - `src/main.cpp` (font registration)
- **Test criteria**:
  - Render "Hello 中文 abc" with primary NotoSerif + fallback NotoSansCJK; first-page render of 三国演义 succeeds with no `[ERR][GFX] No glyph` log lines.
  - Existing Latin-only books render byte-identically vs `master`.
- **Scope**: ~400 LOC.

### Phase 3 — `cpf v2.1` paged metadata (ADR 0001)

- **Goal**: cut resident metadata cost from `~16 B × N` to `< 1 KB` per font.
- **Files changed**:
  - `lib/EpdFont/CpfV2Format.h` (flag bit 1, GlyphPageIndex)
  - `lib/EpdFont/SdFontLoader.{h,cpp}` (paged-mode loader path)
  - `lib/EpdFont/SdDecompressor.{h,cpp}` (add metadata page LRU alongside bitmap LRU)
  - `lib/EpdFont/EpdFontData.h` (`glyphAt` paged path)
  - `scripts/export_fonts_v2.py` (`--paged-metadata` flag)
- **Test criteria**:
  - Round-trip verifier emits identical glyph metadata in flag-on and flag-off modes.
  - Byte-identical bitmap output across the 4-book corpus, paged vs unpaged.
  - Resident heap drops by ≥ 70 KB per CJK font.
- **Scope**: ~700 LOC.

### Phase 4 — Prefetcher + plugin discovery

- **Goal**: 95th-percentile first-page latency < 150 ms; auto-discover `/fonts/*.cpf` for plug-in support.
- **Files added**:
  - `lib/EpdFont/FontPrefetcher.{h,cpp}` (FreeRTOS task)
  - `lib/EpdFont/FontRegistry.{h,cpp}` (boot scan, NVS persistence)
- **Files changed**:
  - `lib/EpdFont/SdFontLoader.cpp` (boot-scan logic)
  - `src/main.cpp` (task spawn, registry init)
  - `src/activities/reader/...` (post next-page codepoint runs to prefetcher)
- **Test criteria** (recorded in PR description):
  - Cold-boot first-page latency: 95th-percentile < 150 ms across 100 trials per book × 4 books.
  - 10-page flip burst: < 60 ms/page p95.
  - Background CPU: < 5% via `uxTaskGetSystemState`.
  - Heap free at idle, 4 fonts loaded: ≥ 35 KB.
- **Scope**: ~600 LOC.

### Phase 5 — `cpf-mkfont` CLI + user docs

- **Goal**: end users (not just developers) can author fonts.
- **Files**:
  - `scripts/cpf-mkfont` — argparse wrapper with `convert / inspect / verify / merge-coverage` subcommands
  - `scripts/export_fonts_v2.py` — CLI polish (`--name`, `--script`, `--coverage`)
  - `docs/fonts/authoring.md` — walkthrough
  - `USER_GUIDE.md` — "Adding fonts" section
- **Test criteria**: documented walkthrough completes in < 5 minutes on a clean machine; round-tripped TTF renders correctly on device.
- **Scope**: ~300 LOC.

**Total**: ~2000 LOC across 5 functional PRs after the docs-only PR-0.

## Verification plan

### Per-phase gates

| Phase | Gate                                                                               |
| ---   | ---                                                                                |
| 0     | ≥ 1 maintainer review; ADRs accepted or amended.                                   |
| 1     | Each cherry-picked PR builds against `master` and passes `pio check` + `pio run`.  |
| 2     | Golden-bitmap diff for 100 mixed-script sentences against pre-recorded fixtures.   |
| 3     | Byte-identical render across paged and resident modes for the 4-book corpus.       |
| 4     | First-page p95 < 150 ms, page-flip p95 < 60 ms, prefetch CPU < 5%, heap ≥ 35 KB.   |
| 5     | Walkthrough completion < 5 min on clean machine; round-tripped TTF renders.        |

### Ground-truth corpus

Four classical Chinese novels in plain UTF-8, available locally during development:

| Book                | Size (txt) | Unique CJK |
| ---                 | ---        | ---        |
| 三国演义.txt        | 2.2 MB     | 4121       |
| 水浒传.txt          | 2.5 MB     | 4251       |
| 红楼梦.txt          | 3.2 MB     | 4817       |
| 西游记.txt          | 2.3 MB     | 4531       |
| **Combined unique** | —          | **5920**   |

The validated full reading set after applying the existing SC→TC mapping (`lib/I18n/Sc2TcTable.h`) is **7830 unique codepoints**.

### Performance benchmarks

Recorded in each PR description for traceability:

- **First-page latency, cold SD**: target < 150 ms p95
- **Page-flip latency, warm**: target < 60 ms p95
- **Prefetch background CPU**: < 5% measured via `uxTaskGetSystemState`
- **Heap free at idle, 4 fonts loaded**: ≥ 35 KB

## Out of scope (with re-entry triggers)

See [ADR 0003](../adr/0003-defer-harfbuzz-bidi.md) for the full rationale on each item.

| Capability                                | Re-entry trigger                                                                             |
| ---                                       | ---                                                                                          |
| HarfBuzz cursive shaping (Arabic, …)      | Hardware refresh to ESP32-S3 + ≥ 2 MB PSRAM                                                  |
| Bidirectional layout (Hebrew, Arabic)     | Contributed RTL i18n translation + user request for RTL ebook reading                        |
| Indic conjuncts and reordering            | Same trigger as HarfBuzz (coupled)                                                           |
| Word-segmenting line breaking (Thai, …)   | Contributed translation + EPUB indexing path is no longer the chapter-open bottleneck        |
| Runtime vector outline rasterisation      | Hardware refresh to PSRAM-equipped variant; user demand for arbitrary runtime size           |

## Appendix — measured numbers from the test corpus

These numbers come from running `python3 -c "..."` snippets against the four-book corpus. They establish the design's quantitative basis.

```
Book                     Unique CJK  ASCII chars    Total chars
三国演义.txt             4121        796247         800368
水浒传.txt               4251        852362         856613
红楼梦.txt               4817        1097783        1102600
西游记.txt               4531        797069         801600
                                                   ─────────
Combined unique CJK:     5920
After SC→TC variants:    7830
```

Coverage curve for top-N most-frequent codepoints across the corpus:

```
top  2000 (2700 after +TC variants):  96.29% coverage,  42 KB resident metadata
top  2500 (3371 after +TC):           97.95% coverage,  53 KB
top  3000 (4062 after +TC):           98.89% coverage,  63 KB
top  3500 (4736 after +TC):           99.41% coverage,  74 KB
top  4000 (5401 after +TC):           99.69% coverage,  84 KB
top  5000 (6692 after +TC):           99.94% coverage, 105 KB
all   7830:                          100.00% coverage, 125 KB
```

Without paged metadata (ADR 0001), even **top 3500 (74 KB)** exceeds the realistic per-style budget for a four-tier fallback stack. With paged metadata, **all 7830 (= 100% coverage)** costs ~700 B per font + 16 KB shared LRU. That order-of-magnitude difference is the technical core of this proposal.
