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

Minimum for lookup: `.dict` + `.idx`. Without `.ifo`, HTML definitions render as plain text (no `sametypesequence` detection).

## Test Infrastructure Layout

```
test/
  data/
    dictionary-sources/               # 19 JSON source-of-truth files
    dictionary-epub-chapters/          # 22 HTML chapter files for test epub
    generate_dictionaries.py           # JSON sources -> test/dictionaries/
    generate_dictionary_test_epub.py   # HTML chapters -> test/epubs/test_dictionary.epub
  dictionaries/                        # generated StarDict binary output
  epubs/                               # generated test epub
  dict-html-renderer/                  # host-side smoke test
    DictHtmlRendererTest.cpp
    run.sh
    README.md

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
   - `ch01_*.html` through `ch20_*.html` — chapters (sorted by filename)

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

Requires `gcc` and `g++`. Runs 14 tests:
- 7 dictionary entry tests against `html-definitions` dictionary
- 5 boundary condition tests (malformed XML, large input, deep nesting, control chars)
- 2 IPA utility unit tests (isIpaCodepoint, splitIpaRuns)

See `test/dict-html-renderer/README.md` for details.

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
| `prep` | Decompress `.dict.dz`/`.syn.dz` and generate `.idx.oft`/`.syn.oft` offset files. Replicates on-device `DictPrepareActivity` behavior. |
| `lookup` | Exact-match word lookup in a prepared dictionary. Prints the definition to stdout. |
| `merge` | Combine two or more StarDict dictionaries into a single monolithic dictionary. |

#### merge details

`merge` reads `.idx`, `.dict`, `.syn`, and `.ifo` from each `--source` folder and writes a complete StarDict dictionary to `--output`. The output folder name becomes the file stem (e.g. `--output /tmp/merged` produces `merged.idx`, `merged.dict`, etc.).

Behavior:
- **Headwords**: Full union of all source headwords, sorted case-insensitively.
- **Definitions**: When the same headword appears in multiple sources, definitions are concatenated in source order.
- **Synonyms**: Full union -- all synonyms from all sources are preserved, with target indices remapped to the merged headword index.
- **sametypesequence**: Inherited from the first source. A warning is printed if sources disagree.
- **Generated files**: `.idx.oft` and `.syn.oft` are produced automatically.
- **Requirements**: Source dictionaries must have decompressed `.dict` files (run `prep` first if needed). No external dependencies -- stdlib only.

## Known Limitations

**Multi-word selection cannot span page boundaries.** Words on the next page are not loaded into memory, so extending a multi-word selection past the last word on the current page is not possible. Workaround: reduce the reader font size so that more words fit on a single page, perform the phrase lookup, then restore the original font size. A complete fix would require significant changes to the rendering engine and is out of scope for the dictionary feature.

## Naming Conventions

- Dictionary folder names use lowercase hyphenated: `english-full`, `prep-gen-idx`
- JSON source files match folder names: `english-full.json`
- `prep-` prefix groups all pre-processing test dictionaries
- Scanner validation dicts describe their structural defect: `no-ifo`, `multi-ifo`
