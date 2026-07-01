# Dictionary Development Guide

Guide for developers working on the dictionary feature: test infrastructure, tooling, and workflows.

For the user-facing dictionary guide, see [dictionary.md](dictionary.md).

## StarDict Format Overview

The dictionary feature uses the StarDict format. Relevant file types:

| File | Required | Purpose |
|------|----------|---------|
| `.dict` | Yes | Definition data (plain text or HTML) |
| `.idx` | Yes | Word index (sorted headwords + offsets into .dict) |
| `.ifo` | Recommended | Metadata (bookname, wordcount, sametypesequence, etc.) |
| `.syn` | Optional | Alternate forms / synonyms (maps to .idx ordinals) |
| `.dict.dz` | Optional | Gzip-compressed .dict (decompressed on-device before use) |
| `.syn.dz` | Optional | Gzip-compressed .syn |
| `.idx.fpi` | Generated | Fenced Prefix Index — sidecar index over `.idx` that narrows a lookup to a short byte range using a fraction of the sector reads a plain binary search would need; the exact-match lookup fast path for `Dictionary::locate`, and (via its per-group cumulative-ordinal field) the ordinal-to-word resolution used by `Dictionary::wordAtOrdinal`/`findSimilar`. Falls back to a full linear scan when absent. See [Fenced Prefix Index (.fpi)](#fenced-prefix-index-fpi) below. |
| `.syn.fpi` | Generated | Same format as `.idx.fpi`, over `.syn`; the exact-match lookup fast path for `Dictionary::resolveAltForm`. |

Minimum for lookup: `.dict` + `.idx`. Without `.ifo`, HTML definitions render as plain text (no `sametypesequence` detection).

## Definition Rendering Pipeline

The definition viewer (`DictionaryDefinitionActivity`) renders one page at a time, so peak RAM is bounded by a single page regardless of how large the definition is (a multi-megabyte hostile entry cannot OOM the device). The pipeline is fully streamed:

1. **Parse (`DictHtmlRenderer`, lib):** expat parses the `.dict` HTML entry in 512-byte chunks and **streams** each finished styled span to a sink (`renderFromFileStreaming`). The whole-definition text buffer and span vector are never materialized — `pendingText` (one span) is the only scratch.
2. **Wrap (`DictLayout::Wrapper`, `src/util`):** spans are word-wrapped into display lines as they arrive. A `LineSink` keeps only the **target page's** lines (discarding the rest as they are produced) and counts the total for the page indicator. Width measurement is injected (`Measurer`), so the wrap logic is renderer-independent and host-unit-testable.
3. **Pool (`TextPool`, `src/util`):** the kept page's segment text is copied into one per-page buffer; segments reference it by `{offset, len}` instead of owning a `std::string` each (fewer, larger allocations → less heap fragmentation over a long session).

**Paging re-parses from the definition start every turn** (both forward and back) — there is no persistent parser kept alive across turns. Per-turn cost is one parse, invisible against the 1–2 s e-ink refresh; this was a deliberate choice for consistent forward/back paging speed (the kept-alive-forward optimization was declined).

**Back-navigation chain (`LookupChain`, `src/util`):** chained lookups keep a compact back-stack — `{history-index, page}` per entry, not owned headword strings — bounded by the Dictionary History Limit. The headword is resolved from the persisted lookup history on back-nav. See its header for the two load-bearing invariants (distance-from-newest addressing; non-contiguous-subset under back-then-forward).

## Test Infrastructure Layout

```
test/
  data/
    dictionary-sources/               # JSON source-of-truth files (one per test dict)
    dictionary-epub-chapters/          # HTML chapter files for the test epub (sorted by filename)
    generate_dictionaries.py           # JSON sources -> test/dictionaries/
    generate_dictionary_test_epub.py   # HTML chapters -> test/epubs/test_dictionary.epub
  dictionaries/                        # generated StarDict binary output
  epubs/                               # generated test epub
  dict-html-renderer/                  # host-side smoke test: DictHtmlRenderer (parser)
    DictHtmlRendererTest.cpp
    run.sh
    README.md
  dict-layout/                         # host litmus: DictLayout wrap/pagination + page-collector
    DictLayoutTest.cpp
    run.sh
  lookup-chain/                        # host litmus: LookupChain back-stack invariants
    LookupChainTest.cpp
    run.sh

scripts/
  dictionary_tools.py                  # standalone CLI: prep, lookup, merge
```

## Test Dictionaries

Test dictionaries, grouped by purpose:

### Lookup content (used for word lookups in test chapters)

| Name | Used in | Purpose |
|------|---------|---------|
| `english-full` | Ch 6-12, 14-15, 20 | Main test dict: 26 headwords + 22 synonyms |
| `english-no-syn` | Ch 16, 23 | No .syn file — verifies alt-form path is skipped |
| `en-es` | Ch 17 | Bilingual English-to-Spanish |
| `phrase` | Ch 13 | Multi-word phrase entries |
| `html-definitions` | Ch 18 | HTML definitions (sametypesequence=h) |
| `ipa-phonetic` | Ch 19 | IPA Unicode character rendering |
| `chain-stress` | Ch 22 | Chained-OOM stress: 5 large (~22 KB text / ~30 pp) style-dense HTML entries that cyclically name the next headword — deep chaining + heap-flat acceptance. Synthetic (`word_prefix: chain_stress`). |
| `fuzzy-fpi` | host tests | Multi-page synthetic (1500 words) used by `DictEngineTest` to exercise `.fpi` bracket binary search in `Dictionary::findSimilar`. |

### Pre-processing (Ch 4-5)

All prefixed `prep-` for alphabetical grouping in the on-device picker.

| Name | Purpose |
|------|---------|
| `prep-gen-idx` | Generate .idx.fpi only |
| `prep-gen-syn` | Generate .syn.fpi only (ships with .idx.fpi pre-built) |
| `prep-extract-dict` | Decompress .dict.dz only |
| `prep-syn-two-step` | Decompress .syn.dz + generate .syn.fpi (ships with .idx.fpi pre-built) |
| `prep-all` | All 4 steps (100k words, ~5 min on device) |
| `prep-mini` | All 4 steps, small (quick — per-book test) |
| `prep-long` | All 4 steps, medium (cancel test — 1-2 min) |
| `prep-fail-decompress` | Corrupt .dz — error handling |
| `prep-fpi-prefix-collision` | `.fpi` prefix-collision edge cases (Ch 4, 21); intentionally ships without `.idx.fpi` so the device generates it during prep |

### Scanner/picker validation (Ch 3)

| Name | Purpose |
|------|---------|
| `no-ifo` | Missing .ifo — still appears in picker |
| `only-dict` | Missing .idx — hidden from picker |
| `multi-idx` | Multiple .idx files — hidden from picker |
| `multi-ifo` | Multiple .ifo files — hidden from picker |
| `overflow-fields` | Long .ifo field values — wrapping test |
| `macos-metadata` | macOS metadata files (`.DS_Store`, `._*` AppleDouble) — scanner must filter these and still show the dict |

## How to Add or Edit a Test Dictionary

1. Create or edit a JSON file in `test/data/dictionary-sources/`.

2. JSON schema — data-driven dictionary:
   ```json
   {
     "meta": {
       "name": "my-dict",
       "bookname": "My Test Dictionary",
       "output_dir": "test/dictionaries/my-dict",
       "entry_format": "m",
       "compress": false,
       "generate_fpi": true
     },
     "entries": [
       {"headword": "apple", "definition": "A round fruit."},
       {"headword": "bridge", "definition": "A structure over water."}
     ],
     "synonyms": [
       ["apples", "apple"]
     ]
   }
   ```

   Key `meta` fields:
   - `name`: stem for output files (e.g. `my-dict.idx`, `my-dict.dict`)
   - `output_dir`: path relative to workspace root
   - `entry_format`: `m` (plain text) or `h` (HTML)
   - `compress` / `compress_dict` / `compress_syn`: produce `.dz` files
   - `generate_fpi` / `generate_idx_fpi` / `generate_syn_fpi`: produce `.idx.fpi` /
     `.syn.fpi`. Convention: any dictionary used for actual lookup testing should
     set `generate_fpi: true` so it matches the deployed format and exercises the
     device's real fast path. The exception is dictionaries explicitly testing
     on-device prep-step generation (e.g. `prep-mini`, `prep-fpi-prefix-collision`),
     which intentionally ship without generated sidecars so the device (or the
     test itself) builds them.
   - `generate_ifo`: write `.ifo` (default true)
   - `generate_idx`: write `.idx` (default true)
   - `corrupt_dict`: write invalid bytes as `.dict.dz`
   - `base_entries`: name of another JSON to inherit entries from

3. JSON schema — synthetic dictionary (algorithmically generated):
   ```json
   {
     "meta": { "name": "...", "bookname": "...", "output_dir": "..." },
     "synthetic": {
       "word_prefix": "word",
       "syn_prefix": "syn",
       "word_count": 1000,
       "synonyms_per_word": 4
     }
   }
   ```

4. Regenerate:
   ```bash
   # Single dictionary
   python3 test/data/generate_dictionaries.py test/data/dictionary-sources/my-dict.json

   # All dictionaries
   python3 test/data/generate_dictionaries.py --all
   ```

## How to Edit the Test EPUB

The test EPUB (`test/epubs/test_dictionary.epub`) is generated from HTML chapter files. Never edit the EPUB directly.

1. Edit HTML in `test/data/dictionary-epub-chapters/`:
   - `cover.html`, `toc_notice.html` — front matter
   - `ch01_*.html` through `ch23_*.html` — chapters (sorted by filename)

2. Regenerate:
   ```bash
   python3 test/data/generate_dictionary_test_epub.py
   ```

3. Chapter files use `<em>dictionary-name</em>` to reference test dictionary folder names. If you rename a dictionary, update all chapter references.

## Host-Side Smoke Test

Tests the `DictHtmlRenderer` library on the host (no device required).

```bash
bash test/dict-html-renderer/run.sh
```

Requires `gcc` and `g++`. The build supplies host stubs for the device-only
`HalStorage.h` / `Logging.h` (see `test/dict-html-renderer/stubs/`) so the
file-based render paths compile and run off-device. Runs 15 tests:
- 7 dictionary entry tests against the `html-definitions` dictionary
- 1 streaming-parity test (`renderFromFileStreaming` vs batch `render`, all entries)
- 5 boundary condition tests (malformed XML, large input, long paragraph, deep nesting, control chars)
- 2 IPA utility unit tests (`isIpaCodepoint`, `splitIpaRuns`, incl. added codepoints + combining marks)

See `test/dict-html-renderer/README.md` for details.

Two further host litmuses cover the layout side (no device required):
- `bash test/dict-layout/run.sh` — `DictLayout` wrap/pagination + page-collector vs a reference oracle.
- `bash test/lookup-chain/run.sh` — `LookupChain` back-stack invariants (distance-from-newest, non-contiguous subset, eviction).

## Standalone CLI Tools

These live in `scripts/` and work independently of the test infrastructure.

### dictionary_tools.py

Offline pre-processing, lookup, and merging for StarDict dictionaries:

```bash
# Pre-process a dictionary (decompress + generate offset files)
python3 scripts/dictionary_tools.py prep test/dictionaries/english-full

# Look up a word
python3 scripts/dictionary_tools.py lookup test/dictionaries/english-full apple

# Merge multiple dictionaries into one
python3 scripts/dictionary_tools.py merge \
  --source /path/to/dict-a \
  --source /path/to/dict-b \
  --output /path/to/merged-dict
```

#### Subcommands

| Subcommand | Purpose |
|------------|---------|
| `prep` | Decompress `.dict.dz`/`.syn.dz` and generate `.idx.fpi`/`.syn.fpi` Fenced Prefix Index sidecars. Replicates on-device `DictPrepareActivity` behavior. |
| `lookup` | Exact-match word lookup in a prepared dictionary. Prints the definition to stdout. |
| `merge` | Combine two or more StarDict dictionaries into a single monolithic dictionary. |

#### merge details

`merge` reads `.idx`, `.dict`, `.syn`, and `.ifo` from each `--source` folder and writes a complete StarDict dictionary to `--output`. The output folder name becomes the file stem (e.g. `--output /tmp/merged` produces `merged.idx`, `merged.dict`, etc.).

Behavior:
- **Headwords**: Full union of all source headwords, sorted case-insensitively.
- **Definitions**: When the same headword appears in multiple sources, definitions are concatenated in source order.
- **Synonyms**: Full union -- all synonyms from all sources are preserved, with target indices remapped to the merged headword index.
- **sametypesequence**: Inherited from the first source. A warning is printed if sources disagree.
- **Generated files**: `.idx.fpi` and `.syn.fpi` are produced automatically.
- **Requirements**: Source dictionaries must have decompressed `.dict` files (run `prep` first if needed). No external dependencies -- stdlib only.

## Fenced Prefix Index (`.fpi`)

### Motivation

`.idx`/`.syn` are flat, alphabetically-sorted lists — correct to binary-search
in principle, but not in practice on an SD card, where each comparison during
the search needs its own word, and each word can require a fresh 512-byte
sector read. SD card reads are orders of magnitude slower than RAM reads, so
the on-device lookup cost is dominated by the *number of sectors read*, not
CPU work. The `.fpi` sidecar exists to minimize that sector-read count: it
holds a compact, front-coded copy of the words at fixed sector boundaries, so
most of the binary search over `.idx`/`.syn` can be done against the small
sidecar instead of the source file, collapsing the search to a short byte
range before a single final linear scan.

### Design and benchmark history

The `.fpi` format is a single combined sidecar — a coarse top-level prefix
table (`tab1_3`) plus a sector-fenced, front-coded prefix table (`fencep_6`) —
tuned from a family of candidates benchmarked in
`scripts/dict_index_benchmark.go` against the previous `.cspt`/`.oft`
sidecars (see the `log` file at repo root for the full sector-read
comparison, and
[this gist](https://gist.github.com/rmmh/2c3d3d4382e9afadf3ef0c9ddcebf434)
for the writeup of the benchmark methodology and results). Across the
benchmarked GCIDE dictionary (108k entries, 1.9MB `.idx`), `fencep_6-tab1_3`
cut the median sector reads per lookup from 19 (`.oft`) / 12 (`.cspt`) down to
3 — the best size/speed tradeoff among the combinations tried — while adding
only ~30KB of sidecar data (1.6% of the `.idx` it indexes).

Two flavors exist with identical format:

- `.idx.fpi` — over `.idx`. `Dictionary::locate` tries it; on absence/failure it
  falls straight to a full linear scan. Its per-group ordinal field also backs
  `Dictionary::wordAtOrdinal` (via `binarySearchFpiOrdinal`), which
  `resolveAltForm`/`findSimilar` use for ordinal-to-word resolution and
  neighbourhood windowing.
- `.syn.fpi` — over `.syn`. `Dictionary::resolveAltForm` tries it the same way
  for exact-match lookup (its ordinal field is generated but unused).

Three producers must stay in sync (each emits both flavors):

- Device: `Dictionary::generateFpi` (parameterized by `skipPerEntry`), called from
  `DictPrepareActivity::generateFpi`; read side is `Dictionary::binarySearchFpi` /
  `Dictionary::binarySearchFpiOrdinal`
- Host CLI: `_build_fpi` / `_search_fpi` in `scripts/dictionary_tools.py`
- Test fixtures: `build_fpi` in `test/data/generate_dictionaries.py`

No self-description beyond a single version byte — prefix lengths are fixed
`constexpr`/module-level constants in each producer, not stored in the file.
`sectorCount` (`ceil(srcFileSize / 512)`) and `tabEntryCount` are always
recomputed from the already-open source file's size, never stored.

**Sector 0 (bytes 0–511): version byte + tab table**

| Offset | Size | Field |
|--------|------|-------|
| 0 | 1 | version (`0`) |
| 1 | 511 | tab entries: up to 170 × 3-byte lowercase prefixes (zero-padded), evenly sampled across source-file 512-byte sectors |

**Sectors 1..N: fencep sidecar**

For every 512-byte sector of the source file, a "group" of 8 sectors' fence
data (61 bytes: 8 relative-offset low bytes + 1 high-bit mask byte + 8 × 6-byte
front-coded prefix slots + a 4-byte LE cumulative-ordinal field) is packed 8
groups (covering 64 source sectors) per 512-byte sidecar sector, zero-padded.
A slot's high bit set means "front-coded against the previous local sector's
word in the same group" (`0x80 | commonLen` + a 5-byte suffix); otherwise it's
a raw zero-padded 6-byte prefix. A single long source entry can be the fence
word for multiple consecutive sectors (mirrors `sort.Search` semantics from
the benchmark reference). The ordinal field holds the 0-based entry index of
the group's `local==0` sector's fence word (or the total entry count as a
sentinel, for trailing sectors past the last entry) — it's written once per
group regardless of how many sectors that entry fences, and is only consumed
by `binarySearchFpiOrdinal`.

### Lookup (exact match)

1. Read sector 0 (one read) and binary-search the tab table by 3-byte prefix,
   widening to the full run of ambiguous (prefix-truncated) matches, to get a
   coarse source-sector range.
2. Iteratively read the fencep sidecar sector covering the midpoint of that
   range and narrow it — distinguishing a "confirmed complete" fence word (an
   embedded zero byte terminates it early, so it's directly comparable) from an
   "ambiguous" one (the 6-byte window is fully packed, so it can only be used
   as a one-sided bound) — until it collapses to a single source-file sector.
3. Linear-scan that one sector's worth of `.idx`/`.syn` bytes for the exact word.

### Lookup (ordinal, `.idx.fpi` only)

`Dictionary::wordAtOrdinal` (used by `resolveAltForm` to turn a `.syn` record's
literal "original word index" into a headword, and by `findSimilar` to window
its fuzzy scan) resolves an ordinal via `binarySearchFpiOrdinal`:

1. Binary-search groups by direct 4-byte seeks to each candidate's ordinal
   field — no full-sector reads, no front-coding decode — to find the last
   group with `ordinal <= target`.
2. Read that group's `local==0` rel/mask bytes to compute its fence byte offset,
   giving a `(byteOffset, ordinal)` pair at or before the target.
3. Linear-scan `.idx` forward from that pair to the exact target ordinal.



## Known Limitations

**Multi-word selection cannot span page boundaries.** Only the current page of a definition is ever resident in RAM — this is inherent to the streaming render pipeline (see Definition Rendering Pipeline), which holds one page at a time so large definitions cannot exhaust memory. Words on adjacent pages are therefore not available to extend a multi-word selection past the last word on the current page. Workaround: reduce the reader font size so that more words fit on a single page, perform the phrase lookup, then restore the original font size. A complete fix would require holding more than one page (defeating the RAM bound) and is out of scope for the dictionary feature.

## Naming Conventions

- Dictionary folder names use lowercase hyphenated: `english-full`, `prep-gen-idx`
- JSON source files match folder names: `english-full.json`
- `prep-` prefix groups all pre-processing test dictionaries
- Scanner validation dicts describe their structural defect: `no-ifo`, `multi-ifo`
