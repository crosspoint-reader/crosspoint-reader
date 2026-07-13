# Dictionaries

CrossPoint's dictionary lookup (Reader Menu → **Dictionary**) reads `.cpd`
("CrossPoint Dictionary") files from the `dictionaries/` folder on the SD
card. The first `.cpd` file found there is used.

## Getting a dictionary

Any StarDict (`.ifo` + `.idx[.gz]` + `.dict[.dz]`) or dictd
(`.index` + `.dict[.dz]`) dictionary can be converted with
`scripts/make_dictionary.py` (Python 3, standard library only):

```bash
# StarDict input
python scripts/make_dictionary.py path/to/dictionary.ifo \
    -o eng-tur.cpd --lang en-tr --title "English-Turkish (FreeDict)"

# dictd input
python scripts/make_dictionary.py path/to/dictionary.index \
    -o eng-eng.cpd --lang en-en
```

Copy the resulting `.cpd` file to `dictionaries/` on the SD card.

Free, redistributable sources:

- [FreeDict](https://download.freedict.org/dictionaries/) — bilingual pairs
  in both StarDict and dictd format (GPL).
- Webster's 1913 and GCIDE — public-domain English dictionaries, widely
  available in StarDict format.

### Turkish and other special-casing languages

Pass `--turkish-keys` when the **headwords** are Turkish so that dotted and
dotless I normalize correctly (`I` → `ı`, `İ` → `i`):

```bash
python scripts/make_dictionary.py tur-eng.ifo -o tur-eng.cpd --turkish-keys --lang tr-en
```

The flag is stored in the file header, and the firmware applies the same
mapping to the selected word at lookup time.

## Lookup behavior

- Headword matching is case-insensitive (Latin script, including Latin-1 and
  Latin Extended-A) and ignores punctuation around the selected word.
- On an exact miss, the lookup falls back to the **longest indexed key that
  is a prefix** of the word (minimum 3 bytes). Inflected forms like
  *kitaplarımızdan* then resolve to the *kitap* entry. The matched headword
  is shown as the popup title, so fallback results are always transparent.
- Definitions longer than 8 KB are truncated at conversion time.

## The .cpd format

`.cpd` is designed around the device's memory budget: lookups binary-search a
sorted fixed-width index **directly on the SD card** (one 36-byte read per
probe, ~16 probes for a 36k-word dictionary), so RAM usage is independent of
dictionary size.

All integers are little-endian:

| Offset | Size | Field |
|-------:|-----:|-------|
| 0 | 4 | magic `"CPD1"` |
| 4 | 4 | `entryCount` (uint32) |
| 8 | 4 | `indexOffset` (uint32, absolute) |
| 12 | 4 | `entriesOffset` (uint32, absolute) |
| 16 | 1 | `keyLen` (uint8, currently 28) |
| 17 | 1 | `flags` (bit 0: Turkish key normalization) |
| 18 | 2 | reserved |
| 20 | 12 | language pair, NUL-padded (e.g. `en-tr`) |
| 32 | 64 | title, UTF-8, NUL-padded |

The index is `entryCount` records of `{ char key[keyLen]; uint32 offset;
uint32 length; }`, sorted by `memcmp()` order of `key`; `offset` is relative
to `entriesOffset`. Keys are the lowercased, UTF-8-encoded headwords
truncated to `keyLen` bytes; distinct headwords whose truncated keys collide
are merged into one entry at conversion time. The definitions blob is
concatenated UTF-8 plain text.

The reader implementation is `lib/Dictionary/DictionaryStore.{h,cpp}`, with
host unit tests in `test/dictionary/`.
