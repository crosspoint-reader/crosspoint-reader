# Pocket Library — Plan

Base: CrossPoint **1.6.5** (tag `1.6.5`, commit `93e98bb`), env `x4pro`.
Status date: 2026-10-01.

## Status

| # | Milestone | Status |
|---|---|---|
| 0 | Foundation: fork, build, docs, debug screen, first flash | **In progress** — both firmwares build on GitHub Actions; waiting on owner's backup and device test |
| 1 | ZIM core library + host tests + Mac CLI | Not started (test files located: openZIM `zim-testing-suite`) |
| 2 | Card builder (Python, Mac) | Not started |
| 3 | First article on device | — |
| 4 | Search | — |
| 5 | Full reader | — |
| 6 | Whole library (shelves, cross-search, Wiktionary, CJK) | — |
| 7 | Power and polish | — |
| 8 | Atlas (stretch) | — |
| 9 | Release | — |

## Milestones as I understand them

**M0 Foundation.** Build unmodified 1.6.5 for X4 Pro; add `POCKET_LIBRARY` build
flag and our envs; hidden Diagnostics screen (Settings → About → tap
"Firmware" 5×) showing PSRAM, internal RAM, flash, the SD bus as the driver
actually clocked it, and an SD benchmark (sequential MB/s; random 4 KB
avg/p95/max). Owner flashes stock first, then ours.

**M1 ZIM core (`lib/zim/`, host-testable C++).** Header, MIME list, dirents
(old `A/` and new `C/` namespaces), path lookup, title lookup (header title
list and `X/listing/titleOrdered/v0|v1`), redirects, cluster decode (none,
zstd, xz), extended 64-bit blob offsets, split files, a small cache. A storage
interface so the same code runs on the Mac (stdio) and the device (HalFile or
raw sectors). Tests against `zim-testing-suite` (real 2024 Wikipedia mini
file, a split `.zimaa/ab/ac` copy, both namespace schemes, ~25 deliberately
corrupt files). CLI: `zimcat <file|first-part> "<title>"`.

**M2 Card builder (`tools/cardbuilder/`, Python 3).** `library.yaml` → Kiwix
catalog → resumable download + checksum → split > 4,000 MiB → sidecar title
index → `manifest.json` → rsync to card, dry run, size report. Idempotent.

**M3–M7** as in the brief. **M8** atlas: tile builder + PMTiles-like pack +
GeoNames/Wikipedia-coordinate indexes + viewer. **M9** release.

## Risks I see now (ranked)

1. **FAT32 seeks in 4 GB parts.** SdFat finds a byte offset by following the
   file's cluster chain; a backward seek restarts from the first cluster. In a
   4 GB part with 32 KB clusters that's up to 131,072 chain links, about 1,000
   FAT sector reads, per lookup. Search does ~25 random lookups per keystroke.
   *Mitigation:* on open, walk each part's chain once and store its extents
   (a copied file is usually one contiguous run), then read by sector through a
   small, mutex-guarded HalStorage extension. The M0 benchmark measures the raw
   cost first, on your largest file.
2. **SD throughput.** 1-bit SDMMC clocked at **20 MHz** (the SDK's "40 MHz"
   comment is wrong: `SDMMC_FREQ_DEFAULT` is 20,000 kHz), with reads bounced
   through a 4 KB DMA buffer. Ceiling about 2.5 MB/s, likely ~1.5 real.
   *Experiment later:* `SDMMC_FREQ_HIGHSPEED` (40 MHz) and larger DMA
   transfers, SDK-side, tested with your OK.
3. **PSRAM budget (resolved 2026-10-02).** 8,080 KB free on device, so three
   2 MiB clusters fit. Original worry: three 2 MiB decompressed clusters = 6 MiB, likely too much. Proposal:
   keep **one** decompressed cluster plus a small cache of extracted article
   blobs and compressed clusters (~250 KB each). Decide after M0 reports free
   PSRAM.
4. **zstd window.** Current Kiwix clusters declare an 8 MiB zstd window.
   Decode each cluster in one pass into its own 2 MiB buffer (no window
   allocation) rather than streaming. Cap the decoder's window anyway and
   reject anything larger with a clear error.
5. **Search latency over ~18 M title entries** (English Wikipedia articles
   plus redirects). The sidecar index (option b) also fixes case and accents;
   I expect to choose it in M3/M4 and will measure both.
6. **Content not reachable from this cloud session.** The network policy
   blocks `download.kiwix.org` and `wiki.openzim.org`. M1 can proceed on the
   openZIM test suite from GitHub; the spec and the catalog need either an
   allowed-domain change or the owner's Mac.
7. **Panel varies by batch** (SSD1677, UC8179 or UC8279); refresh timings must
   be measured on your unit. Diagnostics will name the controller.
8. **Flash space.** The app slot is already 86% full (~915 KB free) before any
   ZIM code. Measure every build; trims or a repartition if needed.
9. **Upstream drift.** `main` is already 17 commits past 1.6.5. Rebase per
   release (see DECISIONS.md).

## Open questions

See the latest `PROGRESS.md` entry.
