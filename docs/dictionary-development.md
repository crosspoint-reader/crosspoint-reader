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
| `.idx.oft` | Generated | Two-level offset index for fast .idx binary search |
| `.syn.oft` | Generated | Two-level offset index for fast .syn binary search |
| `.idx.oft.cspt` | Generated | CrossPoint optimized prefix index over `.idx`; primary fast path for word lookup. Falls back to `.idx.oft` when absent. See [CrossPoint Optimized Index format](#crosspoint-optimized-index-cspt) below. |
| `.syn.oft.cspt` | Generated | CrossPoint optimized prefix index over `.syn`; primary fast path for alternate-form lookup. Falls back to `.syn.oft` when absent. Same format as `.idx.oft.cspt`. |

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
    dictionary-epub-chapters/          # ch01..ch22 HTML chapter files for test epub
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

19 test dictionaries, grouped by purpose:

### Lookup content (used for word lookups in test chapters)

| Name | Used in | Purpose |
|------|---------|---------|
| `english-full` | Ch 6-12, 14-15, 20 | Main test dict: 26 headwords + 22 synonyms |
| `english-no-syn` | Ch 16 | No .syn file — verifies alt-form path is skipped |
| `en-es` | Ch 17 | Bilingual English-to-Spanish |
| `phrase` | Ch 13 | Multi-word phrase entries |
| `html-definitions` | Ch 18 | HTML definitions (sametypesequence=h) |
| `ipa-phonetic` | Ch 19 | IPA Unicode character rendering |
| `chain-stress` | Ch 22 | Chained-OOM stress: 5 large (~22 KB text / ~30 pp) style-dense HTML entries that cyclically name the next headword — deep chaining + heap-flat acceptance. Synthetic (`word_prefix: chain_stress`). |

### Pre-processing (Ch 4-5)

All prefixed `prep-` for alphabetical grouping in the on-device picker.

| Name | Purpose |
|------|---------|
| `prep-gen-idx` | Generate .idx.oft only |
| `prep-gen-syn` | Generate .syn.oft only |
| `prep-extract-dict` | Decompress .dict.dz only |
| `prep-syn-two-step` | Decompress .syn.dz + generate .syn.oft |
| `prep-all` | All 4 steps (100k words, ~5 min on device) |
| `prep-mini` | All 4 steps, small (quick — per-book test) |
| `prep-long` | All 4 steps, medium (cancel test — 1-2 min) |
| `prep-fail-decompress` | Corrupt .dz — error handling |

### Scanner/picker validation (Ch 3)

| Name | Purpose |
|------|---------|
| `no-ifo` | Missing .ifo — still appears in picker |
| `only-dict` | Missing .idx — hidden from picker |
| `multi-idx` | Multiple .idx files — hidden from picker |
| `multi-ifo` | Multiple .ifo files — hidden from picker |
| `overflow-fields` | Long .ifo field values — wrapping test |

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
       "generate_oft": true
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
   - `generate_oft` / `generate_idx_oft` / `generate_syn_oft`: produce `.oft` files
   - `generate_cspt`: produce `.idx.oft.cspt` (requires `generate_idx_oft`).
   - `generate_syn_cspt`: produce `.syn.oft.cspt` (requires `generate_syn_oft`).
     Convention: when `generate_idx_oft`/`generate_syn_oft` are true, also set
     `generate_cspt`/`generate_syn_cspt: true` so fixtures match the deployed
     format and the device's `.cspt` fast path is exercised. The exception is
     dictionaries explicitly testing on-device CSPT generation (e.g.
     `prep-cspt-prefix-collision`), which intentionally ship without `.cspt`.
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
   - `ch01_*.html` through `ch22_*.html` — chapters (sorted by filename)

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
| `prep` | Decompress `.dict.dz`/`.syn.dz`, generate `.idx.oft`/`.syn.oft` offset files, and generate `.idx.oft.cspt`/`.syn.oft.cspt` optimized prefix indexes. Replicates on-device `DictPrepareActivity` behavior. |
| `lookup` | Exact-match word lookup in a prepared dictionary. Prints the definition to stdout. |
| `merge` | Combine two or more StarDict dictionaries into a single monolithic dictionary. |

#### merge details

`merge` reads `.idx`, `.dict`, `.syn`, and `.ifo` from each `--source` folder and writes a complete StarDict dictionary to `--output`. The output folder name becomes the file stem (e.g. `--output /tmp/merged` produces `merged.idx`, `merged.dict`, etc.).

Behavior:
- **Headwords**: Full union of all source headwords, sorted case-insensitively.
- **Definitions**: When the same headword appears in multiple sources, definitions are concatenated in source order.
- **Synonyms**: Full union -- all synonyms from all sources are preserved, with target indices remapped to the merged headword index.
- **sametypesequence**: Inherited from the first source. A warning is printed if sources disagree.
- **Generated files**: `.idx.oft`, `.syn.oft`, `.idx.oft.cspt`, and `.syn.oft.cspt` are produced automatically.
- **Requirements**: Source dictionaries must have decompressed `.dict` files (run `prep` first if needed). No external dependencies -- stdlib only.

## CrossPoint Optimized Index (`.cspt`)

The `.cspt` ("CrossPoint") file is a CrossPoint-specific optimized prefix index. Two flavors exist with identical format:

- `.idx.oft.cspt` — over `.idx`. The on-device `Dictionary::locate` tries it before falling back to `.idx.oft` and then a linear scan.
- `.syn.oft.cspt` — over `.syn`. The on-device `Dictionary::resolveAltForm` tries it before falling back to `.syn.oft` and then a linear scan.

Three producers must stay in sync (each emits both flavors):

- Device: `DictPrepareActivity::generateCspt` (parameterized by `skipPerEntry`) and constants in `src/util/Dictionary.cpp`
- Host CLI: `_build_cspt` in `scripts/dictionary_tools.py`
- Test fixtures: `build_cspt` in `test/data/generate_dictionaries.py`

### Header (12 bytes)

| Offset | Size | Field | Value |
|--------|------|-------|-------|
| 0 | 4 | magic | `"CSPT"` |
| 4 | 1 | version | `1` |
| 5 | 1 | prefixLen | `16` (bytes of headword stored per entry) |
| 6 | 2 | stride | `16` (LE; informational — see note below) |
| 8 | 4 | entryCount | LE |

### Entries (20 bytes each)

| Offset | Size | Field |
|--------|------|-------|
| 0 | 16 | prefix (UTF-8, zero-padded if shorter than `prefixLen`) |
| 16 | 4 | byte offset into `.idx` or `.syn` (LE `uint32`) |

Entries are produced from the `.oft` page boundaries: for each `.oft` page (stride 32), the first headword and the headword 16 entries into the page are sampled. Entries are sorted by case-insensitive prefix in ascending order. The producer's per-entry suffix size differs by source (`.idx` = 8 bytes for offset+size; `.syn` = 4 bytes for the original-word-index), but the resulting `.cspt` format is identical.

### Lookup

Case-insensitive binary search over prefixes finds the largest entry `i` whose prefix is `<= target`. The source file (`.idx` or `.syn`) is then scanned from `entries[i].byteOffset` to `entries[i+1].byteOffset` (or end of source for the last entry).

### `stride` field

`stride` is currently informational. Both readers (`Dictionary::binarySearchCspt`, `_scan_idx` in `dictionary_tools.py`) treat the producer-side stride of 16 as a hard-coded constant. Producers must emit `stride=16`. A future format change that varies stride must increment `version` and update both readers to honor the field.

## Known Limitations

**Multi-word selection cannot span page boundaries.** Only the current page of a definition is ever resident in RAM — this is inherent to the streaming render pipeline (see Definition Rendering Pipeline), which holds one page at a time so large definitions cannot exhaust memory. Words on adjacent pages are therefore not available to extend a multi-word selection past the last word on the current page. Workaround: reduce the reader font size so that more words fit on a single page, perform the phrase lookup, then restore the original font size. A complete fix would require holding more than one page (defeating the RAM bound) and is out of scope for the dictionary feature.

## Naming Conventions

- Dictionary folder names use lowercase hyphenated: `english-full`, `prep-gen-idx`
- JSON source files match folder names: `english-full.json`
- `prep-` prefix groups all pre-processing test dictionaries
- Scanner validation dicts describe their structural defect: `no-ifo`, `multi-ifo`
