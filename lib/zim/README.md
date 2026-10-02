# lib/zim — openZIM reader

Reads Kiwix `.zim` archives (and split `.zimaa`/`.zimab`… sets) on the X4 Pro
and on a computer. GPL-3.0-or-later. Written from the openZIM format
description; no code from libzim or other readers. Vendored decoders:
zstd 1.5.7 single-file decoder (BSD, `src/third_party/zstd/LICENSE`) and
xz-embedded 2024-03-22 (0BSD, `src/third_party/xz/COPYING`).

| Path | What |
|---|---|
| `src/` | the library (device + host): `ZimArchive`, `ZimSource` (incl. `SplitSource`), `ZimDecompress` |
| `host/` | macOS/Linux file access and split-part discovery |
| `tools/zimcat.cpp` | command-line tool |
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
