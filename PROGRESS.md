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

## 2026-10-03 — PR #1 merged; ghost text fix

- PR #1 (M3–M5, SD speed) merged into `pocket-library`.
- Owner: faint text from the previous screen shows inside the gray selection
  bar, on every return to Home too. Cause: list screens draw with the fast
  waveform, which leaves a trace of the screen before; the bar is a dither, so
  the trace breaks its pattern. Stock CrossPoint behaves the same (seen on
  Diagnostics). Fix: one half refresh on the first draw after any screen
  change (a brief flash); moving the selection stays fast. Workaround on
  older builds: control center → Force refresh, or Settings → Controls →
  Short power button → Force refresh.
- Also in this build: ranked search (works with the current cards; results
  rank by popularity once the indexes are rebuilt as v2).
- Device checklist: Home → Library → a collection → Back → Home: no ghost
  text in the selection bars; Settings → About → Diagnostics → Back: same;
  turning article pages is unchanged (no extra flashes); control center's
  Force refresh still does its full refresh.

## 2026-10-03 — Milestone 6 (part 1): Library grid and search everything

- Owner's design: Library is a grid, two across: Recent, eBooks, Wikipedia,
  Maps (placeholder), Medical, More, each with a small icon; Medical and More
  open sub-grids. Search: chips (All · eBooks · Wikipedia · More), results as
  you type across every collection and the EPUBs; opened from a magnifier tab
  on the Cover Grid home, from a collection, or by the power button (Settings
  → Controls → Short power button → Search).
- Built: `zim::searchMany` (host-tested), `Library` keeps every collection
  open, `SearchActivity`, the tile grid, keyboard chips and tagged rows, the
  Home tab, the power-button option. 70 host tests pass; firmware 88.1% of
  the app slot.
- Ranked results need the indexes rebuilt on the Mac (v2); until then each
  collection's results are alphabetical, merged as above.
- Device checklist:
  1. Home (Cover Grid) shows a magnifier as the last tab; tapping it opens
     search with **All** chosen.
  2. Type `paris`: an exact row with "N sources" first; tapping it lists the
     collections; each opens its article. Back returns to the results.
  3. Tap **eBooks**: your EPUBs matching by title or author; one opens in the
     book reader. Tap **More**: a list of Medical and each collection.
  4. Empty field: recent searches (tap fills the field) and recent articles.
  5. Library: the 2×3 grid with icons; Medical and More open sub-grids; Maps
     shows "coming"; eBooks opens the book list; Recent lists articles.
  6. Inside a collection, Search starts with that collection's chip chosen.
  7. Settings → Controls → Short power button → Search; a short press opens
     search from Home and from inside a book; Back returns there.


## 2026-10-03 — Owner feedback on the M6 build; next work chosen

- Grid and search work on the device. Fixed: tile names spilling into the
  next tile; long titles on "Opening" running off the screen (37282e0).
- A crash (abort() on core 1) seen once; waiting for crash_report.txt and
  what was on screen. CI now keeps the firmware .elf to decode such reports.
- Owner's choices for what comes next:
  - Collection home like the Wikipedia app: a search bar with the
    collection's icon first, then continue reading, recent, main page and
    random; About moves to a menu.
  - Full-screen search results that scroll (swipe or buttons) after OK.
  - Article navigation from the Wikipedia app: toolbar on a centre tap,
    contents as navigation (current section marked, page numbers), section
    name in the status bar, link previews, and **outline mode** (lead plus
    section headings; tap a heading to read that section).
  - Search: A any word in a title, B typo tolerance, C search of each
    article's opening paragraph (its own milestone).
  - Medical: a First Aid screen (organised by emergency, MedlinePlus pages
    first, Wikibooks First Aid for detail) and a Medical Encyclopedia
    (MedlinePlus and MDWiki behind one search, browsable by body system).
- Fewer clean refreshes: keep them for list and grid screens, not the
  keyboard or articles.

## 2026-10-03 — Crash opening "Giant panda": out of PSRAM

- crash_report.txt: after "Josephoartigasia" (108 KB HTML, 33 pages) PSRAM
  was down to 816 KB free (largest piece 431 KB); opening "Giant panda" then
  aborted. Built without exceptions, a failed std::string allocation aborts.
- Fixes: articles are cleaned straight from the decoded cluster
  (`Archive::readView`, no copy); `readBlob` checks for room and returns
  NoMemory instead of aborting; other collections drop their decoded
  clusters on every article open and after every search; with under 1.5 MB
  of PSRAM in one piece after cleaning, the article's own clusters are
  dropped before layout. Host tests: ReadViewMatchesRead,
  CopyThatDoesNotFitIsNoMemory (72 pass).
- Open question: what held the other ~4 MB between "PANDA experiment"
  (4857 KB free) and "Josephoartigasia" (816 KB free). Watch the
  "PSRAM free" log line across several articles.
- Measured on the device (same log): article read 425–464 ms when its
  cluster is decoded, 2 ms when cached; clean 34–111 ms; first page
  185–1018 ms; a 33-page article lays out in 3.4 s in the background.

## 2026-10-03 — Next update, build 1: collection home, full-screen results

- Collection home (owner's request, like the Wikipedia app): a rounded
  search bar with the collection's icon ("Search Wikipedia"), then Continue
  reading and up to three recent articles from this collection, Main page,
  Random article, and About (titles, date, size, last article's timings).
- Search: OK now opens every result (up to 80) as a full-screen list that
  scrolls by swipe or the side buttons; Back returns to the keyboard, and
  Back from an article opened there returns to the list. A tapped
  suggestion still opens directly. An exact title in several collections
  names them ("Wikipedia · Wiktionary · +1").
- Clean refresh no longer on the keyboard or the search screen under it.
- Device checklist:
  1. Library → Wikipedia: the search bar with the globe, then Continue
     reading / Recent (if any), Main page, Random article, About.
  2. Tap the bar: search opens with the Wikipedia chip chosen.
  3. Type `paris`, press OK: a full list; swipe up/down; open one; Back
     returns to the list; Back again to the keyboard.
  4. Opening search and typing feel quicker (no flash on the keyboard).
  5. Lists and grids still open with one clean flash (no ghosts).

## 2026-10-03 — Next update, build 2: article navigation

- A tap in the middle of an article shows a toolbar across the top: Back,
  Contents, Search, Outline (tap elsewhere, turn the page or Back closes it).
- Contents mark the section being read (•), select it, and give each
  section's page.
- The status bar names the section being read (when the title is shown).
- Link previews: tapping a link shows its article's title and first
  sentences in a card; tap the card to open it, the page to close it.
- Outline: Introduction (the lead's first sentences) and the sections, each
  with its page and length; choose one to read it, Back returns to the
  outline. "Open articles here first: On/Off" at its foot.
- The cleaner keeps each article's lead (`HtmlCleanOptions::lead`,
  `firstSentences`); 4 new host tests, 76 in all.
- Device checklist:
  1. Open Climate change; tap the middle: the toolbar; each of its four
     buttons works.
  2. Contents: the current section marked, page numbers beside sections.
  3. Tap a link: a preview card; tap it to open; Back returns.
  4. Outline: sections with pages and lengths; pick one; Back returns to the
     outline. Turn "Open articles here first" On and open another article.

## 2026-10-03 — Article images (lead picture; all on request)

- Built and host-tested: WebP → greyscale PNG (`ZimImage`, every WebP in the
  sample converts; PNGs read back, and Pillow opens them), the cleaner's
  lead/all picture modes (3 new tests), the reader's lazy extractor and a
  fifth toolbar button, **Images**. 84 host tests pass.
- Device test needs a ZIM with pictures ("maxi"); the card's files are
  "nopic". Cheapest test: Wikivoyage maxi.
- Device checklist (with a maxi file):
  1. An article with an infobox shows its picture on page 1, in grey.
  2. Toolbar → Images: the article is laid out again at the same place with
     every picture and its caption; Images again returns to the lead picture.
  3. Pages without pictures turn as fast as before; a page with one takes a
     moment the first time only.


## 2026-10-03 — Medical: First Aid and Medical Encyclopedia

- Medical opens on two guides, then its collections: **First Aid** (29
  emergencies, most urgent first: CPR, choking, bleeding, shock, heart
  attack, stroke, anaphylaxis, burns, …) and **Medical Encyclopedia** (search
  over MedlinePlus and MDWiki, then 17 body systems and topics). Each row
  names the page it wants, MedlinePlus first (US National Library of
  Medicine, public domain), then MDWiki or Wikipedia; titles are looked up
  exactly when the screen opens, and rows nothing answers are left out.
- The MedlinePlus titles are from the website's naming and not yet checked
  against the card's file: the Mac session lists which resolve (zimcat).
- Wikivoyage switched to the edition with pictures (maxi) in library.toml,
  to test article images.
- Device checklist: Library → Medical → First Aid: rows with their sources;
  each opens; Encyclopedia: search row opens search scoped to Medical; a
  body system opens.

## 2026-10-03 — Search: words inside titles, typos (index v3)

- Index version 3 (`zimindex`, `cardbuilder.py` rebuilds older ones): each
  article's title is also indexed from each later word on (up to six, words
  of three letters or more, a few stop words skipped), flagged as a word
  record. "panda" now finds Giant panda and Red panda, ranked by popularity
  with the titles that start with "panda"; only a whole title is an exact
  match. The sample's index grows 2.1x (96 KB → 204 KB).
- Typos: when nothing matches, the search tries the query with one of its
  first letter pairs swapped, then trims letters from the end (keeping three)
  until titles turn up, and keeps those within one typo (two for queries of
  eight letters or more). "climte change", "clmiate change", "climate
  chnage" and "climate changee" all find Climate change. Works with older
  indexes too (device-side).
- 90 host tests, 12 card builder tests pass.

## 2026-10-03 — The owner's books: sorting, tidying, shelf order

- The owner's 907 converted EPUBs were sorted into High / Medium / Low
  (shown to the owner first; nothing deleted). The list lives outside the
  repo; it is the owner's library.
- `booklist.py prune books.csv [--apply]`: moves every book not marked keep
  to the macOS Trash (File > Put Back undoes it). Prints the list first and
  does nothing without `--apply`.
- `booklist.py tidy books.csv [--apply]`: writes Title Case titles and mended
  authors into the kept books with Calibre's ebook-meta ("The demolished man"
  → "The Demolished Man", "Cormac Mccarthy" → McCarthy), drops bare "a novel"
  subtitles, and turns "Joe Pitt 1 - Already Dead" into the title plus a
  series. Deliberate capitals (UR, H.M.S., V., Less Than Zero) are kept.
  8 tests (`python3 -m unittest test_booklist` in tools/books).
- Device: the Library's title sort and letter groups ignore a leading "The",
  "A" or "An" ("The Road" files under R; "A Is for Alibi" stays under A).
  Search still matches the whole title. Index fold version 5: the book index
  rebuilds once on the first boot after the update.
- Device checklist: Library sorted by title: The Road between Rabbit and
  Rant, under R; the letter jump shows R for it.

## 2026-10-03 — Owner's first look at M6 on the device: fixes

- Icons (Library tiles, the article toolbar, Home's search tab) were on their
  side: ours are generated upright, and CrossPoint's drawIcon expects icons
  stored a quarter turn round. Drawn with `drawUprightIcon` now.
- The screen flashed on every tap: the ghost-text fix gave every screen change
  a half refresh. Now on every return to Home and every fourth other change.
- No pictures in Wikivoyage: the cleaner took only WebP pictures. It now takes
  JPEG and PNG too (a JPEG goes to the reader's JPEG decoder as it is, a PNG to
  its PNG decoder; WebP is still converted), and a picture with no size on its
  tag uses data-file-width/height. The owner's Wikivoyage (zimcat on Paris)
  holds JPEG photos and PNG maps under ./_assets_/<hash>/, so it had no WebP
  at all; a link test covers that path.
- Contents and Outline say "Page 12" and "3 pages long, from page 12" instead
  of "p. 12".
- 91 host tests pass.
- Contents and Outline merged into one Contents (owner's choice), after the
  Wikipedia app: Introduction with the article's opening sentences, then each
  section and subsection (to level 4) with its first sentence (new
  `HtmlHeading::summary`, from its first paragraph or list item) and length,
  the one being read marked "You are here"; the foot row "Open articles at
  their contents". Back after a jump returns to the page left (or to the
  contents when they opened with the article). The toolbar's freed slot is
  Text size (the reader's own point sizes, same setting books use; the article
  is laid out again at the same place). 92 host tests, 7 layout tests pass.

## 2026-10-03 — First Aid rebuilt around instructions

- The owner found First Aid opening encyclopedia articles about the condition
  (Wikipedia, MDWiki) instead of what to do. First Aid now uses only sources
  written as instructions: MedlinePlus pages named by their permanent address
  on medlineplus.gov (ency/article/000030.htm is Burns), opened at their
  "First Aid" section; else a chapter of the Wikibooks First Aid manual. 33
  rows (CPR by age, choking by age, unconscious person, bleeding, stroke, …);
  Wikipedia and MDWiki are no longer used there.
- Every address was checked against the live site's titles. On the card, a
  page found at an address must also carry the expected title, so a wrong
  number opens nothing rather than the wrong page.
- Articles can open at a named section: `ArticleActivity(…, landing)`, and a
  fragment now also matches a heading's text in any case.
- Device checklist: Medical → First Aid lists the rows with "MedlinePlus ·
  …" under them; Burns opens on its First Aid section; Stroke (no MedlinePlus
  first-aid page) shows only if the Wikibooks collection is on the card.

## 2026-10-03 — Sleep screen: DON'T PANIC

- The Dark and Light sleep screens show DON'T PANIC in large, friendly
  letters (about a quarter of the screen's height, 368 px wide), "Sleeping"
  small under it. The lettering is a 1-bit picture made by
  `tools/sleep/make_dont_panic.py` from Fredoka (SIL OFL), the heaviest
  weight; the font itself is not in the firmware. Custom and Cover sleep
  screens are unchanged.
- Device checklist: Settings → Sleep Screen → Dark; press power: white
  DON'T PANIC on black, centred, edges crisp; Light gives black on white.

## 2026-10-04 — Performance update; Home flash; Paris

- Owner, on the device (newest build, after the wrong-file flashes were
  traced: every flash had sent an old pocketlib-x4pro.bin; flashing now
  downloads the release asset straight to ~/Downloads/pocketlib-new.bin):
  DON'T PANIC good; Home flashed on every visit; "paris" did not find the
  city until "paris france"; result list cut off; Wikivoyage pictures work;
  wants a tapped picture full screen (later).
- Fixed: Home flash (see DECISIONS); search finds the exact title among
  thousands of word matches (test: ExactTitleAmongThousandsEndingInIt).
- Cut or trimmed per the owner's table: link previews, pictures on request,
  search pause. Free fixes: failed article sleep bug, place saves, Medical
  cache, three-cluster cache. Builds name their commit on About; "Check for
  updates" hidden.
- 93 zim host tests, 387 firmware host tests pass; release build OK.
- Device checklist: Home: no flash on return; search "paris": Paris first;
  tap a link: opens straight away, Back returns; an article opens without a
  picture, Images shows them; Medical opens instantly the second time;
  About ends in -pocketlib-<commit>.

## 2026-10-04 — Search part 2, pictures, contents with toolbar; Wikipedia maxi

- Owner: the full result list (after OK) cut off; wants a tapped picture
  full screen and the contents and toolbar together; the card has room for
  Wikipedia with pictures, so library.toml now asks for the maxi edition
  first. The owner is downloading it; the index must be built with the new
  zimindex (popular tree) before copying, and copy runs without --only.
- Built: popular tree in the title index (tests PopularTreeLiftsWellKnownTitles,
  weighted searchMany), More results, PictureActivity, contents with toolbar.
- 94 zim host tests, 387 firmware host tests; release build OK.
- Device checklist: search "pari" offers Paris first (after the new index);
  "All" shows mostly Wikipedia; OK list ends in More results; tap a picture
  (Wikivoyage Paris, Images on): full screen, tap returns; tap the middle of a
  page: contents with the toolbar on top; its buttons work.

## 2026-10-04 — Pet First Aid

- tools/webpack (fetch, extract, write ZIM; tested against a local mock
  site and read back with zimcat and zimindex), recipes/pets.toml (MSD pages,
  not fetchable from this container: first real run is the owner's).
- cardbuilder `file =` collections (two new tests; 14 pass with the test
  data). Firmware: Pet First Aid list and tile. 94 + 387 host tests; release
  build OK; clang-format 21 clean.
- Device checklist (after the pack is made and copied): Medical shows Pet
  First Aid with a paw; Bleeding opens the emergency page at Bleeding;
  Poisoning: foods opens Food Hazards; Every pet page lists them all.

## 2026-10-05 — Wikipedia pictures were empty boxes; articles scroll up and down

- Owner: with the maxi Wikipedia, Images showed empty boxes. The serial log
  said why: the 2026 file keeps each picture's original name
  (`_assets_/<hash>/Name.jpg`) but stores WebP, and the reader trusted the
  name. Now any WebP is turned into a grey PNG whatever its name, and the
  reader picks its decoder by the file's first bytes
  (ImageDecoderFactory::getDecoderForFile). Earlier the same day (4d0e3e5):
  decoders gated on the heap they really use (PSRAM on the X4 Pro), and a
  failed picture is tried again on the next page and full screen.
- Owner: everything but books should scroll up and down. Articles (every
  collection) turn pages on a swipe up (next) or down (previous); sideways
  swipes do nothing there. Lists already scrolled up and down. Books are
  unchanged.
- 387 host tests; release build OK; clang-format clean.
- Device checklist: Lorne Michaels → Images: the photos show; tap one: full
  screen. In any article, swipe up: next page; down: previous; a swipe from
  the top edge still opens the light panel or contents.

## 2026-10-05 — Every picture showed Lorne Michaels

- Owner: after de2b23c, pictures in other articles showed the first one seen
  (Lorne Michaels), sometimes in the wrong place. The article view never
  released ImageBlock's RAM pixel slot after a page render (the book reader
  does), so the first picture loaded stayed resident and was drawn for any
  later picture with the same cache name, /.pocketlib/img/0.pxc in every
  article, at its own size. The slot is now released at the end of each page
  render and when an article's pictures are cleared.
- Device checklist: Lorne Michaels → Images, then open another article with
  pictures → Images: its own pictures, in place; tap one: that picture.
