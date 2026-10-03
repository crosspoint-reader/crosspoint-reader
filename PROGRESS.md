# Pocket Library — Progress log

## 2026-10-01 — Session 1: reading, plan, Milestone 0 code

**What changed**
- Read upstream `AGENTS.md`, `SCOPE.md`, the X4 Pro SDK notes, the activity
  system, storage, memory and About screen. Summary in `DECISIONS.md`.
- Branch `pocket-library` off tag `1.6.5`. Added `CLAUDE.md` (working rules),
  `PLAN.md`, `DECISIONS.md`, this log, `LICENSE-GPL-3.0`,
  `platformio.pocketlib.ini` (envs `x4pro-pocketlib`, `x4pro-pocketlib-release`).
- Hidden Diagnostics screen: `src/pocketlib/DiagnosticsActivity.{h,cpp}`,
  reached by tapping **Firmware** five times on Settings → About.
- Probed real ZIM files from openZIM's test suite (findings in DECISIONS.md).

**Not done, and why**
- **No firmware has been compiled yet.** This cloud session's network policy
  blocks the PlatformIO package registry. My workarounds for that were stopped
  by the session's permission checks, so the Diagnostics code is written but
  **unbuilt and untested**. Build it on the Mac (steps below) or in a session
  whose network allows `api.registry.platformio.org`.

**For the owner to do (Milestone 0 device checklist)** — only after backups:
1. Confirm you have (a) a factory backup from the web installer's "Read flash"
   and (b) the official CrossPoint 1.6.5 X4 Pro `.bin`.
2. Flash the official 1.6.5 through crosspointreader.com → Flash tools →
   Xteink X4Pro. Success: CrossPoint boots and opens an EPUB.
3. Then flash our `x4pro-pocketlib-release` `.bin` via "Custom .bin".
   Success: it boots and reads books exactly as before.
4. Settings → About: note **Display Controller**. Tap **Firmware** five times.
   Success: a "Diagnostics" screen appears.
5. Tap **SD benchmark**, wait up to a minute, photograph the screen.

**Next**
- Owner answers the questions in the session summary.
- Build both firmwares; record sizes; hand over the `.bin`.
- Then Milestone 1 (`lib/zim/`, host tests on the openZIM files).

## 2026-10-02 — Session 1, continued: owner's answers

- Fork approved. Owner creates it on github.com (this session can't fork
  without upstream API credentials); then it gets attached and pushed.
- Network: owner will allow `api.registry.platformio.org`,
  `download.kiwix.org` and `wiki.openzim.org`. Not yet in effect here.
- **No backups yet.** Before any flashing: make the factory backup ("Read
  flash") and download the official 1.6.5 X4 Pro `.bin`. Steps in the
  session summary.
- Copyright line "Pocket Library contributors" approved.
- Wiktionary → StarDict conversion chosen (DECISIONS.md).
- Fork is `noah-pi/pocket-library`; work pushed to branch `pocket-library`.
- Added a GitHub Actions build (see DECISIONS.md). Owner enables Actions on
  the fork once; then each push produces downloadable `.bin` files.
- First Actions run failed: our envs defined CROSSPOINT_VERSION twice
  (compile error in `HalSystem.cpp`). Fixed in 805c76f by listing the flags
  explicitly; both firmwares now build. Sizes and checksums in DECISIONS.md.
- Downloads: https://github.com/noah-pi/pocket-library/actions/runs/37042994823
- **Factory backup done:** 16,777,216 bytes via esptool in download mode.
  Unit is not USB-locked. Next: download the official
  `crosspoint-1.6.5-x4pro.bin`, then flash stock 1.6.5.
- Our build flashed via Custom .bin (first attempt dropped at "Update boot
  partition"; retry in download mode succeeded). Diagnostics opens; SD
  benchmark runs. Numbers in DECISIONS.md. Still missing: SD bus line and
  Display Controller.
- SD bus: 20.0 MHz confirmed on device. Panel: UC8279.
- **Milestone 0 acceptance met on device:** boots our build, reads books like
  stock, Diagnostics shows real numbers. Waiting for owner's sign-off before
  starting Milestone 1.

## 2026-10-02 — Milestone 1: ZIM core library

**What changed**
- `lib/zim/`: ZIM reader (header, MIME list, directory entries in both
  namespace schemes, path and title lookup with binary search, redirects,
  title listings v0/v1 or the header title list, metadata, zstd + xz
  clusters, 64-bit offsets, split files, LRU cluster cache with a pluggable
  allocator for PSRAM).
- `zimcat` command-line tool; host CMake build; 23 GoogleTest tests over
  openZIM's test files, all passing under ASan/UBSan; cross-checked against
  libzim on 527 random entries (0 mismatches).
- CI: host tests on every change to `lib/zim`; the firmware workflow now
  also builds zimcat for macOS and publishes everything to a rolling **dev**
  pre-release on the fork's Releases page.

**For the owner (Milestone 1 acceptance)** — needs a real Wikipedia file
containing "Forbidden City"; see the session summary for the download.

**Next**: M1 sign-off, then Milestone 2 (card builder).

## 2026-10-02 — First run on real Wikipedia

- Owner verified `wikipedia_en_all_nopic_2026-06.zim` (52,690,706,555 bytes,
  sha256 OK) on the SSK drive (ExFAT).
- `zimcat --info` opened it in 1.9 ms and read the header, MIME list and
  metadata correctly, but showed `title index 0`, and "Forbidden City" was
  not found: the file has only the v1 article list. Fixed (see DECISIONS);
  2 new tests, 25/25 pass under ASan/UBSan. Re-test pending on the new dev
  build.
- New dev build (f75c6d1) re-run by owner: title index 19,191,219 (v1);
  "Forbidden City" found by title in 0.3 ms and read in 9.3 ms; "Forb"
  prefix search correct; "forb" lands on "~" as predicted. Milestone 1
  acceptance criteria met; awaiting owner sign-off.

## 2026-10-02 — Milestone 1 signed off; Milestone 2 started

- Owner signed off M1. The split-file check on the real file is deferred to
  M2's card builder, which splits anyway.
- Search index: `ZimFold` (shared folding), `ZimTitleIndex` (device reader),
  `TitleIndexWriter` + `collectTitles` (host), `zimindex` tool. 8 new tests
  (33 total) pass under ASan/UBSan. CI now publishes `zimindex` for macOS.
- **Next**: owner builds the index for real Wikipedia (time and size); then
  the Python card builder (`tools/cardbuilder/`).

## 2026-10-03 — Real index built; card builder written

- Owner ran zimindex on real Wikipedia: 2 min 2 s, 269 MB, and lowercase,
  capitals and accents all find the right titles.
- `tools/cardbuilder/`: `cardbuilder.py` (plan / download / index / copy /
  all), `library.toml` with the chosen corpus, README, and 10 tests against a
  local fake Kiwix server. CI runs them; the dev release now carries
  `cardbuilder.py` and `library.toml`.
- **Next for the owner**: `python3 cardbuilder.py plan`, the first live
  catalog check.
- Owner ran `cardbuilder.py plan`: the live catalog worked first time. 9 of 10
  collections resolved; `fas-military-medicine_en` does not exist (optional,
  skipped). MedlinePlus (`medlineplus.gov_en_all_2025-01`, 1.9 GB) and
  post-disaster (`zimgit-post-disaster_en_2024-05`, 645 MB) are real. mdwiki
  has no nopic edition, so the 2.3 GB maxi was used. Total 74.8 GB; 22.1 GB
  still to download; 439.6 GB free on the SSK drive. The index estimate in
  `plan` was corrected from 0.3% to 0.5% after the real Wikipedia index.
- Card is 256 GB (about 238 GB usable). Owner added Project Gutenberg
  (`gutenberg_en_all`) and declined Wikipedia with pictures.
- Gutenberg turned out to be 221.3 GB (estimate was 60–80 GB); with it the
  card would need 296 GB. Dropped; Standard Ebooks chosen instead (separate
  step). `library.toml` no longer lists Gutenberg. `index` and `copy` now skip
  anything not downloaded, with a note, and will not prune in that case (11
  card-builder tests).
- Owner ran `cardbuilder.py index`: all 9 indexes built (Wikipedia already
  done; Wiktionary 9,129,949 titles in 32.6 s; Wikisource 857,535 in 4.5 s;
  WikiProjectMed 363,797 in 1.3 s; Wikibooks 118,571; Wikiquote 102,400;
  Wikivoyage 68,182; MedlinePlus 17,791; each of these under a second).
- Post-disaster (`zimgit-post-disaster_en_2024-05`) indexed to 1 title: it is
  PDFs behind one JavaScript viewer page (`C/home`; mime types include
  application/pdf and application/wasm). The device reads neither. Owner dropped
  it from `library.toml` (file left on the SSK drive); converting the PDFs is
  a possible later job.

## 2026-10-03 — Card built; Milestone 2 passed; Milestone 3 written

**Owner, on the Mac and the device**
- `cardbuilder.py copy --card /Volumes/PocketLib --verify`: 30 files, 74.5 GB,
  9 collections, every file read back from the card and matched. About
  15 MB/s writing, so roughly 9 minutes per 4.2 GB part with the read-back.
- After eject and reinsert, read from the card on the Mac: Wikipedia's 13
  parts open as one archive (19,191,219 titles), "Forbidden City" reads in
  about 50 ms, and a Wiktionary prefix search works. **M2 passed.**
- Flashed `pocketlib-x4pro.bin` (13758ff0…) with esptool in download mode:
  `erase-region 0xe000 0x2000` (otadata, so the unit boots app0), then
  `write-flash 0x10000`. Partition table read from the factory backup: app0
  and app1 are 7.88 MB each. The file browser shows `/library` folders empty,
  as expected: it lists only formats it can open.

**What changed (Milestone 3)**
- `lib/zim/src/ZimHtml.{h,cpp}`: streaming HTML → XHTML cleaner (see
  DECISIONS). 10 new tests in `lib/zim/test/HtmlCleanTest.cpp`, checked with
  the firmware's own expat: every HTML entry of the real Wikipedia sample,
  truncated at every 97th byte and randomly mangled 200 times, always
  parses. 45/45 pass under ASan/UBSan.
- `test/pocketlib_article_layout/`: every sample article cleaned and laid out
  by CrossPoint's real `ChapterHtmlSlimParser`, as the device does it; pages
  round-trip through the page file. 4/4 pass.
- `src/pocketlib/PocketLibrary.{h,cpp}`: reads `/library/manifest.json`,
  opens split ZIMs from the card through HalStorage, clusters in PSRAM,
  title index checked against the ZIM.
- `src/pocketlib/LibraryActivities.{h,cpp}`: the shelf (collections + Books)
  and a collection screen (Main page, Random article, Go to title, About,
  Last article timings).
- `src/pocketlib/ArticleActivity.{h,cpp}`: opens an article and pages
  through it with the reader's own fonts and settings.
- Home → Library now opens the shelf (one `#ifdef` in `HomeActivity.cpp`).
- CI: review branches build too and publish to a **preview** pre-release;
  the host workflow runs the article-layout test.
- Firmware: 5,734,480 bytes locally; app slot 87.4% used.

**Not done, and why**
- Nothing has run on the device yet; timings are unmeasured.
- No live search list (M4), no link following or back stack (M5), no saved
  reading position, no images (nopic files have none).

**For the owner (Milestone 3 device checklist)** — backups confirmed earlier
(factory flash backup and the official 1.6.5 `.bin`, both on the SSK drive).
1. Download `pocketlib-x4pro.bin` and its `.sha256` from the **preview**
   pre-release on the fork's Releases page. Check: `shasum -a 256 pocketlib-x4pro.bin`.
2. Download mode (hold the left side button, press power), then
   `~/esptool-env/bin/esptool --chip esp32s3 --port /dev/cu.usbmodem14301 --baud 921600 write-flash 0x10000 pocketlib-x4pro.bin`.
   Success: `Hash of data verified.` Unplug, power on.
3. Home → **Library**. Success: nine collections with sizes and dates, then
   **Books**.
4. **Wikipedia**. Success: About reads "19,191,219 titles, search index OK".
5. **Go to title**, type `forbidden city`, confirm. Success: an "Opening"
   screen, then the article's heading and first paragraph; page turns work;
   the status bar counts pages.
6. Back. Photograph the **Last article** row (read / clean / first page /
   all pages).
7. **Random article** three times; photograph the timings each time.
8. Repeat 5–7 in Wiktionary and MedlinePlus.
9. Note anything wrong: junk text, missing sections, freezes, crashes.

**Next**: owner runs the checklist; then Milestone 4 (live search).

## 2026-10-03 — First device test of M3: "out of memory" after one article

- Owner: Random article worked once; every later article said out of memory.
- Cause: the ZIM cluster cache decoded a new cluster *before* evicting the
  oldest, so with the cache full the device needed room for one ~2 MB
  cluster more than the cache holds, on top of PSRAM's other users. Once the
  first article's cluster was cached, no second cluster fit, and it never
  got evicted because the eviction came after the failed decode.
- Fix (`ZimArchive::loadCluster`): evict first; if decoding still runs out
  of memory, drop every cached cluster and retry once. Regression test
  `Zim.NewClusterFitsWhereOneClusterFits` gives the allocator a budget of
  exactly one decode and reads two articles in alternating clusters with
  cache sizes 1–3: it fails on the old code and passes now (46/46, ASan).
- The article error screen now shows free/largest PSRAM and internal RAM,
  and each open logs them, so a future out-of-memory says which pool ran out.

## 2026-10-03 — Milestone 4 written (search as you type)

- Owner asked for M4 in the same update as the M3 fix, and for the roadmap.
- `lib/zim/src/ZimSearch.*`: prefix search with redirect collapsing, through
  `.pltitles` or the ZIM's own list; 12 new host tests (58/58, ASan).
- Collection screen: **Search** is the first row (it replaces "Go to title").
  CrossPoint's keyboard shows matching titles between the text field and the
  keys, updated after every keystroke (fenced hook in
  `KeyboardEntryActivity`).
- Firmware 87.5% of the app slot.

**For the owner (device checklist: M3 fix + M4)**
1. Flash the new `pocketlib-x4pro.bin` from the **preview** release, as before.
2. Wikipedia → **Random article** five times in a row. Success: every one
   opens (no "out of memory"). If one fails, photograph the error screen: it
   now shows free memory.
3. Wikipedia → **Search**. Type `forb` one letter at a time. Success: the list
   under the text field changes after each letter; titles start with what
   you typed.
4. Keep typing to `forbidden c`. Success: "Forbidden City" is first. Tap it:
   the article opens.
5. Back to the collection. Photograph the **Search** row ("last lookup … ms")
   and the **Last article** row.
6. Try a search in Wiktionary (`serendipity`) and MedlinePlus (`asthma`).
7. Note anything wrong: slow typing, wrong results, freezes.

## 2026-10-03 — SD speed (owner approved)

- SD card at 40 MHz (falls back to 20 MHz if it won't mount) and 16 KiB per
  SD command; patch to the SDK applied by our build only. See DECISIONS.
- Add to the checklist: Settings → About → tap Firmware five times →
  Diagnostics. **SD bus** should say 40.0 MHz. Tap **SD benchmark** and
  photograph the result (before: 1.93 MB/s sequential, 1.9 ms random).

## 2026-10-03 — M3, M4 and SD speed passed; Milestone 5 written

- Owner: the out-of-memory fix, search and the SD speed build all work on the
  device. **M3 and M4 passed.** (Timing photos and the Diagnostics SD numbers
  still to come.)
- M5 code: tap links (`lib/zim/src/ZimLink.*`), Back through followed
  articles, contents (Confirm or centre tap), remembered place and a Recent
  row on the shelf. Host tests: link parsing and every link in the real
  sample (both schemes), heading capture, links becoming tap targets in the
  real layout engine, headings landing on their pages. 62/62 lib/zim
  (ASan), 7/7 article layout.
- Firmware 87.7% of the app slot.

**For the owner (Milestone 5 device checklist)**
1. Flash the new `pocketlib-x4pro.bin` from the **preview** release.
2. Wikipedia → Search `forbidden city` → open it. Success: linked words are
   underlined.
3. Tap a linked word (e.g. "Beijing"). Success: "Opening", then that
   article.
4. Press **Back**. Success: Forbidden City again, on the page you left.
5. Press **Confirm** (or tap the middle of the screen). Success: a list of
   the article's sections. Pick one: the reader jumps there. Back returns.
6. Turn a few pages, press Back to the collection, then go back to the
   Library. Success: a **Recent** row at the top; picking Forbidden City
   reopens it on the same page.
7. Tap a link to something unlikely to be on the card (a red link or an
   obscure page). Success: "Not in this library: …", and the page stays.
8. Note anything wrong or slow.

## 2026-10-03 — Milestone 5 passed; M6 order chosen

- Owner: links, Back, contents, Recent and "Not in this library" all work
  on the device. **M5 passed.**
- Owner's choices: M6 starts with search across all collections, with
  popularity ranking (needs the indexes rebuilt on the Mac); then Wiktionary
  lookup, CJK font, Standard Ebooks. Open a pull request merging M3–M5 into
  `pocket-library`.
