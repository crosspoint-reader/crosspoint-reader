# CrossPoint 2.0 for Murphy M3

This is a downstream port based on official CrossPoint `2.0.0`, commit
`42d58daaabcf469a2e5e1ba871714fb9dcf51d29`. Its SDK starts at
`5e94fe7480f019c89fbf691aef87523eb1b46704` with local Murphy-only hardware changes.
The local branch is `feat/murphy-m3-2.0`. A local port is available; publishing a
The remote fork is https://github.com/ShiroDXD/crosspoint-reader, connected as
`fork`; `origin` remains the official upstream. The Murphy port is maintained on branch `feat/murphy-m3-2.0`.

## Status

This is an unverified hardware preview, identified in the UI as
`2.0.0-murphy-m3-preview1`. Host tests check software contracts, but do not prove
that this binary boots or that display waveforms, touch, SD, light, power, or
battery readings work on a retail unit. Do not describe it as hardware-ready
until the device checks below pass.

The target is the touchscreen Murphy M3 / HamGeek M3, with 16 MB flash and 8 MB
PSRAM. This is not a Murphy M4, Xteink, Paper Mono, or PaperS3 firmware image.

## Port decisions

The SDK board profile uses the SDMMC wiring from MR_TBOT's M3 hardware port:
CLK16, CMD17, D0=15, D1=14, D2=21, D3=18, and the active-low GPIO10 SD rail.
GPIO43 enables the shared I2C rail and GPIO45 powers touch, active-low. The
battery divider is 2.0. Touch uses the calibrated swapped axes and Y inversion,
and a STOP-separated CHSC6x read with a 10 ms bus timeout. The existing 2.0
input frame and UI ownership are retained. Idle sampling on M3 stays at the
existing 10 ms slice rather than waiting through five slices for a brief IRQ.

The display uses the mode-0 default and destination-drive fast LUTs from
MR_TBOT's SDK commit `13e7c5bd2b0868378c1f765dc6096339fa96e8d9`, which credits
the community SDK (`684effb`) and OEM touch-v525 firmware. Both controller planes
receive the new frame. This is intentionally a different refresh path from the
previous local 1.5 preview's differential ALT bank: it needs no previous-image
buffer and adds no framebuffer allocation. A first paint cleans the panel;
four fast paints are followed by a cleaning paint. Ghosting quality needs a
physical test. This cadence can cause cleaning more often than the reader's
page-count setting suggests. Monochrome rendering is used; grayscale overlay
requests are ignored rather than displaying a gray bit plane as a black page.

The 2.0 reading/UI architecture is retained. Legacy 1.5 Settings layout code,
MOBI/PDF conversions and audio player are not copied into this port. Hardware
metadata may describe the codec, but this build has no audio-player feature.
The original 1.5 workspace and binaries are preserved in the adjacent
`Murphy-M3` directory.

## Build and software checks

On a fresh checkout, initialize the pinned SDK and apply the reviewed downstream
patch before building. The current workspace already has this patch applied;
do not apply it a second time.

```sh
git submodule update --init --recursive
# Fresh SDK at 5e94fe7480f019c89fbf691aef87523eb1b46704 only:
git -C freeink-sdk apply ../patches/murphy-m3-sdk.patch
```

Then, from this repository:

```sh
pio run -e murphy
python3 test/murphy_display/run.py
sh freeink-sdk/libs/hardware/InputManager/test/host/run_murphy.sh
sh freeink-sdk/libs/hardware/InputManager/test/host/run.sh
python3 freeink-sdk/libs/hardware/BoardConfig/test/host/run_serial.py
python3 freeink-sdk/libs/display/FreeInkDisplay/test/host/run_pro.py
```

Build logs and test records are stored in ignored `build/`. The Murphy touch
suite compiles the actual InputManager and BoardConfig with hardware stubs. It
checks rails, SD/battery metadata, STOP-separated reads, both calibrated corners,
and a single release/tap event without phantom button presses. The panel suite
checks actual plane bytes, selected LUT banks, first-paint and periodic cleaning,
no grayscale refresh, and sleep invalidation. They do not simulate physical ink,
capacitive sensing, or SD electrical timing.

## Local build result

`pio run -e murphy` succeeded. The application and combined factory image are in
`build/murphy-m3-2.0.0-preview1/`, alongside SHA-256 checksums, image validation
output and a manifest. The image checksum/hash, Murphy board tag, configured
application-slot fit and combined-image component offsets were verified. The
four independent code reviews and targeted host suites passed after correcting
SDMMC backend selection; see `build/reviews.txt` for records. The owner accepted the refresh cadence, touch polling power tradeoff and
maintenance responsibility on 2026-11-06. Hardware validation remains pending.

## Backup and installation

This task does not flash a device. Preserve a complete verified stock/current
flash backup and a separate copy of the SD card before testing. Full-flash dumps
can contain Wi-Fi credentials; keep them private.

The PlatformIO output `.pio/build/murphy/firmware.bin` is an application-only
image, for the documented application slot at `0x10000`; it must not be written
at address zero. An app-only flash preserves the partition table, so first
verify that your installed partition layout provides enough room for the built
image. The build checks its own 0x640000-byte slot; that does not inspect the
partition table on your reader. The `.factory.bin` artifact is a combined bootloader, partition table, OTA
selector and application image for offset `0x0`. It also overwrites the NVS
region with erased bytes, clearing stored credentials/settings there. Do not
use it as an app-only update or write it at `0x10000`.
Use the matching 16 MB partition table and documented recovery procedure if a
layout change is necessary. SD updates may also require the current updater's
board-tag support; do not assume the old 1.5 updater validates 2.0 images.

## Device acceptance checklist

1. Confirm normal boot, correct portrait screen orientation and frontlight.
2. Mount the SD card, list books, open a small EPUB, turn forward/back and reopen
   after sleep. Check a larger EPUB, custom fonts, images and Wi-Fi separately.
3. Tap the centre and corners of visible controls once. Expect the selected row
   to match the touched row. Tap two different rows with a full finger lift.
4. Leave Settings idle for 60 seconds, then navigate with buttons and touch.
   Expect no continuous idle flashing, no version/tab overlap, immediate menu
   updates and periodic cleaning flashes. Record any failure with serial logs.
5. Repeat navigation and reading in portrait, inverted portrait, landscape CW
   and CCW, including any existing button remapping. Check no inherited
   release activates a newly opened or resumed menu.
6. Exercise all three physical buttons, short/long presses, shutdown and wake
   with USB attached and detached. Keep ROM-download recovery accessible.
7. The selected SDK backend uses 40 MHz SDMMC; the earlier community M3 port
   used 20 MHz. Check sustained reads/writes and wake remounts with the actual
   card, and record any CRC, timeout or card-initialization failures.
8. Record internal and PSRAM free bytes and largest blocks through repeated
   book/settings/font/Wi-Fi transitions. Check for leaks, allocation failures,
   battery plausibility and overnight sleep drain. Faster touch polling can
   increase awake idle power use.

The human owns device validation and acceptance of the destination-drive
waveforms, periodic cleaning, faster idle polling and ongoing maintenance of
this downstream SDK patch. No PR or remote publication is automatic.
