# ftwasm: FreeType in the browser for the .cpfont converter

This is the engine for turning TTF/OTF fonts into `.cpfont` in the browser. It
produces the same per-glyph data as `lib/EpdFont/scripts/fontconvert_sdcard.py`.
It covers glyphs, bitmaps and metrics. Kerning, ligatures and the file writer are
not ported yet (see the last section).

## Results

| | |
|---|---|
| Engine | FreeType **2.13.3** + HarfBuzz **8.3.0** (lean), built with emscripten **6.0.10** at `-Oz` |
| `ftwasm.wasm` | **607,732 B raw / 279,565 B gzip -9** |
| `ftwasm.mjs` glue | 4,747 B raw / 2,320 B gzip -9 (ES module, works in a module Web Worker) |
| Parity vs the repo's Python converter | **70,148 / 70,148 glyphs byte-identical (100 %)**: 62 style-sizes over 7 families, every glyph checked for bitmap, width, height, advance, left and top. advanceY, ascender, descender and the validated intervals are identical for all 62 |
| Whole family in Node (Literata, 4 styles × 10/12/14/16/18 pt, 18,060 glyphs, 1.75 MB bitmaps) | **~145 ms** warm, 160 ms first run; module load + init 4 ms |
| Same family in Python (`rasterize_font_style`, includes fontTools kerning) | ~234 s (measured while other jobs were running, so only a rough figure) |

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
venv/                    freetype-py 2.5.1 wheel (FreeType 2.13.2 + HarfBuzz 8.3.0) + fonttools 4.63.0
venv-ft2.13.3/           same, with a native FreeType 2.13.3 + HarfBuzz 8.3.0 libfreetype.so
out/                     Python reference dumps (ref-wheel/, ref-native-ft2.13.3/), parity logs/JSON
dl/                      downloads: tarballs + .sig, GnuPG keyring, freetype-py sdist, OTF test fonts
emsdk/                   the pinned emscripten SDK (created by build.sh)
```

## Rebuild

```sh
./build.sh                                   # FreeType 2.13.3 + lean HarfBuzz 8.3.0 -> dist/ft2.13.3-hblean
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
`build/src/harfbuzz-8.3.0/COPYING` verbatim, for example as `licenses/harfbuzz.txt` next to
the wasm, with a link from the credits. FreeType's `src/autofit/ft-hb.c` carries the same
notice (Red Hat / Google). A credits block could read:

> Font rasterisation: FreeType 2.13.3 (FreeType License) with HarfBuzz 8.3.0 (Old MIT licence; see licenses/harfbuzz.txt).
> Portions of this software are copyright © 2024 The FreeType Project (www.freetype.org). All rights reserved.

## What remains to port (writer), and pitfalls

None of this needs FreeType. It needs a small OpenType table reader in JS: `head`, `cmap`,
`kern`, `GPOS` and `GSUB`.

### Kerning: `extract_kerning_fonttools` + `derive_kern_classes`

**Sources.** Two sources are read:
- The legacy `kern` table: every format-0 subtable fontTools exposes. Coverage bits (horizontal,
  cross-stream, minimum, override) are ignored.
- GPOS: every lookup of every `kern` feature, under all scripts and language systems. Only
  lookup type 2 is used, with type-9 extension wrappers unwrapped. The effective type comes from
  `ExtensionLookupType`. Contextual kerning (type 8) is dropped silently.
- Only `Value1.XAdvance` is used. Value2, XPlacement and Device/VariationIndex records are
  ignored.

**Summing.** Values are summed over everything: across lookups, across subtables in a lookup,
and across `kern` + GPOS. That is not OpenType's rule. Within one lookup, the first subtable that
matches a pair wins, which is how exceptions override class kerning: a format-1 pair subtable
comes before format-2 class subtables.
- `test/kern_semantics.py` compares the converter's sum with a first-match evaluation at 14 pt.

| font | pairs that differ |
|---|---:|
| Literata Regular | 3,973 |
| Bitter Regular | 3,750 |
| Merriweather | 803 |
| Lora | 354 |
| Noto Serif | 155 |
| SourceSerif4 | 101 |
| Spectral | 26 |
| Charis, GentiumBookPlus | 0 |

- Example: SourceSerif4 `Vã` ships −61/16 px (−3.8 px) where the font means −23/16 (−1.4 px).
- This looks like a real over-kerning bug in the current converter. For byte parity the browser
  must copy it. The better fix is to change Python and JS together, to first match per lookup
  and to ignore `kern` when GPOS has a `kern` feature. HarfBuzz uses `kern` only when GPOS has
  no kerning. None of the fonts tested has a `kern` table.

**Rounding.** `fp4_from_design_units` uses Python `round()`, which rounds half to even.
`Math.round` rounds half up, so JS needs its own half-even rounding.
- Exact halves really occur. At 12 pt the ppem is exactly 25.0, and a font with 2048 units per
  em gets an exact binary scale: du = 64 gives 12.5. Python stores 12, `Math.round` gives 13.
- Compute `ppem = size * 150.0 / 72.0`, `scale = ppem / upem`, then `du * scale * 16` in
  exactly that order, so the doubles come out bit-identical.
- After rounding, clamp to [−128, 127] and drop pairs that end up 0. That happens per size, so
  the class tables differ by size.

**Codepoints.**
- The codepoint set is every emitted glyph. That includes default-ignorables the font maps (they
  get kerning pairs too) and every alias: all codepoints sharing a glyph get every combination.
  SMP codepoints are dropped after the expansion.
- The cmap is fontTools' `getBestCmap()`, which prefers (3,10), (0,6), (0,4), (3,1), (0,3),
  (0,2), (0,1), (0,0). That is not necessarily the charmap FreeType picked; they agree for normal
  Unicode fonts.

**Classes and matrix.**
- Classes come from identical rows over the sorted right codepoints, and likewise for columns.
  Class ids are given in ascending-codepoint order of first appearance.
- If there are more than 255 left or right classes, the style's kerning is dropped entirely.
- Each class entry is `<HB`: 3 bytes, no padding. The matrix is int8, row-major.

**Practical notes.**
- Format-2 expansion is classes × glyphs. Bitter gives about 282k raw pairs, so use integer keys
  like `l << 16 | r`, not strings.
- How fontTools reads the `kern` table, which a JS reader must copy:
  - When a table has exactly one format-0 subtable, the length field is ignored and the length
    is computed from nPairs. This is because of fonts that overflow the length past 10,920 pairs,
    and others like OpenSans/Calibri.
  - A pair that repeats inside one subtable keeps its last value (a dict assignment). Values from
    different subtables are summed.
  - Apple version-1 headers are read, but only format-0 subtables are used. Format 2 is skipped.

### Ligatures: `extract_ligatures_fonttools`

**Lookups.**
- Only GSUB features `liga` and `rlig`, never `dlig` or `clig`, from all scripts.
- Lookup type 4, with type-7 extensions unwrapped.
- Lookups are iterated over a Python `set`. When two lookups map the same sequence differently,
  the later one wins, and in CPython that is roughly ascending lookup index.

**Glyph-to-codepoint map.**
- It is built from the **whole** best cmap, not just the requested codepoints, and the last
  codepoint wins for aliased glyphs (ascending iteration).
- That codepoint must then be in the requested set, or the ligature is dropped.
- When the ligature glyph has no cmap entry, `STANDARD_LIGATURE_MAP` is the fallback (ff, fi, fl,
  ffi, ffl, ſt, st).

**Filtering and chaining.**
- The output and every input must be in the requested set and be ≤ U+FFFF.
- A 3+ character ligature is kept only as a chain through an existing prefix ligature: ffi needs
  ff. It is stored as `(prefixLigCp << 16) | last`.

**Order and size.**
- Pairs are stable-sorted by the packed key, with all two-character pairs inserted first. They
  are truncated to 255 **after** sorting.
- Each entry is `<II`.

### Synthetic bold

`--synthetic-bold` is opt-in; `build-sd-fonts.py` never passes it. If style 1 or 3 is
pixel-identical to its base for 'A', 'a', 'g' and '0', every glyph gets a 3×3 max-dilation of the
2-bit pixels. Only the UI needs this, and only if it offers "fake bold".

### File writer: `generate_cpfont_multistyle` / `pack_style_sections`

**Header and TOC.**
- 32-byte header `<8sHHB19s`: `CPFONT\0\0`, version 4, flags = 1, styleCount.
- A 32-byte TOC entry per style, in ascending style id: `<B3xIIBhhHHBBBI4x`. The style data
  offset is absolute.

**Per-style sections**, back to back with **no alignment padding**:
1. intervals: `<III` start, end, running glyph index
2. glyphs: `<BBHhhH2xI`
3. kern left
4. kern right
5. kern matrix
6. ligatures
7. bitmaps

**Range checks.** Python's `struct.pack` raises on out-of-range values; `DataView` silently wraps
them. Add explicit checks for:
- glyph width and height ≤ 255
- data_length ≤ 65535
- advanceX ≤ 0xFFFF (12.4)
- left and top as int16
- advanceY ≤ 255. Python checks this one explicitly and exits.

The file name is `<name>_<size>.cpfont`. The name is the regular file name with a
`-Regular`/`-Bold`/… suffix removed, unless `--name` is given.

### Inputs the browser cannot reproduce byte for byte, and other caveats

- **Variable fonts.** `build-sd-fonts.py` makes static instances with `fontTools.instancer`
  (Bitter, Inter, Alegreya, Vollkorn, Arimo, Gelasio).
  - FreeType's own `FT_Set_Var_Design_Coordinates` gives slightly different outlines, because
    fontTools rounds coordinates to integers. It also does not bake GPOS variations into the
    kerning.
  - Suggestion: reject fonts with an `fvar` table and ask for static files, or accept that they
    will not match.
- **Not supported by this build:**
  - WOFF/WOFF2: zlib and brotli are left out.
  - Fonts with embedded bitmap strikes at the target ppem (EBDT/CBDT/sbix). FreeType returns the
    strike, and Python would misread it too.
  - Apple fonts that have `morx` but no GSUB: lean HarfBuzz drops AAT, so these could hint
    differently.
  - TrueType collections (TTC): the face index is plumbed through but untested.
- **Shaping language.** The auto-hinter shapes blue-zone characters in HarfBuzz's default
  language, which comes from `setlocale(LC_CTYPE)`. Python ran under `en_US.UTF-8` here, and
  the wasm side has emscripten's C locale; that made no difference on any test font. In theory, a font with
  `locl` rules on blue characters could hint differently when the Python converter runs under
  another locale.
- **Pin freetype-py exactly.** `requirements.txt` says `freetype-py>=2.5.1`. A future freetype-py
  bundling FreeType 2.14.x would silently break parity, because the auto-hinter changed; the old
  prototype measured 92 % with 2.14.3. Suggest `freetype-py==2.5.1`.
- **Serving.** Serve `ftwasm.wasm` as `application/wasm`, or fetch the bytes yourself and pass
  `{wasmBinary}`, for example from the hashed SD web bundle. Load `ftwasm.mjs` in a module worker
  (`new Worker(url, {type: 'module'})`). The device web server probably does not gzip, so plan
  for 608 KB over WiFi.
