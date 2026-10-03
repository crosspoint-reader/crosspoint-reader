# Card builder

Builds the Pocket Library microSD card on a Mac from `library.toml`.
Standard-library Python 3.11+ only (macOS's `python3` is fine). It needs
`zimindex` from the dev release next to it.

```sh
python3 cardbuilder.py plan                          # what, how big, does it fit (writes nothing)
python3 cardbuilder.py download                      # fetch + SHA-256 check, resumable
python3 cardbuilder.py index                         # build .pltitles search indexes
python3 cardbuilder.py copy --card "/Volumes/POCKET LIB" --dry-run
python3 cardbuilder.py copy --card "/Volumes/POCKET LIB"
python3 cardbuilder.py all  --card "/Volumes/POCKET LIB"
```

Options: `--only KEY` (one collection), `--verify` (read every copied file
back), `--prune` (delete old editions under `/library`; otherwise they are only
listed), `--staging FOLDER`, `--library FILE`.

## What ends up on the card

```
/library/manifest.json
/library/wikipedia/wikipedia_en_all_nopic_2026-06.zimaa … .zimam   (4,000 MiB parts)
/library/wikipedia/wikipedia_en_all_nopic_2026-06.pltitles          (search index)
/library/wikivoyage/wikivoyage_en_all_nopic_2026-07.zim             (small: kept whole)
```

`manifest.json` lists each collection's title, edition date, language, parts,
SHA-256 and UUID of the whole ZIM, and its index.

## Safety

- Nothing on the card is deleted without `--prune`.
- Every download is checked against Kiwix's published SHA-256. A mismatch
  keeps the file as `.bad` and stops.
- Files are written as `.tmp` and renamed when complete, so an unplugged card
  never holds a half-written part under its real name.

## Tests

```sh
cmake -S lib/zim -B build/zim && cmake --build build/zim -j --target zimindex
ZIMINDEX=build/zim/zimindex ZIM_TEST_DATA_DIR=build/zim/_deps/zim_testing_suite-src/data \
  python3 -m unittest tools/cardbuilder/test_cardbuilder.py -v
```

The tests run a local stand-in for the Kiwix catalog and mirror: newest
edition and flavour selection, a dropped connection resumed, a bad checksum
refused, splitting and reassembly, the manifest, dry run, prune, a full card,
and re-runs that do nothing.
