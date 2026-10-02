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

## 2026-10-02 — First device measurements change two plans

- **PSRAM is nearly all ours.** 8,080 KB free with CrossPoint running, so the
  brief's 3 × 2 MiB decompressed-cluster LRU fits with ~1.9 MB to spare.
  Earlier worry (PLAN risk 3) withdrawn; the cache size stays a setting.
- **The card is slow but steady.** 1.93 MB/s sequential and ~1.9 ms per
  random 4 KB read with a tight tail. Projected cost of an uncached article:
  ~250 KB compressed cluster ≈ 130 ms to read, plus decompression (to be
  measured). Search: ~25 probes × ~2 ms ≈ 50 ms if, and only if, seeks in
  4 GB parts stay cheap; that test needs a real 4 GB file.

## Dependencies

| Dependency | License | Use | Status |
|---|---|---|---|
| CrossPoint Reader 1.6.5 | MIT | base firmware | in use |
| freeink-sdk | MIT | hardware layer | in use (upstream) |
| Upstream's own deps (ArduinoJson MIT, QRCode MIT, PNGdec Apache-2.0, JPEGDEC Apache-2.0, WebSockets LGPL-2.1, Arduino-wolfSSL GPL, FreeType FTL/GPL-2, expat MIT, miniz MIT) | as listed — to verify one by one at M1 | upstream features | in use (upstream) |
| zstd (decoder only) | BSD-3-Clause (dual BSD/GPLv2) | ZIM clusters | planned M1 |
| xz-embedded | 0BSD (public domain before 2024) | old ZIM clusters | planned M1, only if our collections need it |
| zim-testing-suite (openZIM) | test data | host tests | planned M1 |

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

## Measurements

### Device (fill in from Diagnostics and serial logs)

| Quantity | Value | Date | Notes |
|---|---|---|---|
| Chip | ESP32-S3 (QFN56) rev v0.2, dual core 240 MHz, 40 MHz crystal | 2026-10-02 | esptool 5.4.0 |
| PSRAM size | 8 MB embedded (AP_3v3) per esptool; firmware value pending | 2026-10-02 | confirm with `esp_psram_get_size()` |
| PSRAM free at Diagnostics | 8,080 KB free, largest block 8,063 KB | 2026-10-02 | CrossPoint 1.6.5 barely touches PSRAM |
| Internal RAM free / largest | 183 KB / 135 KB | 2026-10-02 | |
| Flash size / speed | 16 MB @ 80 MHz | 2026-10-02 | Diagnostics + esptool |
| SD bus | | | expect "SDMMC 1-bit @ 20.0 MHz" |
| Panel controller | | | Settings → About → Display Controller |
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

### Performance targets (brief §7) — measured values arrive from M3 on

| Action | Target *(est.)* | Measured |
|---|---|---|
| Search update per keystroke | ≤ 100 ms | |
| Open article, cluster not cached | ≤ 800 ms | |
| Open article, cluster cached | ≤ 300 ms | |
| Page turn layout | ≤ 100 ms | |
| Wake to usable screen | ≤ 1 s excl. refresh | |
