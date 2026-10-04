# Pocket Library — Decisions

Each entry: date, decision, reason. Measured numbers live in the tables at the
bottom; anything marked *(est.)* is still a guess.

---

## What upstream CrossPoint 1.6.5 gives us (read 2026-10-01)

- **Build.** PlatformIO with the pioarduino ESP32 platform (Arduino core 3.3.11,
  ESP-IDF underneath). Env `x4pro`: board `esp32-s3-devkitc1-n16r8`, OPI PSRAM
  (`dio_opi`), 16 MB flash, `FREEINK_DEVICE_X4PRO`, `USE_BLOCK_DEVICE_INTERFACE`.
  `x4pro-gh_release` is the same with release logging. Partitions: two 6.25 MB
  OTA app slots, 3.4 MB SPIFFS (unmounted), 64 KB coredump.
- **Hardware layer** lives in the `freeink-sdk` submodule (MIT, FreeInk), at
  commit `111fdcc` for this tag. Its `docs/xteink-x4pro-support.md` is a
  bench-verified pin map: SSD1677 *or* UC8179/UC8279 panel (varies by batch,
  detected at boot), GT911 touch (Home key is a GT911 key bit), digital side
  buttons on GPIO0/7, Power GPIO3, CW2017 fuel gauge, BM8563 RTC, warm/cool
  frontlight on GPIO8/9.
- **SD card: native SDMMC, 1-bit, slot 1** (CLK 41, CMD 42, DAT0 40; power
  enable GPIO5, active-low). Mounted as a block device under SdFat. *Not SPI.*
- **Activities.** One `ActivityManager` owns a stack of activities and a single
  render task. List screens derive from `UiListActivity` (FreeInkUI). Children
  open with `startActivityForResult`. There is already an on-screen keyboard
  (`KeyboardEntryActivity`) with layouts, and a StarDict dictionary with a
  word-selection UI (`DictionaryWordSelectActivity`).
- **Storage rule.** All SD access goes through `HalStorage`/`HalFile`, which
  serialize on one mutex. Never call SdFat or `SDCardManager` directly.
- **Memory.** `HalMemory::allocatePsram()` returns PSRAM-only buffers (never
  falls back to internal RAM); `makeUniqueNoThrow` for every `new`. Upstream's
  rules were written for the 380 KB ESP32-C3; on the S3 they're still the
  right hygiene for internal SRAM.
- **Rendering pipeline** (to read in depth at M3): `lib/Epub` parses XHTML with
  expat into pages and caches laid-out sections on SD under `/.crosspoint/`;
  `lib/GfxRenderer` draws; fonts are built-in bitmap fonts plus runtime
  TTF/OTF from the card via FreeType (`lib/EpdFont/TtfEpdFont`).
- **Upstream already has `src/activities/library/` and `lib/LibraryIndex`**
  (its book library). Our code therefore uses `src/pocketlib/` and `lib/zim/`.

## 2026-10-01 — Base on tag 1.6.5, not `main`

1.6.5 is the latest tagged release with `x4pro`. `main` is 17 commits ahead
and has already changed `lib_deps` (adds SdFat, JsonSax, Opds and others).
Release tags are what we rebase onto.

## 2026-10-01 — How we stay mergeable

- New code only in `lib/zim/`, `src/pocketlib/`, `tools/`, and our own docs.
- Our build envs live in `platformio.pocketlib.ini`, pulled in by one edit to
  `platformio.ini` (`extra_configs`). Each env = upstream env + `-DPOCKET_LIBRARY=1`.
- Upstream source files are touched only inside `#ifdef POCKET_LIBRARY`, so the
  stock envs in our fork build byte-for-byte what upstream builds.
- **Rebase procedure** per upstream release: `git fetch upstream --tags`;
  `git rebase --onto <new-tag> <old-tag> pocket-library`; resolve the touch
  points listed below; `git submodule update`; build both stock `x4pro-gh_release`
  and `x4pro-pocketlib-release`; run host tests; device checklist.

### Upstream touch points

| File | Change | Why |
|---|---|---|
| `platformio.ini` | `extra_configs` also lists `platformio.pocketlib.ini` | our envs |
| `CLAUDE.md` | symlink to `AGENTS.md` replaced by our working rules, which import `@AGENTS.md` | brief §1 |
| `src/activities/settings/AboutActivity.{h,cpp}` | `#ifdef POCKET_LIBRARY`: 5 taps on "Firmware" open Diagnostics | hidden debug screen |
| `src/activities/home/HomeActivity.cpp` | `#ifdef POCKET_LIBRARY`: Home's **Library** opens our shelf; CrossPoint's book library is the shelf's last row | the library's front door (M3) |
| `test/CMakeLists.txt` | one `add_subdirectory(pocketlib_article_layout)` | real articles through the layout engine on the host |
| `src/activities/util/KeyboardEntryActivity.{h,cpp}` | `#ifdef POCKET_LIBRARY`: optional live-suggestion rows between the text field and the keys, refilled after each edit; a tapped row (or OK) is reported in the result | search as you type (M4) |
| `src/activities/ActivityResult.h` | `#ifdef POCKET_LIBRARY`: `KeyboardResult::picked` | which suggestion was chosen |
| `freeink-sdk` (submodule, patched at build time) | `SdmmcBlockDevice.{h,cpp}`: 40 MHz with 20 MHz fallback, 32-sector transfers, all behind `POCKET_LIBRARY_SD_FAST` | SD speed |
| `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp` | `#ifdef POCKET_LIBRARY`: with no Epub (an article), an `<img>`'s size comes from its width/height attributes and no file is extracted at layout time | article images (lead image, "Images") |
| `lib/Epub/Epub/blocks/ImageBlock.h` | `#ifdef POCKET_LIBRARY`: `getExtractor()`, so an article opened over a book puts the book's image extractor back | article images |
| `src/activities/util/KeyboardEntryActivity.{h,cpp}` (extended) | `#ifdef POCKET_LIBRARY`: live rows carry a source tag and can refill the field (past searches); scope chips above the rows, the last opening a list | search everything (M6) |
| `src/activities/home/HomeActivity.{h,cpp}`, `src/activities/ActivityManager.h` (`HomeMenuItem::SEARCH`), `src/components/CoverGridHomeUi.{h,cpp}` | `#ifdef POCKET_LIBRARY`: a sixth tab (magnifier) on the Cover Grid home opens search; the list homes are unchanged; its icon is drawn upright (`pocketlib/icons/UprightIcon.h`) | search entry point (owner's choice) |
| `src/activities/boot_sleep/SleepActivity.cpp` | `#ifdef POCKET_LIBRARY`: the Dark and Light sleep screens show DON'T PANIC (a 1-bit picture, `pocketlib/images/DontPanic.h`, lettered in Fredoka, OFL) with "Sleeping" small under it, in place of the CrossPoint logo | owner's request, after the Guide's cover |
| `src/CrossPointSettings.h`, `src/SettingsList.h`, `src/main.cpp`, `src/activities/ActivityManager.{h,cpp}` | `#ifdef POCKET_LIBRARY`: short power button option **Search** (value 6, appended), opening search over whatever is open | search entry point (owner's choice) |
| `src/activities/ActivityManager.{h,cpp}`, `lib/GfxRenderer/GfxRenderer.h` | `#ifdef POCKET_LIBRARY`: a half refresh on every return to Home and on every fourth other screen change (push, pop, replace), never weakening a deeper one already promoted; readers and the control center are left alone. (First version did it on every change: it cleared the ghosting but flashed the panel on each tap; owner report) | ghost text from the previous screen, plainest in the dithered selection bar (owner report) |
| `lib/LibraryIndex/LibraryBuilder.cpp`, `lib/LibraryIndex/LibraryFormat.h`, `src/activities/library/LibraryListActivity.cpp`, `test/CMakeLists.txt` | `#ifdef POCKET_LIBRARY`: the title sort and the letter groups skip a leading "The", "A" or "An" (`TitleSortKey.h`, ours); `CLIX_FOLD_VERSION` 5 so existing indexes rebuild once; one `add_subdirectory(pocketlib_title_sort)` | "The" shouldn't decide where a book files (owner) |

## 2026-10-01 — Licensing layout

- Upstream CrossPoint (MIT) and freeink-sdk (MIT) keep their notices.
- Everything we write is GPL-3.0-or-later; full text in `LICENSE-GPL-3.0`;
  each new file carries an SPDX header. The combined firmware binary is
  distributed under GPL-3.0-or-later, which MIT permits.
- Header copyright line reads "Pocket Library contributors" (owner approved
  2026-10-02).

## 2026-10-01 — Hidden Diagnostics screen

Entrance: Settings → About → tap **Firmware** five times. Shows PSRAM size
(`esp_psram_get_size`), PSRAM and internal heap free/largest block, flash
size and speed, the SD bus width and **real** clock (`sdmmc_host_get_real_freq`,
not a config comment), and an on-demand benchmark:
- picks the largest file under `/library` (two levels) or `/` (one level);
  if none ≥ 16 MB, writes a 32 MB scratch file `/.pocketlib/bench.bin` and
  reports write speed;
- sequential: 16 MB in 64 KB reads into PSRAM;
- random: 64 × 4 KB reads at random aligned offsets across the whole file,
  including backward seeks (exposes FAT-chain walking), avg / p95 / max.
All values are logged on serial with tag `DIAG`.

## 2026-10-01 — Findings that change the brief's assumptions

1. **SD clock is 20 MHz, not 40.** `SdmmcBlockDevice.cpp` sets
   `host.max_freq_khz = SDMMC_FREQ_DEFAULT; // 40 MHz`, but ESP-IDF defines
   `SDMMC_FREQ_DEFAULT` as 20000 (`sd_protocol_types.h:218`). 1-bit × 20 MHz
   ≈ 2.5 MB/s ceiling. Reads also bounce through a 4 KB DMA buffer
   (`kMaxTransferSectors = 8`). Diagnostics will confirm the real clock.
2. **zstd window.** Probing openZIM's 2024 Wikipedia sample: zstd clusters
   decompress to ≤ 2,096,688 bytes, compress to ≤ 267,897 bytes (≈ 8:1), but
   the frames declare an **8 MiB window**. Plan: one-shot decode of a whole
   cluster into a 2 MiB PSRAM buffer (no separate window), with the frame's
   content size checked against the buffer first.
3. **Namespaces and listings.** The 2024 sample exists in both schemes:
   v5.0 with `A/` articles and v6.2 with `C/`. The v6.2 file carries both
   `X/listing/titleOrdered/v0` (all entries) and `.../v1` (front articles
   only, the right list for Random and for "articles only" search).
4. **Older files use xz** (2017 Wikibooks sample, compression type 4). Some
   real-world collections may still be xz; the card builder will report it.
5. **FAT32 seek cost** in 4 GB parts (see PLAN.md risk 1).

## 2026-10-02 — Wiktionary lookups go through StarDict

Owner left the choice to me. The card builder will convert the Wiktionary ZIM
into a StarDict dictionary (`.ifo/.idx/.dict.dz`) on the Mac, and tap-and-hold
in our reader will use CrossPoint's existing StarDict lookup and its
definition screen. Reasons: upstream's lookup already handles case folding,
synonyms and its own sidecar index, and is tested on hardware; a StarDict
entry is a few hundred bytes of plain text, where a Wiktionary article is a
full HTML page in a 2 MiB cluster, so lookups get cheaper and faster; and the
same dictionary works inside ordinary EPUBs too. The Wiktionary ZIM stays on
the card as a browsable shelf for full entries. Revisit if conversion loses
too much (etymologies, translations).

## 2026-10-02 — Corpus is English only; keep a CJK font

Owner wants English collections only, so Chinese Wikipedia is dropped. A CJK
font still goes on the card (SD-card TTF, no firmware cost) because English
articles carry native-script names (紫禁城, 東京, القاهرة); without it they
render as boxes. Card-builder defaults are pending the owner's corpus picks.

## 2026-10-02 — Corpus chosen (owner)

In: Wikipedia, Wiktionary, Wikivoyage, Wikiquote (core); WikiProjectMed
(mdwiki) plus other first-aid / emergency-medicine collections the catalog
offers as text (candidates to verify: WikEM; Kiwix "zimgit" medicine and
post-disaster sets are mostly PDFs, which the reader can't render, so they
are excluded unless that changes); Wikisource; Wikibooks; Standard Ebooks as
EPUBs in `/books`. Out: Stack Exchange, DevDocs, iFixit, Chinese Wikipedia.
All English, text-only (nopic) where an edition exists.

## 2026-10-02 — First device measurements change two plans

- **PSRAM is nearly all ours.** 8,080 KB free with CrossPoint running, so the
  brief's 3 × 2 MiB decompressed-cluster LRU fits with ~1.9 MB to spare.
  Earlier worry (PLAN risk 3) withdrawn; the cache size stays a setting.
- **The card is slow but steady.** 1.93 MB/s sequential and ~1.9 ms per
  random 4 KB read with a tight tail. Projected cost of an uncached article:
  ~250 KB compressed cluster ≈ 130 ms to read, plus decompression (to be
  measured). Search: ~25 probes × ~2 ms ≈ 50 ms if, and only if, seeks in
  4 GB parts stay cheap; that test needs a real 4 GB file.

## 2026-10-02 — Milestone 1: how the ZIM reader is built

- **Source of truth.** wiki.openzim.org is blocked from the cloud session, so
  the reader is written from the format as documented (header, MIME list,
  directory entries, cluster info byte, blob offset tables, title listings)
  and **verified against real files** rather than from a copy of the spec
  page: byte-level probes of the openZIM test files, then a cross-check
  against libzim's Python binding as an independent oracle. 527 randomly
  sampled entries matched byte-for-byte across both namespace schemes, xz
  and zstd clusters, and a 3-part split. Re-read the spec page when the
  network allows and note any differences here.
- **No exceptions, no per-entry memory.** Every lookup binary-searches on the
  card. RAM use is independent of archive size: a dirent read (512 B,
  growing only for very long paths) plus the cluster cache.
- **One-shot decompression.** Each cluster is decoded straight into a buffer
  of its exact size (zstd records it; xz grows from 1 MiB). No decoder
  window is reserved, which matters because Kiwix frames declare 8 MiB to
  128 MiB windows. Clusters above `maxClusterBytes` (default 16 MiB) are
  refused.
- **Uncompressed clusters are never loaded whole**: blobs are read straight
  from the file. This is how the 72 MB title listing of English Wikipedia
  stays usable.
- **Corruption handling.** Every pointer and size is bounds-checked; each
  cluster's whole offset table is validated on first use (caught
  `too_large_offset_of_first_blob_in_cluster`, where only unused entries
  were bad). All 23 tests, including ~25 corrupted files in both schemes,
  pass under AddressSanitizer and UBSan.
- **Title search is byte-wise for now** (case-sensitive). Case/accent
  folding (sidecar index vs. probing variants) is decided in M3/M4 as the
  brief says.
- **Not implemented, on purpose:** MD5 checksum verification (the card
  builder verifies downloads on the Mac instead; hashing 60 GB on the device
  would take hours), zlib/bzip2 clusters (absent from Kiwix files for
  years), Xapian full-text indexes (non-goal).
- **Firmware cost so far: zero.** `lib/zim` is only compiled into the
  firmware once something includes it (M3).

## 2026-10-02 — Real Wikipedia has no full title index; use the article list

Found on the owner's `wikipedia_en_all_nopic_2026-06.zim` (ZIM 6.3,
19,707,096 entries, 178,627 clusters): it has no `X/listing/titleOrdered/v0`
and no header title list, only `v1` (19,191,219 front articles and their
redirects, sorted by title). The reader reported `title index 0` and a title
lookup failed. openZIM's test suite already has this case
(`noTitleListingV0/`).

- When neither full source exists, the title index now *is* the v1 list
  (`TitleSource::Articles`). Searching and opening articles only ever needs
  front articles, so nothing user-facing is lost.
- zimcat also tries the path (title with spaces as underscores) when the
  title misses. The device reader will do the same.
- Answered: v1's ~77 MB blob sits in an uncompressed cluster (the reader
  refuses clusters over 16 MB, and an exact-title lookup across all 19.2 M
  entries took 0.3 ms), so binary search reads it in place on the device.
- Confirmed: the list is byte-ordered, so "forb" lands past every
  capitalised title (on "~"). Case-insensitive search needs the Milestone 2
  sidecar index; probing case variants is not enough.

## 2026-10-02 — Search uses a sidecar title index (`.pltitles`)

Chosen over probing case variants of the query, because Wikipedia titles all
start with a capital letter and the ZIM's own list is ordered byte by byte
("forb" sorts after every capitalised title; see the measurements).

- **Key**: `foldKey(title)`. ASCII is lower-cased. A generated table (Python
  `unicodedata`: NFKD, combining marks dropped, `casefold`, plus a hand list
  for ø/ł/æ/ß/þ and typographic quotes and dashes) covers Latin, Greek,
  Cyrillic and Vietnamese. Whitespace is collapsed. CJK passes through
  unchanged. Keys are capped at 255 bytes. The Mac and the device run the same
  C++, so they cannot disagree; `kFoldVersion` is stored in each index, and
  stale ones are refused.
- **Format**: a static B-tree of 4 KB pages. Leaves are front-coded
  `(key, entry index)` records, sorted, on consecutive pages. Inner pages
  hold each child's first key. The ZIM's UUID and entry count are in the
  header, so an index is never used with the wrong file.
- **What is indexed**: the v1 front-article list (articles plus redirects to
  them). Files without one index the HTML entries and redirects in the
  content namespace.
- **Device cost**: one read per level. Measured on 19.2 M synthetic titles:
  3 inner levels, so 4 reads (3 if the root page stays in RAM), about 8 ms at
  the measured 1.9 ms per 4 KB read. Each result shown costs one directory
  read for its display title.
- **Size and build** (synthetic, 19.2 M titles, cloud x86): 160 MB file,
  1 min 55 s, 918 MB peak memory.
- **Real Wikipedia** (owner's Mac, Intel, file on the SSK drive): 19,191,219
  titles indexed in 2 min 2 s. The index is 269,029,376 bytes, 0.5% of the
  ZIM; real titles share less of their prefixes than the synthetic ones.
  "forbidden ci", "FORB" and "zurich" (finds "Zürich") all work.
- Seen in that run: one target can have a dozen redirects ("FORBA
  Holdings", "FORBA Holdings LLC", ...). The device list should collapse
  redirects that share a target (M4).
- Not yet: ranking. Matches come back in alphabetical order, so "Forb" lists
  "Forbach…" before "Forbidden City". Ranking is decided in M4 with real
  queries, and the format has a version number for it.

## 2026-10-03 — Card builder: one stdlib Python file, `library.toml`

- **TOML, not the brief's YAML.** Python 3.11+ reads TOML out of the box
  (`tomllib`); YAML needs PyYAML, and the owner's macOS Python refuses
  `pip install` outside a venv. One file, nothing to install.
- **Catalog**: Kiwix OPDS v2 (`library.kiwix.org/catalog/v2/entries?name=`).
  Per collection, the newest edition in the first listed flavour wins. The
  download URL is the entry's `.meta4` link minus `.meta4`, and the checksum
  is `URL.sha256`, as the owner fetched by hand. Written from the format,
  not tested live (this session cannot reach Kiwix); the owner's `plan` run
  is the first live check, and it is read-only.
- **Staging** defaults to the SSK drive, where Wikipedia already sits; the
  builder recognises the file by name and checks its hash once (state in
  `.cardbuilder-state.json`, keyed by size + mtime).
- **Card layout**: `/library/<key>/`, ZIMs over 4,000 MiB written straight to
  the card as `.zimaa…` parts (no second 53 GB copy on the staging drive),
  each file written as `.tmp` and renamed, then `/library/manifest.json`.
- **Deletes only with `--prune`**, per the owner's ask-first rule; stale
  files are otherwise listed.
- First-aid names beyond mdwiki (MedlinePlus, post-disaster, military
  medicine) are unconfirmed and marked optional.
- Standard Ebooks (EPUB, not ZIM) and Wiktionary → StarDict come later in M2.

## 2026-10-03 — Books: Standard Ebooks, not Project Gutenberg

Kiwix ships `gutenberg_en_all` one way only, covers included: 221.3 GB
(measured from the live download, not the 60–80 GB estimated earlier). With
the other 74.8 GB that is 296 GB, more than the 256 GB card holds (about
238 GB usable). The owner chose Standard Ebooks instead: a few thousand
carefully edited public-domain books as EPUB, which CrossPoint already reads.
Their download route is checked before it is added to the card builder.

The card builder now skips collections that are not downloaded yet instead of
stopping, and refuses to `--prune` in that run so a skipped collection's files
on the card are never taken for stale.

## 2026-10-03 — Milestone 3: how an article reaches the screen

- **Reuse CrossPoint's EPUB layout engine instead of writing a renderer.**
  `ChapterHtmlSlimParser` already does fonts (built-in and SD), justification,
  hyphenation, headings, lists, tables, bold/italic, sub/superscript and the
  page cache format, and is tested on hardware. It needs well-formed XHTML
  from a file; Kiwix articles are browser HTML5. So a new streaming cleaner,
  `lib/zim/src/ZimHtml.{h,cpp}` (`zim::cleanArticleHtml`), sits between them.
  It runs on the host too, so it is tested there, and the parser is driven
  with no `Epub` object, no CSS and images off.
- **Cleaner rules.** Keeps h1–h6, p, lists, block quotes, tables
  (colspan/rowspan), b/i/u/s, sub/sup, br/hr, ruby. Renames to tags the parser
  lays out (`section`/`dl`/`pre`/`figcaption` → `div`, `dd` → `blockquote`,
  `dt`/`caption` → `p`, `em`/`cite` → `i`, `strong` → `b`). Drops, with their
  content: head, script, style, media, forms, nav, figures; elements whose
  class is MediaWiki or mwoffliner chrome (edit links, navboxes, hatnotes,
  message boxes, thumbnails, citation markers `sup.reference`, reference
  lists, TOC) or that are `display:none`. Unknown tags are unwrapped, keeping
  their text. Math shows its TeX source (`alttext`) in italics. Entities
  are decoded (249 named, numeric, Windows-1252 quirks), bad UTF-8 becomes
  U+FFFD, characters XML forbids are removed, HTML's implied end tags (`p`,
  `li`, `dt`/`dd`, `tr`/`td`/`th`) are applied, and every element is closed.
  Memory: the open-element stack plus a 4 KB output buffer.
- **Links are unwrapped for now.** The parser underlines every internal link,
  and a Wikipedia paragraph has dozens. Following links is Milestone 5; the
  cleaner already has `keepLinks` for it (tested).
- **Reference lists are dropped with their markers.** Without the `[1]`
  markers the numbered list at the end means nothing. Revisit in M5 if
  footnote popups are wanted.
- **Pages go to a file, not RAM.** Laid-out pages are serialized to
  `/.pocketlib/article.pages` as they are made (offsets in RAM, 4 bytes per
  page); the screen holds one page. Small allocations would otherwise pile up
  in the 183 KB of internal RAM. The first page shows as soon as it exists;
  the rest are laid out in 60 ms slices between page turns, only while the
  screen task is idle (same render-lock rule as the EPUB reader).
- **Cluster cache: 2 × ~2 MiB in PSRAM** (not the brief's 3): PSRAM also
  holds the article's HTML (up to ~1 MB) while it is cleaned, and SD fonts.
  Large `malloc`s go to PSRAM on this build (`CONFIG_SPIRAM_USE_MALLOC`,
  threshold 4 KB), so the HTML string does too.
- **Ways in for M3**: main page, random article (uniform over the front-article
  list), and "Go to title", which uses the card's `.pltitles` index to open
  the first title that starts with what was typed, ignoring case and accents.
  The live result list is M4.
- **Entry point**: Home → Library opens the shelf. One `#ifdef` in
  `HomeActivity::onLibraryOpen`, so it works with every Home theme (the cover
  grid draws its own menu); CrossPoint's book library is the shelf's last row.
- **Shelf source**: `/library/manifest.json` from the card builder (titles,
  dates, parts in order, index path); if it is missing or unreadable, the
  shelf scans `/library/<key>/` for `.zim`/`.zimaa…` and `.pltitles`.
- **Hyphenation language** is set to English for articles (the corpus is
  English-only, per the owner).
- **Cost** (local build, commit of this entry): flash +110 KB (app slot 87.4%,
  ~824 KB free); static internal RAM +3.2 KB.

## 2026-10-03 — Milestone 4: search as you type

The owner asked to build M4 before M3's device sign-off (the M3 out-of-memory
fix is untested on the device). Search is a separate row, so the M3 paths it
would test are unchanged.

- **Where**: the collection screen's first row, **Search**. It opens
  CrossPoint's own keyboard with up to eight matching titles drawn between the
  text field and the keys, refilled after every keystroke. Tap a title to open
  it; OK opens the top one. Reusing the keyboard (one fenced hook, see touch
  points) keeps its layouts, shift/symbol layers, cursor editing and button
  navigation, instead of a second keyboard to maintain.
- **Matching**: `zim::searchTitles` (`lib/zim/src/ZimSearch.*`, host-tested).
  With `.pltitles`: folded prefix (case, accents, spacing ignored). Without:
  the ZIM's byte-ordered title list, so case-sensitive.
- **Order**: title order, which puts an exact match first (it is the shortest
  key with that prefix). No popularity signal exists in a ZIM, and cheap
  proxies (article size, redirect count) cost a cluster decode or a scan per
  result; "forb" therefore lists Forbach before Forbidden City. Typing more
  narrows it quickly. Revisit with real use; the index format has a version
  number for a ranked variant (e.g. a precomputed popularity byte from the
  card builder).
- **Redirects collapsed**: results whose redirect target is already listed
  are skipped, so one article appears once (under the first of its titles
  reached).
- **Cost per keystroke**: one index seek (≤ 4 page reads, ~8 ms measured on
  the synthetic index) + one directory read per result, with at most 4 ×
  results records read. Estimate ~25–60 ms on the device *(est.)*; the
  collection screen shows the last lookup's time. The e-ink refresh, not the
  lookup, is expected to dominate.

## 2026-10-03 — SD speed: 40 MHz and 16 KiB transfers (owner approved)

- The SDK asks for `SDMMC_FREQ_DEFAULT`, commented "40 MHz", which ESP-IDF
  defines as 20 MHz; the SDK's own notes say the OEM firmware runs 40 MHz.
  Our envs now ask for `SDMMC_FREQ_HIGHSPEED` (the card is switched to high
  speed with CMD6 when it supports it). The SDK's mount loop already retries
  four times; attempts 3 and 4 now fall back to 20 MHz, so a card that will
  not run at 40 still mounts.
- Each SD command now moves up to 32 sectors (16 KiB, internal DMA RAM)
  instead of 8 (4 KiB): a ~300 KB compressed cluster takes ~19 commands
  instead of ~75.
- How: `scripts/pocketlib_sdk_patches/0001-sd-fast-clock-and-transfers.patch`,
  applied to the freeink-sdk submodule by `scripts/pocketlib_patch_sdk.py`
  (our envs only, idempotent via `git apply --check`, fails the build if the
  SDK moves). Everything is fenced in `POCKET_LIBRARY_SD_FAST`, defined only
  in our envs, so stock envs compile upstream's code even from a patched tree.
- Check on the device: Diagnostics → SD bus should read 40.0 MHz; rerun the
  SD benchmark and compare with the 1.93 MB/s / 1.9 ms baseline.

## 2026-10-03 — Milestone 5: links, Back, contents, remembered place

- **Links are kept and tappable.** The cleaner now keeps `<a href>` to pages
  inside the archive; the layout engine underlines them and records each
  one's box on the page (CrossPoint's footnote-link machinery), and a tap
  inside a box (6 px slop, 28 px minimum width, as in the EPUB reader)
  follows it. At most 32 links per page are tappable (the engine's cap).
- **Resolving a link**: `zim::parseLink` / `zim::resolveLink`
  (`lib/zim/src/ZimLink.*`): fragment and query split off, percent-decoding,
  `&amp;`, relative paths against the linking entry's directory, `..`
  climbing out of the namespace in the old scheme (`../A/Foo`), absolute
  `/C/Foo`. External schemes are refused. Tested on every link in the real
  sample in both namespace schemes. A link to an article not on the card
  shows "Not in this library: …" over the page.
- **One reading screen, a Back stack.** Following a link loads the new
  article in the same screen (one page file on the card, not one per
  article); Back reloads the previous article and lands on the page left.
  Up to 32 steps. Reloading costs the open time again; caching the previous
  article's page file is a later optimisation if it feels slow.
- **Contents**: Confirm, or a tap in the middle third of the screen (the
  reader-menu gesture CrossPoint uses), lists the article's section
  headings. The cleaner gives every heading its own anchor (`pl-h<N>`) and
  remembers the page's own ids on or inside it (`<span id="History">`), so
  `#History` links and the contents land on the same page. Jumping to a
  section also goes on the Back stack.
- **Remembered place**: `/.pocketlib/history.tsv`, the 50 most recent
  articles, most recent first, with the character offset of the page being
  read (the layout engine's visible-text offset), so the place survives a
  change of font or size. Saved on every page turn (write to a temporary
  file, then rename). Opening an article from a list resumes there; a link
  opens at the top (or its section).
- **Recent**: the shelf's first row lists that history; picking one opens
  the article where it was left.
- Last page: a page turn past the end now stays put (Back leaves) instead of
  closing the article.

## 2026-10-03 — Preview builds for review branches

`pocketlib-build.yml` and `pocketlib-host.yml` also run on `claude/**`
branches. A review branch publishes to a separate **preview** pre-release, so
a change can be flashed and tried before it is merged without replacing the
known-good **dev** build of `pocket-library`.

## 2026-10-03 — Milestone 6: search everything, Library grid

- **Library grid** (owner's design): two across, Recent · eBooks /
  Wikipedia · Maps / Medical · More. Medical and More open the same grid for
  their members. Groups come from the manifest's `group` field when the card
  builder writes one, else from the collection's name (`tileGroupFor`), so the
  grid works on the current card. Maps is a placeholder tile. The selected
  tile is solid black, never dithered (see the ghosting fix), and appears only
  once the buttons move it (touch users see no stray selection).
- **Icons**: stock Lucide SVGs from the SDK, made with the SDK's own
  `gen_icons.py` (`src/pocketlib/icons/libraryIcons.manifest`). Wikimedia's
  logos are trademarks and not used.
- **All collections stay open** once used (`Library::ensureOpen`): an archive
  costs a few KB plus open files; only the focused one keeps decoded clusters
  in PSRAM (`Library::open` drops the others' caches).
- **Merging across collections** (`zim::searchMany`): exact matches from every
  source first, then the sources take turns. Popularity scores count
  redirects inside one archive, so they don't compare across archives; taking
  turns keeps a small collection's best match beside Wikipedia's.
- **An exact title in several collections** is one row ("4 sources"); tapping
  asks which to open.
- **eBooks** are searched through CrossPoint's own book index
  (`/.crosspoint/library.idx`, every EPUB on the card, title and author), read
  once per search screen. "eBooks" means the owner's EPUBs only, whatever
  their source; Wikisource and Wikibooks stay under More.
- **Scope follows where search was opened** (a collection, a group, Home).
- **Recent searches** (`/.pocketlib/searches.txt`, six) and recent articles
  fill the empty search field.

## 2026-10-03 — Article images: the lead picture, and every picture on request

- Owner's choice: options 1 and 2 (the infobox/lead picture always; all
  pictures when asked, from the article toolbar's **Images**).
- Kiwix stores Wikipedia's pictures as **WebP** (checked: the 2024 sample has
  100, about 23 KB each). The reader's image pipeline reads JPEG and PNG, so
  the decoder of Google's **libwebp 1.4.0** (BSD-3-Clause + patent grant) is
  vendored in `lib/libwebp` (decoder only, no SIMD; +96 KB of flash, app
  slot 89.7%).
- Lazy, using CrossPoint's own mechanism: the cleaner writes each kept
  picture as `<img src="/pl-img/N.png" width height>`, the layout engine
  sizes it from those attributes (fenced patch), and ImageBlock's extractor
  hook calls `ArticleActivity::extractImage` when the picture's page is first
  drawn: one read of the WebP, decoded already scaled (libwebp's scaler, so a
  big picture never sits in memory full size), composited on white, turned
  grey, written as an uncompressed 8-bit greyscale PNG in
  `/.pocketlib/img/` (emptied for each article). Opening an article costs
  nothing more; a picture costs its decode only on the page that shows it.
- Kept: WebP pictures at least 60×40 (no flags, icons or formula images), in
  infobox image cells, figures and thumbnails, with their captions. Lead
  mode keeps the first such picture before the first section heading.
- Needs a "maxi" ZIM: the card's "nopic" files have no pictures, so this
  build changes nothing on them.

## 2026-10-03 — Search index v3: words inside titles; typo tolerance on the device

- Word records over a full-text index: a few hundred MB more on the card
  (estimate from the sample's 2.1x) instead of a separate index of article
  text, and the device's search code is unchanged but for one flag. Only
  articles (not redirects) get word records, at most six each.
- Typos are handled on the device, only when nothing matches, so a correct
  query costs nothing extra; a typo in the first three letters is found only
  as a swap of two of them.

## Dependencies

| Dependency | License | Use | Status |
|---|---|---|---|
| CrossPoint Reader 1.6.5 | MIT | base firmware | in use |
| freeink-sdk | MIT | hardware layer | in use (upstream) |
| Upstream's own deps (ArduinoJson MIT, QRCode MIT, PNGdec Apache-2.0, JPEGDEC Apache-2.0, WebSockets LGPL-2.1, Arduino-wolfSSL GPL, FreeType FTL/GPL-2, expat MIT, miniz MIT) | as listed — to verify one by one at M1 | upstream features | in use (upstream) |
| zstd 1.5.7 single-file decoder | BSD-3-Clause (dual BSD/GPLv2) | ZIM clusters | in use, `lib/zim/src/third_party/zstd` |
| xz-embedded v20240322 | 0BSD | older (pre-2020) ZIM clusters; the 2017 test files use it | in use, `lib/zim/src/third_party/xz` |
| zim-testing-suite (openZIM) @ 2edf720 | test data, fetched at test time, not vendored | host tests | in use |
| libwebp 1.4.0 (decoder only) | BSD-3-Clause + WebM patent grant | article images (WebP) | in use, `lib/libwebp` |
| Lucide icons (via freeink-sdk `libs/assets/Icons/lucide`) | ISC | Library tile icons, generated into `src/pocketlib/icons/libraryIcons.h` | in use |
| GoogleTest 1.17.0 | BSD-3-Clause | host tests (same as upstream) | in use |
| python-libzim | GPL-3.0 | **test oracle only**, run by hand to cross-check zimcat output; not shipped, no code used | used once, 2026-10-02 |

## 2026-10-02 — Firmware is built by GitHub Actions on the fork

The fork is `noah-pi/pocket-library`, work on branch `pocket-library`.
`.github/workflows/pocketlib-build.yml` builds on every push to that branch:
stock 1.6.5 `x4pro-gh_release` straight from upstream's tag (the known-good
fallback) and our `x4pro-pocketlib-release`, each with a `.sha256`. Reason:
GitHub's runners reach the PlatformIO registry, so builds don't depend on the
cloud session's network policy or the owner's Mac, and every `.bin` the owner
flashes is traceable to a commit. Toolchain pins copied from upstream's
`release.yml`. Upstream's own workflows don't trigger on this branch.

## Cloud-build workarounds (not part of the firmware)

This cloud session's network policy blocks the PlatformIO registry,
`download.kiwix.org` and `wiki.openzim.org`. To build here: SCons comes from
PyPI, registry libraries come from their GitHub tags at the same pinned
versions via a git-ignored `platformio.local.ini`, and the proxy's CA was added
to PlatformIO's private certifi bundle. None of this is needed on a Mac.
Refs that work (2026-10-03): ArduinoJson `v7.4.2`, QRCode `v0.0.1`, PNGdec
`1.1.6`, WebSockets commit `1c8b8de` (2.7.3 has no tag), wolfSSL `5.7.2`.
SdFat (a dependency of the SDK's SDCardManager) must be pre-placed in
`.pio/libdeps/<env>/SdFat` from tag `2.3.1` with a `.piopm` naming owner
`greiman`, or PlatformIO goes to the registry for it. Such local builds are
for compile checks and sizes only: they are not byte-identical to CI's (the
1.6.5-based build came out 13,792 bytes smaller), so the owner flashes CI
builds.

## Measurements

### Device (fill in from Diagnostics and serial logs)

| Quantity | Value | Date | Notes |
|---|---|---|---|
| Chip | ESP32-S3 (QFN56) rev v0.2, dual core 240 MHz, 40 MHz crystal | 2026-10-02 | esptool 5.4.0 |
| PSRAM size | 8 MB embedded (AP_3v3) per esptool; firmware value pending | 2026-10-02 | confirm with `esp_psram_get_size()` |
| PSRAM free at Diagnostics | 8,080 KB free, largest block 8,063 KB | 2026-10-02 | CrossPoint 1.6.5 barely touches PSRAM |
| Internal RAM free / largest | 183 KB / 135 KB | 2026-10-02 | |
| Flash size / speed | 16 MB @ 80 MHz | 2026-10-02 | Diagnostics + esptool |
| SD bus | SDMMC @ **20.0 MHz** (driver-reported); width read aloud as "2-bit", which SDMMC does not support, so almost certainly 1-bit | 2026-10-02 | confirms the SDK's "40 MHz" comment is wrong |
| Panel controller | **UC8279** (UltraChip variant, not the SSD1677 in the brief's spec sheet) | 2026-10-02 | refresh timings must be measured on this panel, not taken from the GDEQ0426T82 datasheet |
| SD sequential read | 1.93 MB/s (16 MB, 64 KB reads into PSRAM) | 2026-10-02 | 32 MB scratch file; consistent with a 20 MHz 1-bit bus |
| SD random 4 KB avg / p95 / max | 1.9 / 1.9 / 2.1 ms | 2026-10-02 | 32 MB scratch file: too small to show FAT-chain cost; repeat on a 4 GB part |
| SD sequential write | 1.72 MB/s | 2026-10-02 | 32 MB scratch file `/.pocketlib/bench.bin` |

### Factory backup (2026-10-02)

- Owner's unit is **not USB-locked**. Stock firmware enumerates as
  "XTEink X4 Pro" (USB 303a:4002), mass storage only, no serial port.
  Download mode (hold **left side button**, press power) exposes
  USB-Serial/JTAG at `/dev/cu.usbmodem14301`. This confirms the SDK note
  that the left button is GPIO0.
- Full 16 MB read with esptool 5.4.0 in 139.9 s (959 kbit/s), saved as
  `X4Pro-factory-backup.bin` on the owner's flash drive. Restore command,
  kept here for emergencies (writes everything back exactly):
  `esptool --chip esp32s3 --port <port> write-flash 0 X4Pro-factory-backup.bin`

### Build outputs (GitHub Actions run 37042994823, commit 805c76f, 2026-10-02)

| Firmware | .bin bytes | SHA-256 | App slot used | Static internal RAM |
|---|---|---|---|---|
| stock 1.6.5 `x4pro-gh_release` | 5,632,640 | `d5dfea88…b56c5ac` | 85.9% of 6,553,600 | 101,792 B (31.1%) |
| ours `x4pro-pocketlib-release` | 5,638,416 | `13758ff0…27f2db` | 86.0% | 101,792 B (31.1%) |

Diagnostics costs 5,776 bytes of flash and no static RAM. **Flash headroom is
the new constraint:** about 915 KB remain in each 6.25 MB OTA slot, and the
ZIM reader, zstd decoder, xz decoder, HTML converter and library UI must fit
there. Watch this number every build; options if it gets tight are the
`firmware_tuned`-style trims upstream uses on the C3, dropping unused
features from our env (e.g. the OPDS/KOReader-sync code), or a repartition
(SPIFFS is 3.4 MB and unmounted) — the last needs a full-flash, so it waits.

### zimcat on real Wikipedia (owner's Mac, SSK drive, 2026-10-02)

`wikipedia_en_all_nopic_2026-06.zim`, 52,690,706,555 bytes, sha256 OK.

| Operation | Time |
|---|---|
| Open archive | 0.3–1.9 ms |
| Exact title lookup, 19.2 M titles | 0.3 ms |
| Prefix lower bound ("Forb") | 0.3 ms |
| Read "Forbidden City" (282,438 B, cluster 75354), cold | 20.9 ms |
| Same, second run | 9.3 ms |

The Mac's file cache flatters these numbers. Device estimate (speculative):
about 25 binary-search probes × 2 random reads × 1.9 ms ≈ 100 ms per
lookup, plus about 1 s to read and decompress a ~2 MB cluster at 1.93 MB/s.
To be measured in M3.

### Performance targets (brief §7) — measured values arrive from M3 on

| Action | Target *(est.)* | Measured |
|---|---|---|
| Search update per keystroke | ≤ 100 ms | (shown as "last lookup" on the collection screen, M4) |
| Open article, cluster not cached | ≤ 800 ms | |
| Open article, cluster cached | ≤ 300 ms | |
| Page turn layout | ≤ 100 ms | |
| Wake to usable screen | ≤ 1 s excl. refresh | |

## 2026-10-03 — First Aid: instructions only, by permanent address

First Aid rows name a MedlinePlus page by its path under medlineplus.gov and
open it at its "First Aid" heading; the fallback is the Wikibooks First Aid
manual. Encyclopedia articles (Wikipedia, MDWiki) describe a condition at
length and bury what to do, so they stay out of First Aid (they still serve
the Medical Encyclopedia). Lookup by path is stable across MedlinePlus
retitlings; a title check guards against a wrong address. Stroke, drowning,
dehydration and the recovery position have no MedlinePlus first-aid page, so
they come from Wikibooks or are folded into other rows (recovery position is
in "Unconscious person").

## 2026-10-04 — Scope: performance first

Owner: the Wiktionary tap-to-look-up is overkill (dropped); CJK waits (fonts
download over Wi-Fi when needed); the last missing-symbol fixes come later;
the six failed book conversions are dropped. From here the priority is
everything working excellently: speed, battery life, fast loading, with
optional features cut where they cost too much.
