# ftwasm: FreeType in the browser for the .cpfont converter

This turns TTF/OTF fonts into `.cpfont` files in the browser. The files are byte-identical
to what `lib/EpdFont/scripts/fontconvert_sdcard.py` writes for the same fonts, sizes and
interval presets.

It has two layers:
- FreeType compiled to WebAssembly plus `js/cpfont-raster.mjs`: glyph bitmaps and metrics.
- `js/cpfont-writer.mjs`: kerning, ligatures, synthetic bold and the v4 file layout.
  `convertFamily()` is the single entry point.

## Results

| | |
|---|---|
| Engine | FreeType **2.13.3** + HarfBuzz **8.3.0** (lean), built with emscripten **6.0.10** at `-Oz` |
| `ftwasm.wasm` | **607,732 B raw / 279,565 B gzip -9** |
| `ftwasm.mjs` glue | 4,747 B raw / 2,320 B gzip -9 (ES module, works in a module Web Worker) |
| Parity vs the repo's Python converter | **70,148 / 70,148 glyphs byte-identical (100 %)**: 62 style-sizes over 7 families, every glyph checked for bitmap, width, height, advance, left and top. advanceY, ascender, descender and the validated intervals are identical for all 62 |
| Whole family in Node (Literata, 4 styles × 10/12/14/16/18 pt, 18,060 glyphs, 1.75 MB bitmaps) | **~145 ms** warm, 160 ms first run; module load + init 4 ms |
| Same family in Python (`rasterize_font_style`, includes fontTools kerning) | ~234 s (measured while other jobs were running, so only a rough figure) |
| **Complete .cpfont files vs the Python CLI** | **18 / 18 files byte-identical** (8 cases, 8.8 MiB), plus 11 / 11 with the opt-in kerning fix against the patched script; see [Writer](#writer-complete-cpfont-files) |
| Complete family in Node (Literata, 4 styles × 5 sizes → 5 files, 2.93 MB) | **~0.8 s** warm, 0.9 s first run. The Python CLI takes 247 s |
| JS modules (raster + writer + OpenType reader) | 52 KB raw / 16.5 KB gzip -9, no dependencies |

The build is reproducible bit for bit. Two builds into different directories gave the same
`ftwasm.wasm` sha256: `923b85dc94c125b3849f52b82800c556fb9376cfd025d058304d8c22a3592902`.

## Findings that decide the pins

1. **The Python side runs FreeType 2.13.2 + HarfBuzz 8.3.0, not 2.13.3.** `freetype-py` 2.5.1
   is the newest release on PyPI. Its wheel bundles FreeType 2.13.2 and HarfBuzz 8.3.0, linked
   statically into `libfreetype.so`. This was checked with `freetype.version()` and
   `hb_version_string()`, and the library is byte-identical to the one in the repo `.venv`. No
   freetype-py release bundles 2.13.3.

2. **The old prototype's missing 0.4 % was HarfBuzz, not the version.** The auto-hinter uses
   HarfBuzz to sort glyphs into "styles", each with its own blue zones:
   - Glyphs that the font's GSUB features can reach get a feature style. SourceSerif4's `sups`
     gives ²³⁰⁴…, and the subscripts behave the same way.
   - Literata's `ss02` maps a–d to ←↑→↓, so with HarfBuzz the arrows count as Latin.

   Without HarfBuzz, Literata 14 gives **3,596 / 3,612**, which is exactly the prototype's number.
   Across all test fonts it is 98.8 %, and Bitter loses up to 3 %. In Bitter even ASCII letters
   differ (D J L N Z d f i j l n z). **HarfBuzz is required for parity.**

3. **Why 2.13.3 in the browser even though Python uses 2.13.2.** In 2.13.2, `ftgrays.c`
   flattens conic (TrueType) curves with a DDA only on x86_64, aarch64 or SSE2 hosts. Every
   other target, wasm32 included, uses binary subdivision, which gives different coverage.
   2.13.3 uses the same DDA arithmetic wherever a 64-bit integer type exists, wasm32 included.
   The auto-hinter changes from 2.13.2 to 2.13.3 do not change behaviour: type fixes and an
   overflow-safe rewrite of the same x-height test.
   - wasm 2.13.2 + HarfBuzz: 30 glyphs differ. All are TrueType (Ψ, S, g, ş …); the CFF fonts
     are 100 %.
   - wasm 2.13.3 + HarfBuzz: 0 differ.

4. **A Python FreeType 2.13.3 build was also tested.** `test/build_native_ft.sh` builds a
   native `libfreetype.so` using freetype-py's own recipe from `setup-build-freetype.py`, with
   FreeType 2.13.3 instead of 2.13.2. It goes into `venv-ft2.13.3/`, where `freetype.version()`
   reports `(2, 13, 3)`.
   - Its output is identical to the stock wheel: 70,148 / 70,148.
   - The same recipe with 2.13.2 reproduces the wheel exactly.

   So with the current pins, it makes no difference whether Python uses 2.13.2 or 2.13.3. This
   was tested on x86_64 Linux. By the same source logic it also holds for aarch64 (macOS arm64)
   and Windows x64, though those were not run. The browser build matches both. No change to the
   Python side is needed.

5. **Lean HarfBuzz is enough.** `csrc/hb-ftwasm-config.h` removes code that cannot change glyph
   ids: drawing, paint, color, CFF, metrics, AAT and so on. It saves 195 KB raw / 68 KB gzip
   compared with full HarfBuzz, with identical parity.

### Variant matrix (parity vs the freetype-py 2.5.1 wheel, 62 style-sizes, 70,148 glyphs)

| variant (`dist/…`) | wasm raw | wasm gzip -9 | identical glyphs |
|---|---:|---:|---:|
| **ft2.13.3-hblean** (default) | 607,732 | 279,565 | **70,148 (100 %)** |
| ft2.13.3-hbfull | 802,627 | 347,195 | 70,148 (100 %) |
| ft2.13.3-hbnone | 323,646 | 156,914 | 69,305 (98.80 %) |
| ft2.13.2-hblean | 608,030 | 279,638 | 70,118 (99.96 %) |
| ft2.13.2-hbfull | 802,925 | 347,029 | 70,118 (99.96 %) |
| ft2.13.2-hbnone | 323,944 | 156,955 | 69,275 (98.76 %) |

Build options compared (lean, 2.13.3):
- `-Os`: 642,889 / 288,541, about 7 % faster than `-Oz`.
- `-Oz -flto`: 606,972 / 277,659.

`-Oz` is the default.

### Fonts tested

All fonts are from `lib/EpdFont/scripts/`, with intervals as in `sd-fonts.yaml`, except the CFF
font.

| set | fonts and sizes | intervals |
|---|---|---|
| `core` | Literata 14 pt and SourceSerif4 14 pt, all four styles | `builtin,latin-cyrillic,greek-letters,musical-symbols` |
| `literata` | Literata 10/12/14/16/18 pt, all four styles | as above |
| `wide` | Bitter (instanced), NotoSerif, Inter (instanced) and Charis at 12 and 16 pt, all four styles | as in `sd-fonts.yaml` |
| `otf` | Adobe's CFF-flavoured `SourceSerif4-Regular.otf` / `-It.otf` at 12/14/18 pt | as `core` |

The `otf` fonts are not in the repo; they are downloaded to `dl/fonts-otf/`. They exercise the
`cff` driver and cubic curves.

## Layout

```
build.sh                 reproducible build (emsdk + verified tarballs -> dist/<variant>/)
csrc/ftwasm.c            the C surface (ftw_* functions below)
csrc/ftmodule.h          module list: autofit, truetype, cff, psaux, psnames, sfnt, smooth
csrc/hb-ftwasm-config.h  lean HarfBuzz switches (with the reasoning)
js/cpfont-raster.mjs     JS rasteriser (port of rasterize_font_style's glyph half)
js/cpfont-writer.mjs     convertFamily(): kerning, ligatures, synthetic bold, v4 file layout
js/otf-tables.mjs        the fontTools subset the script uses: head, cmap, kern, GPOS/GSUB
js/pyset-order.mjs       CPython set iteration order (ligature lookups are visited in it)
patches/                 fontconvert_sdcard-kerning-first-match.patch (opt-in fix, not applied)
dist/<variant>/          ftwasm.mjs + ftwasm.wasm + BUILDINFO.txt (versions, sha256s, sizes)
test/parity.mjs          parity test (Python dump vs JS, byte for byte)
test/py_dump.py          runs the repo's own rasterize_font_style() and dumps JSON
test/jobs.mjs            font/size/interval job sets (core, literata, wide, otf)
test/matrix.sh           parity of every dist/ variant
test/compare_refs.mjs    Python-vs-Python (wheel 2.13.2 vs native 2.13.3)
test/build_native_ft.sh  native FreeType+HarfBuzz via freetype-py's recipe -> venv-ft<ver>/
test/bench.mjs           whole-family timing
test/smoke.mjs           page-style load (wasmBinary), memory growth mid-run
test/kern_semantics.py   measures the kerning-summing quirk described below
test/file_parity.mjs     whole-file parity: Python CLI vs convertFamily(), byte for byte
test/make_kern_fonts.py  builds test fonts with a legacy 'kern' table (none in the repo has one)
test/bench_writer.mjs    whole-family timing of the complete conversion
venv/                    freetype-py 2.5.1 wheel (FreeType 2.13.2 + HarfBuzz 8.3.0) + fonttools 4.63.0
venv-ft2.13.3/           same, with a native FreeType 2.13.3 + HarfBuzz 8.3.0 libfreetype.so
out/                     Python reference dumps (ref-wheel/, ref-native-ft2.13.3/), CLI output
                         (cli/, cli-first-match/), generated kern fonts (fonts-kern/), logs
dl/                      downloads: tarballs + .sig, GnuPG keyring, freetype-py sdist, OTF test fonts
emsdk/                   the pinned emscripten SDK (created by build.sh)
```

## Rebuild

```sh
./build.sh                                   # FreeType 2.13.3 + lean HarfBuzz 8.3.0 -> dist/ft2.13.3-hblean
                                             # (the repo commits this build flattened into dist/)
FT_VERSION=2.13.2 HARFBUZZ=none ./build.sh   # other variants: HARFBUZZ=none|lean|full, OPT_FLAGS="-Os"
```

`build.sh` does the following:
- Clones emsdk, checks out tag **6.0.10** (released 2026-09-21), then installs and activates it.
- Fetches `freetype-<ver>.tar.xz` from savannah, falling back to sourceforge, and
  `harfbuzz-8.3.0.tar.xz` from GitHub releases.
- Refuses any tarball whose sha256 does not match the pin:
  - 2.13.3: `0550350666d427c74daeb85d5ac7bb353acba5f76956395995311a9c6f063289`
  - 2.13.2: `12991c4e55c506dd7f9b765933e62fd2be2e06d421505d7950a132e4f1bb484d`
  - HarfBuzz 8.3.0: `109501eaeb8bde3eadb25fab4164e993fbace29c3d775bcaa1c1e58e2f15f847`

How the pins were checked:
- Both FreeType tarballs are identical on savannah and sourceforge, and each has a good GPG
  signature from Werner Lemberg (`E30674707856409FF1948010BE6C3AAC63AD8E3F`).
- The 2.13.2 and HarfBuzz hashes are also the exact pins in freetype-py 2.5.1.

Configuration:
- `ftoption.h` is FreeType's default, which is also what freetype-py's CMake build uses. Only two
  things change:
  - `FT_CONFIG_OPTION_USE_HARFBUZZ` is on, as in freetype-py.
  - zlib/LZW are off, so WOFF and `.Z` files are not accepted.
- `pshinter` is left out because the auto-hinter never reaches it.
- Link: `MODULARIZE` + `EXPORT_ES6`, `ENVIRONMENT=web,worker,node`, `FILESYSTEM=0`,
  `DYNAMIC_EXECUTION=0` (CSP-safe), memory growth on, a 256 KB stack, and Closure on the glue.
- FreeType's rasteriser needs setjmp/longjmp. It uses `SUPPORT_LONGJMP=wasm` (native wasm
  exceptions: Chrome 95+, Firefox 100+, Safari 15.2+, Node 17+).

## Exports

C (`csrc/ftwasm.c`, each a thin wrapper):
- `ftw_init`, `ftw_version`: major/minor/patch, plus whether HarfBuzz is linked.
- Faces: `ftw_new_memory_face`, `ftw_done_face`.
- Sizes: `ftw_set_char_size`, `ftw_set_pixel_sizes`.
- Glyphs: `ftw_get_char_index`, `ftw_load_glyph`, `ftw_render_glyph`.
- `ftw_glyph_info`: bitmap width, rows, pitch, buffer pointer, pixel mode, num_grays,
  bitmap_left/top, linearHori/VertAdvance, advance, lsb/rsb delta, glyph metrics, glyph index,
  format.
- `ftw_size_metrics`: ppem, scale, ascender, descender, height, max_advance.
- `ftw_face_info`: units_per_EM, ascender/descender/height, glyph count, flags, family/style
  name, bbox and so on.
- `malloc` and `free`.

Struct fields are copied into int32 arrays, so JS never depends on FreeType's struct layout.

JS (`js/cpfont-raster.mjs`, no Node or DOM dependencies):
- `FreeTypeWasm.create(createFtWasm, {wasmBinary?, locateFile?})`
- `ft.openFace(bytes)` returns a `Face` with the methods above.
- `rasterizeStyle(ft, fontBytes, size, intervals)` returns
  `{intervals, glyphs[{codePoint, width, height, advanceX, left, top, dataLength, dataOffset, bitmap}], bitmap, totalBitmapSize, advanceY, ascender, descender}`.
- `INTERVAL_PRESETS`, `resolveIntervals()`, `isDefaultIgnorable()` and `pack2bit()` are copies
  of the Python ones.

`rasterizeStyle` follows `rasterize_font_style` step for step:
- Load flags `FT_LOAD_RENDER | FT_LOAD_FORCE_AUTOHINT` at 150 dpi, with the size set before any
  load.
- Interval validation keeps default-ignorables and emits them empty.
- 2-bit packing: v>>6, row-major, continuous across rows, MSB first, last byte zero-padded.
  This equals the script's 4-bit thresholds.
- `advanceX = (linearHoriAdvance + 2048) >> 12`.
- The metrics come from size metrics after loading `|`.
- One difference: Python also renders every glyph during validation. JS only looks up the cmap
  there. Rendering has no side effects, and parity is unaffected.

## Running the parity test

```sh
node test/parity.mjs --sets core                     # Python dumps are cached in out/ref-wheel/
node test/parity.mjs --sets core,literata,wide,otf --dist dist/ft2.13.3-hblean --ascii 3
node test/parity.mjs --sets core --python venv-ft2.13.3/bin/python --ref out/ref-native-ft2.13.3
test/matrix.sh                                       # every dist/ variant
node test/bench.mjs                                  # whole-family timing
```

The Python side imports the repo's `fontconvert_sdcard.py` and calls its
`rasterize_font_style()`, so it is the real pipeline, not a re-implementation. It runs with
`python -I`. The test reports the match rate per style-size and overall. Each mismatch is listed
with its codepoint and what differs (size, origin, advance, or pixel count with the maximum level
delta). `--ascii N` prints side-by-side glyphs for the first N mismatches.

## Licence credits for the web page

FreeType is used under the FreeType License (FTL), which fits the repo's MIT licence. The FTL asks
for this line in the documentation; for 2.13.3, `<year>` is 2024:

> Portions of this software are copyright © 2024 The FreeType Project (www.freetype.org). All rights reserved.

The HarfBuzz notice must also ship with the page, because HarfBuzz is now linked in.
HarfBuzz uses the "Old MIT" licence, which requires the copyright notice and its two
disclaimer paragraphs "in all copies of this software". Ship
`build/src/harfbuzz-8.3.0/COPYING` verbatim next to the wasm, with a link from the credits.
In the repo it is committed as `dist/licenses/HarfBuzz-COPYING.txt`, next to
`FreeType-FTL.txt`. FreeType's `src/autofit/ft-hb.c` carries the same
notice (Red Hat / Google). A credits block could read:

> Font rasterisation: FreeType 2.13.3 (FreeType License) with HarfBuzz 8.3.0 (Old MIT licence; see licenses/harfbuzz.txt).
> Portions of this software are copyright © 2024 The FreeType Project (www.freetype.org). All rights reserved.


## Writer: complete .cpfont files

```js
import createFtWasm from './dist/ftwasm.mjs';
import { FreeTypeWasm } from './js/cpfont-raster.mjs';
import { convertFamily } from './js/cpfont-writer.mjs';

const ft = await FreeTypeWasm.create(createFtWasm);      // once per worker
const files = convertFamily(ft, {
  fonts: { regular, bold, italic, bolditalic },          // Uint8Array each; any subset of the four
  sizes: [10, 12, 14, 16, 18],                            // or "10,12,14,16,18"
  intervals: 'builtin,latin-cyrillic,greek-letters,musical-symbols',  // as in sd-fonts.yaml
  name: 'Literata',                                       // file names: Literata_<size>.cpfont
  // fileNames: { regular: 'Literata-Regular.ttf', ... }  // optional: error messages, default name
  // syntheticBold: false,                                // --synthetic-bold
  // kerning: 'sum',                                      // 'first-match' = the fix, off by default
  // onProgress: ({ size, style, done, total }) => {},
});
// files: [{ fileName, size, bytes: Uint8Array, styles: [{ style, glyphs, kernLeftClasses,
//          kernRightClasses, kerningDropped, ligatures, ligaturesTruncated, ... }] }]
```

This is what `build-sd-fonts.py` runs for each family in `sd-fonts.yaml`:

```
fontconvert_sdcard.py --regular R --bold B --italic I --bolditalic BI \
  --intervals <set> --sizes <list> --name <name> --output-dir <dir>/
```

A family with a single style uses a positional font plus `--style` instead; the output layout
is the same. There is one `<name>_<size>.cpfont` per size, holding all the given styles.

- **Errors.** Errors are thrown with clear messages:
  - **Variable fonts** (an `fvar` table) are refused with a `FontError`. The Python script would
    silently use the default instance; `build-sd-fonts.py` instead makes static instances with
    fontTools first, which the browser cannot reproduce.
  - TTC/OTC collections, WOFF/WOFF2 and unsupported cmap formats are refused too.
  - Values that Python's `struct.pack` would reject raise a `RangeError` naming the field (for
    example a glyph wider than 255 px), as does `advanceY > 255`.
- **No compression.** `.cpfont` stores the 2-bit bitmaps uncompressed. `fontconvert_sdcard.py`
  imports `zlib` only for its `--debug-images` PNGs. zopfli and the grouped/deflated glyph
  storage belong to `fontconvert.py`, which builds the built-in `.h` fonts. Byte-identical
  output therefore needs no deflate implementation.
- **Worker-safe.** The writer modules use no Node or DOM APIs: only `DataView`, `Map`/`Set`
  and `TextEncoder`.

### Parity (`test/file_parity.mjs`)

The Python CLI runs exactly as `build-sd-fonts.py` invokes it, with freetype-py 2.5.1. Its
output is compared with `convertFamily()` byte for byte. On a mismatch, the test names the
first differing byte's section, for example "style 2 kern matrix [37]".

| case | what it covers | sizes | files identical | JS | Python |
|---|---|---|---:|---:|---:|
| Literata (4 styles) | the sd-fonts.yaml family; GPOS extension lookups | 10–18 | 5/5 | 0.84 s | 247 s |
| Bookerly-SD (4 styles, `builtin`) | GPOS **and GSUB** extension lookups (ligatures via type 7), as `generate-sd-fonts.sh` | 12–18 | 4/4 | 0.62 s | 263 s |
| OpenDyslexic (4 styles) | **CFF** outlines (in the repo) | 12, 16 | 2/2 | 0.14 s | 243 s |
| SourceSerif4 OTF (2 styles) | CFF, Adobe release | 14 | 1/1 | 0.15 s | 14 s |
| Inter (2 styles) | 2048 upem: 129 kerning values in Inter Bold 12 pt are exact .5 ties where `Math.round` would differ | 12, 14 | 2/2 | 0.44 s | 43 s |
| KernTest (3 styles, generated) | legacy `kern`: one subtable + GPOS (both summed), two format-0 subtables + a format-2 stub, Apple version 1 | 12, 14 | 2/2 | 0.32 s | 59 s |
| SynthBold | `--synthetic-bold`, with bold = regular file (dilation verified as applied) | 14 | 1/1 | 0.18 s | 27 s |
| Charis | single-style mode (positional font + `--style`) | 14 | 1/1 | 0.07 s | 11 s |
| **total** | | | **18/18** (8.8 MiB) | | |

A Bitter variable font is refused with: `Bitter[wght].ttf: variable fonts are not supported (it
has an 'fvar' table). Use the static .ttf/.otf files of the family, or make a static instance
first (fontTools varLib.instancer, as build-sd-fonts.py does).`

Timing (`test/bench_writer.mjs`): Literata, 4 styles × 5 sizes → 5 files (2.93 MB), takes
**0.80–0.88 s** warm and about 0.9 s on the first run in Node 22. Of that, rasterising is about
0.15 s. Most of the rest is kerning: expanding about 92k design-unit pairs per style, then
deriving classes.

```sh
node test/file_parity.mjs                                  # all cases; Python output cached in out/cli/
node test/file_parity.mjs --cases Literata,Bookerly --regen
node test/bench_writer.mjs
```

### How the writer reproduces the script, including its quirks

**fontTools semantics.** `js/otf-tables.mjs` reads tables the way fontTools does, keyed by
glyph id; fontTools keeps glyph names unique, so ids and names map one to one.
- `getBestCmap()`:
  - preference order (3,10) (0,6) (0,4) (3,1) (0,3) (0,2) (0,1) (0,0), first matching subtable
    in file order;
  - gid-0 entries are dropped, a repeated code keeps its first position but takes the last gid;
  - in format 4 the last segment is always skipped.
- `kern` table:
  - only format-0 subtables are read, coverage bits are ignored, and Apple version-1 headers
    are understood;
  - with exactly one subtable, the length comes from nPairs;
  - a pair repeated inside a subtable keeps its last value.
- Coverage format 2 ranges are stable-sorted by StartCoverageIndex. ClassDef never stores
  class 0. ValueRecord sizes count all 16 format bits.

**Kerning, as `extract_kerning_fonttools` does it today.**
- What is read and summed:
  - every format-0 `kern` pair, plus every `kern`-feature lookup from all scripts and
    languages;
  - only PairPos (type 2, also inside type-9 extensions), `Value1.XAdvance` only;
  - values are **summed across all subtables and lookups and both tables**.
- Rounding: Python `round()` is half to even; JS uses its own `roundHalfEven()`. The doubles
  are computed in the same order: `size*150.0/72.0`, `/upem`, then `du*scale*16`.
- Values are clamped to int8, and pairs that end up 0 are dropped before the codepoint
  aliases are expanded. SMP codepoints are dropped afterwards.
- Classes come from identical rows and columns, numbered in ascending codepoint order. A side
  that needs more than 255 classes (one byte each) first has its closest classes merged, by
  `js/kern-reduce.mjs`, the twin of `lib/EpdFont/scripts/kern_reduce.py` that both Python
  converters use; `test/web/kern_reduce_test.mjs` checks that the two agree. The `styles` result
  reports it as `kerningReduced` (before and after counts, pairs changed). Before this, the
  script wrote no kerning at all for such a style.

**Ligatures, as `extract_ligatures_fonttools` does it.**
- Lookups:
  - `liga` and `rlig` only, LigatureSubst only (type 4, also inside type 7);
  - lookups are visited in **CPython `set` iteration order**, which `js/pyset-order.mjs`
    reproduces. That order is not ascending: 2,964 of 3,000 random lookup-index sets came out
    non-ascending. All 3,000 matched real Python 3.14, which matters when two lookups map the
    same sequence differently.
- The glyph-to-codepoint map:
  - it is built from the whole best cmap, and the last codepoint wins for aliased glyphs;
  - `STANDARD_LIGATURE_MAP` is the fallback.
- Filtering and output:
  - only ligatures whose inputs and output are all requested codepoints ≤ U+FFFF are kept;
  - 3+ character ligatures are chained through their prefix ligature;
  - the list is stable-sorted by the packed key, then truncated to 255.

**Synthetic bold.** When style 1 or 3 is pixel-identical to its base for A, a, g and 0, every
glyph gets a 3×3 max-dilation. The base is italic for style 3 when italic is present,
otherwise regular.

**File layout.**
- A 32-byte header `<8sHHB19s` and 32-byte TOC entries `<B3xIIBhhHHBBBI4x`.
- Per style, back to back with no padding: intervals `<III`, glyphs `<BBHhhH2xI`, kern left
  `<HB`, kern right `<HB`, the int8 matrix, ligatures `<II`, then the bitmaps.
- Every field is range-checked like `struct.pack`.

### The kerning fix (opt-in, not used yet)

The script sums every subtable. OpenType applies only the first subtable of a lookup that
matches a pair, which is how glyph-pair exceptions override class kerning, and shapers ignore
`kern` when GPOS has kerning. Summing over-kerns: for example SourceSerif4 "Vã" ships at
−3.8 px instead of −1.4 px. Pairs that differ at 14 pt:

| font | pairs |
|---|---:|
| Literata Regular | 3,973 |
| Bitter Regular | 3,750 |
| Merriweather | 803 |
| Lora | 354 |
| Noto Serif | 155 |
| SourceSerif4 | 101 |
| Spectral | 26 |
| Charis, GentiumBookPlus | 0 |

- JS: `convertFamily(ft, { ..., kerning: 'first-match' })`. The default is `'sum'`, today's
  behaviour.
- Python: `patches/fontconvert_sdcard-kerning-first-match.patch` applies cleanly with
  `git apply`. It makes the same change in `extract_kerning_fonttools`.
- Verified: `node test/file_parity.mjs --kerning first-match --script <patched copy>` gives
  **11/11 files byte-identical** (Literata 10–18, Inter, KernTest, SourceSerif4 OTF, Charis).
  The fix really changes the output: 201,195 bytes of Inter 12, 38 of SourceSerif4 OTF 14, and
  KernTest Regular's legacy table is ignored because the font has GPOS.
- Switch both sides in the same change, or browser and CLI files will differ.

### Remaining caveats

- **The 255-class cap is close.** Literata Bold already needs 249–250 right classes with
  today's intervals. A font past it no longer loses its kerning: the closest classes are merged,
  weighted so that rare characters absorb the change (ASCII pairs untouched in the measurements),
  and the Font Manager says so.
- **Not reproducible in the browser:**
  - variable fonts (refused);
  - WOFF/WOFF2 and TTC (refused);
  - fonts with embedded bitmap strikes at the target ppem;
  - Apple `morx`-only fonts (lean HarfBuzz drops AAT);
  - cmap subtables in formats 2, 8 or 10 as the best cmap (refused; they don't occur in normal
    Unicode fonts).
- **Malformed fonts.** Where fontTools would raise (corrupt cmap 12 lengths, LigatureSubst
  count mismatches, out-of-range lookup indices), the JS throws a `FontError` too. Other
  malformations may fail differently; neither side writes a file in that case.
- **Shaping language.** The auto-hinter shapes blue-zone characters in HarfBuzz's default
  language, which comes from `setlocale(LC_CTYPE)`. Python ran under `en_US.UTF-8` here, and
  the wasm side has emscripten's C locale; that made no difference on any test font. In
  theory, a font with `locl` rules on blue characters could hint differently when the Python
  converter runs under another locale.
- **freetype-py is pinned** to 2.5.1 (`requirements.txt`). A release bundling FreeType 2.14
  would break parity, because the auto-hinter changed.
- **Serving.** Serve `ftwasm.wasm` as `application/wasm`, or fetch the bytes yourself and pass
  `{wasmBinary}`. Load the modules in a module worker (`new Worker(url, {type: 'module'})`).
  The device web server probably does not gzip, so plan for 608 KB of wasm plus 52 KB of JS
  over WiFi.
