# Measure page rendering on an S3 reader

Measure each device against its own baseline. Use the same book, SD card, configuration, and room temperature for both builds.
The serial log measures firmware work. A phone video measures the delay that you see after a button press.

## Choose a test book

Use [test_page_render_benchmark.epub](../test/epubs/test_page_render_benchmark.epub) for the first comparison.
This is the same generated English book used for the emulator measurements.
It contains two chapters of numbered paragraphs, with no images, CSS, or embedded fonts.

Copy the file to your SD card and start at the beginning of Chapter A.
If Chapter A has fewer than 31 pages on your device, use a larger font or a longer chapter.
You can then repeat the comparison with your own EPUB, while keeping the same book and configuration in both builds.

## Prepare the two builds

Keep a copy of the baseline firmware before you install the changed firmware.
If you need to build the baseline, create a separate checkout:

```sh
git worktree add --detach /tmp/crosspoint-render-baseline a4f64a3b6
git -C /tmp/crosspoint-render-baseline submodule update --init --recursive
cd /tmp/crosspoint-render-baseline
pio run -e x4pro
```

Return to your implementation checkout before you build the changed firmware.
Use your usual project directory in `cd`. Do not switch branches inside either dirty working tree.
The baseline for this change is reader `a4f64a3b6` with FreeInk SDK `a8acebf919`.
The implementation is on reader branch `perf/s3-page-rendering` and SDK branch `perf/s3-display-transfers`.
The reader commit records the SDK revision it needs. Keep that submodule revision when you check out this implementation.

Use a development environment with debug logs enabled:

| Device | PlatformIO environment |
| --- | --- |
| X4 Pro | `x4pro` |
| X4 Classic | `x4c` |
| EEGO A4 | `eego_a4` |
| Paper Mono | `papermono` |
| Metalio EInk4 | `metalio_eink4` |
| Picco | `picco` |
| X3 or original X4, for comparison | `default` |

Build with `pio run -e x4pro`. Replace `x4pro` with your device environment.
Install the matching firmware with your normal upload method.
If you build a baseline in another checkout, use the same local build flags in both checkouts.

## Record page times

1. Use a plain-text EPUB with at least 31 pages in one chapter and no images.
2. Select the same font, size, orientation, and margins in both builds.
3. Enable text anti-aliasing and select a full refresh every 15 pages.
4. Disable automatic page turns and turn Wi-Fi off.
5. Open the test chapter once and wait until its build finishes.
6. Return to Home before you start the recording.
7. Start the serial monitor at 115200 baud and save its output:

   ```sh
   pio device monitor --port /dev/ttyACM0 --baud 115200 | tee baseline-x4pro.log
   ```

8. Open the book at your chosen starting position.
9. Wait until the first page and the chapter build finish.
10. Turn forward exactly 30 pages, one press at a time.
11. Wait until each page finishes before the next press.
12. Stop the monitor with Ctrl-C.
13. Install the changed build and return to the same starting position.
14. Repeat the recording as `optimized-x4pro.log` with identical configuration.

Replace `/dev/ttyACM0` with your serial port. Use `pio device list` to find it.
Keep all 30 turns inside that already-built chapter.
If an indexing or build popup appears, restart after the chapter finishes its build.
Make sure that each log contains 31 `Page render` lines: the opening page and 30 turns.
Menus, rotation changes, and manual refreshes add renders or reset the refresh schedule. Restart the recording after these actions.

Compare the logs:

```sh
python3 scripts/compare_page_render.py baseline-x4pro.log optimized-x4pro.log
```

Read `ordinary.median_ms` for the typical ordinary turn.
Read `cleanup.median_ms` for the two scheduled full refreshes.
`ordinary_time_reduction_percent` reports how much less time the changed build takes. A positive result is faster.
Repeat the complete comparison three times before you draw a conclusion.

The `total` timer includes font preparation, drawing, display transfers, panel waits, and grayscale cleanup.
It starts after the page loads from SD. It ends before the firmware saves reading progress.
It therefore excludes part of the button-to-page delay.

## Check the visible delay and quality

Record the button and the screen together at 120 or 240 frames per second.
Count frames from the button press to the settled page. Divide by the recording frame rate to get seconds.
Compare ordinary turns and full refreshes separately. Use the same exposure and room temperature for both builds.

Repeat shorter quality tests with these cases:

- Text anti-aliasing off and on.
- Portrait, inverted portrait, and both landscape orientations.
- A built-in font and an SD font with many different characters.
- Grayscale images, black-and-white images, and a chapter boundary.
- Dark mode, clipping highlights, and the status bar.
- Backward turns, a manual full refresh, sleep, and wake.

Make sure that text, images, and reading positions match the baseline.
Look for shifted rows, missing gray edges, stale text, extra flashes, and incomplete refreshes.
For images and mixed workloads, use `--cleanup-every 0` to compare all turns together.
Their refresh schedule differs from the plain-text test. Use that option for chapter-boundary tests too.

## Check memory and the selected transfer path

DMA sends bytes through the SPI hardware while the CPU can wait.
An eligible device prints `[EPD] DMA enabled: 4096-byte staging, ... Hz` during boot.
If that line is absent, the device uses its existing Arduino SPI path.
Shared SD/display SPI buses keep that path. The SPI clock stays at its previous value.

The firmware prints `[MEM] Free` for internal memory and `[MEM] PSRAM` for external memory every ten seconds.
`MaxAlloc` is the largest block available for one allocation.
Record these lines at Home, inside the book, and after you close the book.
Open and close books repeatedly. Make sure that free memory and `MaxAlloc` settle instead of falling each time.
On X3 and original X4, keep more than 50 KB of internal memory free.

The reader retains two gray planes until its activity is destroyed.
Their combined size is `2 × physical width ÷ 8 × physical height` bytes in PSRAM.
That is 96,000 bytes on X4 Pro and 105,984 bytes on EEGO A4.
Each SD font can retain another 16 KiB for grouped reads until its caches are released.
The display bus retains a 4 KiB internal buffer plus ESP-IDF driver storage when DMA setup succeeds.
If optional allocations fail, the firmware uses its previous rendering or read path.

## What each optimization does

PSRAM is extra memory outside the processor.
A gray plane is a bitmap used to represent gray shades.
A glyph is the drawn shape of a character.

### Larger grayscale strips

Previously, the reader drew gray content in 80-row strips and repeated the page drawing work for each strip.
On PSRAM boards, the reader keeps two full-height gray buffers and draws each plane across the full screen.
If allocation fails, it uses the previous smaller-buffer path.

### Draw both gray planes together

Previously, the reader walked the page once for each gray plane.
Text pages now produce both gray planes during one walk through the page.
This uses the two PSRAM buffers, while image pages keep their separate drawing passes.

### Send consecutive display bytes together

Previously, the UC8279 display driver copied visible rows into a small temporary buffer before it sent them.
It now sends consecutive bytes directly from the existing page buffer when no inversion or mirroring is needed.
This needs no extra memory and keeps the panel padding, row order, and display clock unchanged.

### Use DMA for display transfers

Previously, the processor fed display data through Arduino SPI in small chunks.
Eligible ESP32-S3 boards now gather writes in a 4 KiB buffer and let the SPI hardware send each batch.
The PSRAM build flag guards this path, and shared buses or failed setup keep their previous transfers.

### Group nearby font reads

Previously, font preparation read each glyph bitmap separately from the SD card.
It now groups nearby bitmaps into reads of at most 16 KiB, with small gaps allowed between them.
Each font reuses a PSRAM buffer, and allocation failure keeps the previous per-glyph reads.

### Keep pixel writes inside the drawing loop

The glyph renderer writes many individual pixels for each character.
Its small helper stays inside the drawing loop, so each pixel needs no separate function call.
This keeps the shared drawing code fast when it draws either one gray plane or both planes together.

### Keep the existing refresh overlap

Some controllers already let the reader draw gray data while a black-and-white refresh runs.
This change keeps that existing render task and capability check.
It adds no display task or queue, so it also avoids the stock firmware's extra page snapshots.

## Compile guards and stock-firmware ideas

| Change | Guard and reason |
| --- | --- |
| Full-height gray buffers | `BOARD_HAS_PSRAM`. Allocate in PSRAM only and retain the 80-row fallback. |
| Draw both gray planes together | `BOARD_HAS_PSRAM`. Use the two retained planes for text pages. |
| Group nearby SD-font reads | `BOARD_HAS_PSRAM`. Use a bounded 16 KiB buffer without internal-memory fallback. |
| DMA and 4 KiB transfer batches | Arduino + `BOARD_HAS_PSRAM` + ESP32-S3. Also require an exclusive display bus and successful setup. |
| Contiguous UC8279 visible-plane writes | No PSRAM guard. Reuse existing bytes and preserve padding, inversion, and row order. |
| Draw while a base refresh runs | Reuse the existing render task and controller capability. Add no task or pending-page queue. |

The stock firmware also uses a separate display task and several page snapshots.
This implementation keeps CrossPoint's existing task ownership and refresh overlap.
It keeps the existing waveforms, grayscale cleanup, and display clock.

The X4 Pro emulator results are recorded under `.artifacts/s3-page-rendering-2026-11-15/`.
Those results do not establish device speed or verify the original stock-firmware claim.
