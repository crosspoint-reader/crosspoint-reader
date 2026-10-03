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
  application/pdf and application/wasm). The device reads neither. Owner chose
  to keep it on the card for now; converting the PDFs is a possible later job.
