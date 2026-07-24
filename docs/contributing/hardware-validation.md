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
- Hold Volume Down + Power from a cold start. Confirm the safe-start notice appears, SD fonts and saved stores are skipped for that boot, continuing reaches Home, and none of the skipped files change. Also force one crash at each guarded startup stage and confirm only the interrupted stage is skipped on recovery.
- Check every supported orientation for clipped text, content under physical
  buttons, ghosting, unexpected full refreshes, and stuck busy states.
- Exercise Classic, Dashboard, and CrossVi Home themes. Confirm an empty library, a
  missing recent book, a book without a cover, and a corrupt cover thumbnail
  all remain usable.
- In CrossVi, verify a blank, short, Vietnamese, and overlong device display
  name; confirm the model-name fallback and ellipsis never overlap the battery.

## 2. Dashboard and statistics

- Open a new EPUB, a partly read EPUB, and a completed EPUB. Confirm Dashboard
  never treats a partial chapter cache as the final page count.
- Verify title, author, chapter, page count, progress, time, session count,
  streak, local-device totals, and Nearby totals against known test data.
- Open Reading statistics from both Home and the Reader menu; inspect every
  page in portrait and landscape.
- From **This device**, open the reading calendar. Check current-day outline, read-day markers, an empty month, leap February, a six-row month, navigation at the 730-day boundary, and the unavailable-clock message on both X3 and X4.
- Read a TXT/Markdown book for more than ten active seconds, turn pages, then
  press Confirm in the text reader. Verify time, page-turn count and estimated
  byte progress; page pace and finish-time estimates must remain not applicable.
- Reopen that text book after changing font, margin and orientation. It must
  resume at the same byte-anchored text, not merely the old layout page number.
- Replace a TXT file with different content at the same path (including a
  same-size replacement). The new book must not inherit the old progress,
  completion or per-book statistics; retained/quarantined old state must not
  be silently deleted.
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

## XTC/XTCH hardware release gate

Run this complete matrix on both a physical X3 and X4. Use the frozen
converter-provenance fixtures from `test/xtc/resources/` plus a longer,
copyright-free 480×800 v1.0 book with chapters and cover metadata.

- Open one XTC and one XTCH from Browse Files, cold and from a warm recent-book
  reopen. Confirm title, author, chapters, cover, Home thumbnail and sleep cover.
- Turn forward and backward at least 50 times, jump chapters, reboot, and
  verify that only the last successfully displayed page is restored.
- Exercise hidden, top and bottom status bars. On X4, compare a four-corner
  marker screenshot pixel-for-pixel with the 480×800 source. On X3, verify all
  four source edges/corners remain visible, the aspect ratio is unchanged and
  only centered letterboxing is added.
- On XTCH, verify the converter's white, dark-gray, light-gray and black test
  regions remain distinct on the panel. Record ghosting and refresh behavior;
  simulator grayscale is not panel validation.
- Replace book A with same-size book B at the exact same path. Confirm B gets
  none of A's progress, cover, thumbnail or chapter-derived state. Then move an
  unchanged file through the supported UI and confirm its state follows the
  existing move transaction.
- Repeat book changes and sleep/wake cycles while recording minimum free heap,
  largest free block, reset reason, out-of-bounds logs and displayed/status/
  saved page agreement.

Until this matrix passes on both models, report XTC/XTCH as **host/simulator
validated, pending X3/X4 hardware validation**, not stable or hardware
validated.

## Clock, upload and tilt hardware checks

- X3: boot and wake with Wi-Fi disabled after a battery-backed RTC calendar is
  set. Verify Reading Stats date, weekday and streak, then NTP-sync and confirm
  a later offline reboot retains the complete UTC calendar.
- X4: boot with an invalid system epoch, connect Wi-Fi and verify NTP is still
  offered even when a previous-sync flag is stored. Check a local UTC-offset
  change and midnight rollover without a double timezone shift.
- Upload a replacement font, then interrupt each stage using a spare card:
  during chunks, before publish and after backup rotation. The prior font must
  remain usable or recover on restart; Back/restart must close staging first.
- X3 tilt: open a reader, cover it with chapter/statistics/nested modal screens,
  tilt, then return. No queued gesture may turn the page. Active-reader tilt
  must still work. Confirm the same flow does not change X4 input behavior.

## Deferred performance benchmark matrix

The findings below are measurement tasks, not authorized production changes.
Use the same firmware, SD card and input corpus before/after any future patch.

| Deferred finding | Input and repetitions | Measure | Required environment |
| --- | --- | --- | --- |
| EPUB page LUT RAM | EPUBs with small, medium and extreme page counts; 10 cold/warm opens | minimum heap, largest block, LUT bytes, first-page latency | X3 and X4 hardware |
| File Browser names | 100, 500 and 2,000 mixed-length entries; 20 enter/leave cycles | minimum heap, largest block, input latency | simulator for behavior; hardware for heap/latency |
| TXT/Markdown index | 1, 10 and 100 MiB files; 10 cold/warm opens | time to first page, bytes read, SD reads, peak/minimum heap | hardware with fixed SD card |
| Grayscale EPUB cache | image-heavy EPUB at current strip policy; 20 pages twice | decode time, cache hit/miss, minimum heap, largest block | X3 and X4 hardware |
| Web/Calibre batching | 1, 10 and 100 MiB transfers; 10 runs | throughput, Back latency, watchdog resets, minimum heap | hardware and controlled Wi-Fi |
| Home refresh | empty/covered/uncovered recent book; 50 entries to Home | full/partial refresh count and region, cover-ready latency, ghosting | simulator call counts plus both panels |
| Partial EPUB rebuild | large chapter, interrupt at several build percentages; 20 sleep/wake cycles | wake time, cache correctness, SD writes, progress retention | hardware |

Desktop wall-clock time and PlatformIO static RAM are not substitutes for
ESP32-C3 latency, peak heap or largest free allocation. Do not change batching,
pagination, grayscale strips, Home updates or background rebuild behavior until
these measurements reproduce a concrete problem and the candidate patch is
benchmarked against the same corpus.

## Release gate

A stable release requires all automated gates plus this checklist on both X3
and X4. Nearby also requires a two-device run. Record failures and exact logs;
do not replace an unverified result with “looks fine” or infer hardware safety
from a successful firmware build.
