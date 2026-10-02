# lib/zim — openZIM reader

Reads Kiwix `.zim` archives (and split `.zimaa`/`.zimab`… sets) on the X4 Pro
and on a computer. GPL-3.0-or-later. Written from the openZIM format
description; no code from libzim or other readers. Vendored decoders:
zstd 1.5.7 single-file decoder (BSD, `src/third_party/zstd/LICENSE`) and
xz-embedded 2024-03-22 (0BSD, `src/third_party/xz/COPYING`).

| Path | What |
|---|---|
| `src/` | the library (device + host): `ZimArchive`, `ZimSource` (incl. `SplitSource`), `ZimDecompress`, `ZimFold` (search-key folding), `ZimTitleIndex` (search index reader) |
| `host/` | macOS/Linux file access, split-part discovery, `TitleIndexWriter` |
| `scripts/gen_fold_table.py` | regenerates `src/ZimFoldTable.inc` |
| `tools/zimcat.cpp`, `tools/zimindex.cpp` | command-line tools |
| `test/` | GoogleTest suite over openZIM's `zim-testing-suite` |

## Build and test on a computer

```sh
cmake -S lib/zim -B build/zim -DCMAKE_BUILD_TYPE=Release
cmake --build build/zim -j
ctest --test-dir build/zim --output-on-failure
```

Add `-DZIM_SANITIZE=ON` for AddressSanitizer + UBSan (CI does).

## zimcat

```sh
zimcat wikipedia_en_all_nopic.zim --info
zimcat wikipedia_en_all_nopic.zim "Forbidden City" > forbidden_city.html
zimcat wikipedia_en_all_nopic.zimaa "Forbidden City"      # split copy
zimcat wikipedia_en_all_nopic.zim --search "Forb" 10
zimcat wikipedia_en_all_nopic.zim --path C/Forbidden_City
```

Timings for open, lookup and read go to stderr.

## zimindex

Builds the search index the device uses: a `.pltitles` file next to the ZIM,
keyed by titles folded case- and accent-insensitively (format in
`src/ZimTitleIndex.h`).

```sh
zimindex wikipedia_en_all_nopic.zim                 # writes wikipedia_en_all_nopic.pltitles
zimindex wikipedia_en_all_nopic.zim --search "forbidden ci" 10
```

English Wikipedia (19.2 M titles) gives an index of about 160 MB and needs
about 1 GB of memory and a few minutes to build.
