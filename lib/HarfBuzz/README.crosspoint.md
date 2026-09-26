# HarfBuzz

CrossPoint shapes complex scripts with
[HarfBuzz 14.5.0](https://github.com/harfbuzz/harfbuzz/releases/tag/14.5.0),
Old MIT license (see `COPYING`).

HarfBuzz is not vendored. `scripts/fetch_harfbuzz.py` downloads the pinned
release, checks its SHA-256, unpacks its `src/` into `upstream/` (git-ignored)
and applies `scripts/harfbuzz_patches/`. PlatformIO builds run it as a
pre-build script and the host tests run it when CMake configures; it does
nothing once `upstream/` is current. Environments that list their own
`extra_scripts` (such as a local simulator environment) need
`pre:scripts/fetch_harfbuzz.py` added, or can run the script once by hand.
Set `HARFBUZZ_TARBALL` to a local copy of the release to build offline.

Only `src/harfbuzz-crosspoint.cc` is compiled: one translation unit holding
the OpenType shaper, its script shapers and the built-in Unicode data. It is
built with the `HB_TINY` profile plus `src/hb-crosspoint-config.h`, which
routes every allocation through CrossPoint's budgeted allocator
(`lib/EpdFont/ComplexShaper.cpp`) and enables the local patch below.

## Updating

1. Change `VERSION` and `SHA256` in `scripts/fetch_harfbuzz.py` and
   `version` in `library.json`.
2. Build once. The fetch stops with an error if a patch no longer applies;
   refresh it against the new release.
3. Update the include list in `src/harfbuzz-crosspoint.cc` if upstream renamed
   or split any of the listed `.cc` files.
4. Run the host tests: `test/complex_shaper` compares CrossPoint's shaping
   with HarfBuzz's own output.

## Local patches

- `0001-drop-subtable-coverage-digests.patch` (`HB_CROSSPOINT_NO_SUBTABLE_DIGEST`)
  removes the per-subtable coverage digest from `hb_applicable_t`. Each
  subtable's own coverage check still runs, so shaping output is unchanged;
  the lookup accelerators shrink from five words per subtable to two, about a
  third of HarfBuzz's heap on an Indic face.
