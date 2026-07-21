# CrossVi Hardware Validation Checklist

CrossVi's host tests and firmware builds cannot validate the e-paper panel,
button wiring, ESP-NOW radio, real SD-card timing, or peak heap on an Xteink
X3/X4. Complete this checklist before publishing a stable release.

## Test setup

- Use an X3/X4 Developer Edition or another device whose USB flashing and
  recovery path are already known to work. Do not use CrossVi as an Xteink
  Unlocker payload.
- Back up the complete SD card and use a spare card for power-loss and I/O-fault
  trials.
- Keep a known-good recovery firmware, USB cable, serial log, and charged
  battery available.
- For Nearby Sync, use two recoverable devices and the exact same EPUB on both.
- Record the commit, device model, panel/board revision, SD-card model, free
  space, orientation, and test EPUB for every run.

Stop immediately if the panel repeatedly flashes without settling, reports
busy timeouts, becomes abnormally warm, or no longer responds to a normal
reset. Preserve the SD card and serial log before attempting recovery.

## 1. Boot and display

- Flash the exact candidate binary and confirm the displayed build identifies
  the expected commit.
- Cold boot, warm reboot, sleep, wake, and repeat on both X3 and X4.
- Check every supported orientation for clipped text, content under physical
  buttons, ghosting, unexpected full refreshes, and stuck busy states.
- Exercise Classic and Dashboard Home themes. Confirm an empty library, a
  missing recent book, a book without a cover, and a corrupt cover thumbnail
  all remain usable.

## 2. Dashboard and statistics

- Open a new EPUB, a partly read EPUB, and a completed EPUB. Confirm Dashboard
  never treats a partial chapter cache as the final page count.
- Verify title, author, chapter, page count, progress, time, session count,
  streak, local-device totals, and Nearby totals against known test data.
- Open Reading statistics from both Home and the Reader menu; inspect every
  page in portrait and landscape.
- Read across midnight with a valid clock. With an invalid clock, confirm
  undated totals remain usable while current streak/date estimates are shown
  as unavailable instead of fabricated. Then establish a streak, jump the
  clock far into the future, read, restore the correct date, and confirm the
  known streak history was not erased.

## 3. Indexing and reader performance

- Follow the [EPUB index benchmark](./epub-index-benchmark.md) with a small,
  medium, and unusually large chapter.
- Measure first open, cached reopen, page turn, chapter change, cache hit/miss,
  and largest free allocation. Keep the serial log.
- Reopen a chapter while its cache is partial, change font/margin/orientation,
  and rebuild a deliberately damaged derived cache. Progress must survive.
- Compare input latency with CrossInk v1.4.0 using the same book and SD card;
  do not claim a speed improvement from desktop timings alone.

## 4. Cache and per-book settings

- Set a distinct font, size, margin, line spacing, orientation, and auto-turn
  interval for one EPUB. Reboot, disable the profile, re-enable it, then reset
  it using the two-step confirmation.
- Run **Delete Book Cache**. Confirm reading position, per-book settings,
  statistics, bookmarks, and clippings remain unchanged while generated
  layout/index files rebuild.
- Confirm a newer or damaged settings file is preserved and reported rather
  than overwritten.

## 5. Clippings

- Save a single-page clipping and a clipping spanning adjacent pages in one
  chapter. Cancel before and after setting the first selection point.
- Test punctuation, RTL text, multibyte Vietnamese text, the 256-word/512-byte
  bounds, the end of a chapter, and a failed next-page load.
- Browse per-book and all-book Clippings, jump to the exact start, delete one
  item with confirmation, and export twice. Existing exports and the clipping
  library must not be overwritten.
- Change layout after saving. Text must remain available; an uncertain
  underline or jump must not be shown as exact.

## 6. Nearby Sync

- Verify explicit **Send** and **Receive** roles, matching pairing codes, sender
  identity, book/chapter preview, and the incoming position before Apply.
- Drop packets, repeat an offer, restart one side, and begin a new session.
  Old ACKs/offers must not apply and the receiver must never send its old
  position back automatically.
- Apply only with a finalized chapter cache. Confirm a partial cache, an
  ambiguous paragraph mapping, a different EPUB, and a missing target section
  are rejected without changing the receiver's saved position.
- Sync statistics twice from the same device. Totals must not double-count or
  replace a newer saved record with an older one.
- Treat the four-digit code as pairing verification only; Nearby traffic is
  not encrypted.

## 7. Migration and storage faults

- On a copied CrossInk v1.4.0 SD image, test settings, clippings, per-book stats,
  global stats, and saved peer stats. Confirm every original CrossInk file and
  versioned backup remains byte-for-byte available after migration.
- Reboot during each supported publish/migration operation using only a spare
  SD card and recoverable device. On restart, CrossVi must load a verified
  primary/backup/temp or fail closed without deleting the source.
- Repeat with an almost-full card, read-only/failing card, corrupt temp,
  corrupt backup, and unsupported newer version. Never hot-remove the user's
  real SD card; simulate faults with a spare card or adapter.

## Release gate

A stable release requires all automated gates plus this checklist on both X3
and X4. Nearby also requires a two-device run. Record failures and exact logs;
do not replace an unverified result with “looks fine” or infer hardware safety
from a successful firmware build.
