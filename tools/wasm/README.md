# Browser preview

Run the CrossPoint reader in a browser without connecting or flashing a device.
The preview compiles the firmware's C++ application and rendering code to
WebAssembly, using the official CrossPoint simulator for hardware and FreeRTOS
compatibility. X4, X3, and X4 Pro have separate builds with their own display
geometry and input capabilities.

## Build and run

Requires Python 3.10+, PlatformIO, Ninja, and **Emscripten 6.0.6** on macOS or
Linux. Initialize the SDK submodule, activate Emscripten, then build:

```sh
git submodule update --init --recursive
source /path/to/emsdk/emsdk_env.sh
python3 tools/wasm/build.py --device x4
python3 tools/wasm/build.py --device x3
python3 tools/wasm/build.py --device x4pro
```

Each build produces a self-contained HTML file in `build/preview/`:

- `crosspoint-x4.html`
- `crosspoint-x3.html`
- `crosspoint-x4pro.html`

Double-click the file for your device, or open it in Chrome. Each file works
on its own, offline, with no server or installation. It includes the firmware,
sample book, frontend, build information, and license notices. The virtual SD card manager works the same way as in the hosted preview. Each file contains one device;
open a different file to try another device.

The multi-device website remains available through `index.html`. Serve it with:

```sh
python3 -m http.server 8099 --bind 127.0.0.1 --directory build/preview
```

Open <http://127.0.0.1:8099>. Any ordinary static HTTP server works. No HTTPS,
COOP/COEP headers, service worker, or SharedArrayBuffer is required.
`python3 tools/wasm/serve.py` is a development convenience that disables caching.
Use a `crosspoint-*.html` file for direct opening; `index.html` still loads
separate files and needs HTTP. Selecting an unbuilt device in the hosted site
reports an error in the diagnostic log.

The build script obtains a fresh compilation database with
`pio run -c platformio.preview.ini -e preview-x4 -t compiledb`, then compiles
those sources with Emscripten. Ninja tracks source, header, and command changes.
The separate PlatformIO configuration leaves device build environments
unchanged. Outputs live under the ignored `build/preview/` and `.cache/wasm/`
directories; generated binaries must not be committed.

To install a local toolchain without changing an existing Emscripten setup:

```sh
git clone https://github.com/emscripten-core/emsdk.git .cache/wasm/emsdk
git -C .cache/wasm/emsdk checkout b44154299bdefe75ba287874bb63fc9806243771
.cache/wasm/emsdk/emsdk install 6.0.6
.cache/wasm/emsdk/emsdk activate 6.0.6
source .cache/wasm/emsdk/emsdk_env.sh
```

## Try a build

Choose **Browse Files**, open **books**, then open **A Small Book of Pages**.
On **X4 and X3**, use the on-screen buttons, or focus the display and use arrow
keys, Enter, and Escape. Their screens do not accept touch input.

On **X4 Pro**, click or tap items directly on the screen. Click and drag horizontally
with a mouse, or swipe on a touchscreen, to turn pages. Tap the center to open
the reader toolbar with the default settings. Page-edge taps depend on the reader's gesture settings; the firmware
defaults to swipe-only page turns. Enable **Tap & Swipe** for **Next Page** and
**Previous Page** in **Settings → Controls** to turn pages by tapping their edges.
Mouse flicks and slow drags both work, including releases outside the screen. Touchscreen swipes and
stationary long presses keep the firmware's timing. The page buttons also accept
Up/Down arrows; **H**
or **Home** presses the Home key. The four front buttons are absent on this
profile. Open <http://127.0.0.1:8099/?device=x4pro> to select it directly.

Input passes through the firmware's normal mapping, including remapping and
orientation settings. Hold an on-screen button or key to exercise long presses.

Open the reader menu with **Confirm** to change reading orientation or Night
Mode on X4/X3, or use the center tap and **More** panel on X4 Pro.
Screenshots save the actual rendered canvas as PNG. Diagnostic logs and
build information can be downloaded or copied when reporting a problem.

Choose **Browse SD card** to manage the running reader's files. The manager opens
at `/books`; use the breadcrumbs to browse other directories. Upload any files,
upload a folder with its directory structure, or drag files and folders into the
manager. You can create folders, rename or delete items, download individual
files, and show hidden files. Deleting a folder removes its contents too.
Replacing a file and deleting an item require confirmation.

Uploads copy files immediately and preserve the current book, page, and settings.
Reopen **Browse Files** to refresh its list. Use **Restart firmware** to reload
fonts and other data read at startup; it preserves the virtual SD card and saved
settings. The browser preview does not modify core firmware code.
For SD fonts, create `/fonts` and upload a family folder such as `RIDIBatang`
containing its `.cpfont` files. Restart firmware, then choose the family in the
reader's font settings. Network font downloads from **Manage Fonts** remain unavailable in the offline preview.

Close a book before replacing, renaming, or deleting it. Select another font
before changing the active font family. Other open files are protected, and
`/.crosspoint` firmware settings and caches are read-only in the manager; they
can still be browsed and downloaded. Changing a closed book clears its old
path-based reading cache so replacement content is parsed again.

Uploads are limited to **64 MiB of total SD card contents**, including generated
caches, with at most 8,192 entries and 32 directory levels. The usage indicator
includes firmware-generated files; those files can grow as books are read.
Uploads run one file at a time. If a batch fails, completed files remain on the
card and the error identifies the item to check. These limits bound browser
storage, not the physical device's RAM.

Files stay in this tab's memory and are never sent to a server or saved in browser
storage. **Restart firmware** keeps the card; **Clear SD card** asks for
confirmation, restores the sample card, and restarts the reader. Reloading the page also discards files and settings.
Switching device profiles on the hosted page carries the virtual card, including
settings and reading progress, into a new firmware instance. The old instance's
task timer is stopped when it is replaced.

The generated sample EPUB contains original fixture text. Every build starts
without pre-rendered book or section caches, so opening it exercises EPUB
parsing, pagination, and cache generation in the selected firmware.

## What this verifies

Use the preview for menus, translations, book parsing, text layout, orientation,
and reader interactions. It uses the simulator's image decoder adapters, so
JPEG/PNG output can differ from the device decoders.

It does not verify ESP32 memory usage, task scheduling, SD-card timing, e-ink
waveforms, ghosting, battery behavior, or deep-sleep recovery. Network services,
file-transfer servers, protected-book crypto, and firmware updates are outside
this PoC. Unsupported download/server operations report failure instead of
pretending to complete. The seeded settings disable automatic sleep; use Reset
if a power or sleep setting stops the preview.

The displayed serial heap statistics come from the simulator and are synthetic.
Browser memory starts at 64 MiB and may grow to 256 MiB. The browser task adapter
uses Emscripten Asyncify fibers on a single thread. Task notifications, sleeps,
and contended semaphores suspend the current fiber so another task can run;
the scheduler returns to JavaScript between batches. There are no pthreads or
Web Workers. Cooperative scheduling does not reproduce preemption or thread
races, and a long computation without a yield can briefly delay browser input.

Four static task slots each reserve a 1 MiB C stack and a 256 KiB Asyncify stack;
the scheduler has another 256 KiB Asyncify stack. Together these reserve
5.25 MiB inside browser memory. A static pool holds up to 32 semaphores.
Task creation and switching do not allocate heap memory. Pool exhaustion logs
an error and returns failure. The firmware currently uses two task slots. The SDL bridge reserves three ARGB frames (staging, published,
and a stable JavaScript snapshot) as browser-only static storage. The snapshot
lets JavaScript read a complete frame between firmware task runs. None of these buffers or runtime allocations enter a device
build; the ESP32-C3 still has its usual RAM limit and single framebuffer.
Touch and key events share a fixed 64-entry queue, with no per-event heap
allocation. Coordinates are scaled to the current logical screen; the simulator
handles orientation. Cancellation suppresses the active touch before releasing
it, preventing a lost pointer or focus change from selecting a menu item.
On mouse release, movement of at least 12 CSS pixels becomes a brief touch swipe.
Short flicks are extended past the simulator's 60 display-pixel threshold;
the segment is moved inside the display when a release goes past an edge.
The entire contact replacement is published to the input queue at once, so the
firmware cannot interpret a partially submitted swipe as a tap. Its four events
use a 64-byte stack array and the existing queue; no heap allocation is needed.
Touchscreen contacts retain their original timing and distance;
canceled mouse drags never become swipes.
Chrome can report capture loss on a final mouse move with no buttons pressed,
before reporting the button release. That event completes the gesture once;
the later release is ignored. Capture loss while held and explicit cancellation
still cancel the contact.

## Verify changes

With the preview server running, install Playwright and run the smoke test:

```sh
python3 -m pip install playwright==1.63.0
python3 -m playwright install chromium
python3 tools/wasm/smoke_test.py
```

Pass `--device x4`, `--device x3`, or `--device x4pro` when only one target is built. The test opens
the real sample EPUB, turns pages using pointer and keyboard input, selects all
four orientations through the reader menu, changes Night Mode, saves a PNG,
uploads files without resetting reading state, and clears the card. It also checks for JavaScript errors, unexpected
external requests, and worker creation. Tests require a server without isolation
headers and confirm that SharedArrayBuffer is unavailable, including after reset.
The SD card checks cover nested Unicode folders, hidden and empty files, binary
download integrity, overwrite/rename/delete, open-book protection, path traversal,
upload size limits, and narrow-screen layout. The X4 checks also cover cache
invalidation, directory drops, cancelling an upload during reset, and preserving
the card across device switches on the hosted site. Restart checks verify that
uploaded files and saved settings survive on all three profiles. Screenshots and logs go
to `build/preview-test/`. X4 Pro additionally exercises direct touch and mouse
navigation, swipes in all four orientations, single-move mouse flicks, short and
diagonal drags, slow drags, drags released past the screen edge, short and held
Home presses, and canceled contacts
(pointer cancellation, lost capture, focus changes, and releases outside the screen).
To verify the standalone files directly from disk:

```sh
python3 tools/wasm/smoke_test.py --standalone build/preview --output build/preview-test/standalone
```

This copies only the HTML files into a temporary folder whose name contains
spaces and Korean characters, disables network access, and runs the same
interaction checks. It rejects requests for neighboring files or network
resources. Use `--browser-channel chrome` to test an installed Chrome instead
of Playwright's Chromium.

An additional test delays the browser during input to check that a mouse swipe
cannot accidentally open the reader toolbar. It also retains the old runtime
after reset and verifies that its scheduler stops.
The mouse tests also reproduce Chrome's capture loss before release by sending
a final move with `buttons=0`, and check that it turns exactly one page.

With Emscripten still activated, run the scheduler checks after a preview build:

```sh
python3 tools/wasm/test_scheduler.py --device x4
```

These execute the same WASM scheduler under Node and cover notifications,
timeouts, mutex contention, recursive locking, binary semaphores, task deletion,
pool reuse and exhaustion, and stopping the scheduler.

Compare the same book, font, and settings on a device
when a change depends on rendering fidelity; use serial heap and stack
measurements on hardware for resource-sensitive changes.

## PR builds and hosting

`.github/workflows/browser-preview.yml` builds and tests all three devices for each
PR. It checks out the PR **head SHA**, not GitHub's generated merge commit, and
puts the source SHA in each `build.json`. Local builds mark uncommitted changes.
The workflow uploads each standalone HTML file as an unzipped artifact, plus a
complete static-site ZIP, with a 14-day retention period. It has read-only
repository permissions and no deployment credentials.

The existing firmware PR comment includes a **Browser previews** section with
downloads for X4, X3, and X4 Pro. Sign in to GitHub, download the HTML for your
device, and open it directly in Chrome. No extraction or server is needed. The
static-site ZIP also contains the three HTML files and instructions in
`README.txt`. CI tests both plain HTTP hosting and direct file opening with
network access disabled.

`.github/workflows/pr-firmware-links.yml` refreshes the same comment when either
firmware CI or Browser preview completes. It resolves both workflows for the
PR's current head commit, so completion order does not remove the other build's
links. Each refresh reports pending, failed, missing, and expired preview builds
instead of linking an older build. This trusted workflow reads GitHub metadata
without checking out or executing PR code. GitHub requires the updated
`workflow_run` listener to be present on the repository's default branch before
it can respond to preview builds.

**Public hosting is not configured.** To host the multi-device website, publish
the complete static-site artifact folder to a static HTTP or HTTPS host and
serve `.wasm` files as `application/wasm`.

Use a dedicated preview origin without the main site's cookies or stored data.
Publish immutable commit-specific URLs, record both source and base SHAs if
adding comparisons, and display failures without labeling an older artifact as
the latest build. A separate trusted deployment workflow can publish verified
static artifacts; it must not execute scripts from the
PR or build artifact with deployment credentials. Keep runtime JS, WASM, and
sample data together. Do not apply immutable caching to a mutable URL.

## Standalone packaging

`package_standalone.py` embeds the compiled JS, WASM, virtual SD image, and
metadata. Native frontend module imports use embedded data URLs. Emscripten
receives the embedded bytes through `wasmBinary` and `getPreloadedPackage`,
so the standalone artifact uses the same WASM as the hosted preview. Each reset
creates a fresh `srcdoc` iframe using the embedded assets. File-origin messages
use the browser's opaque origin and still validate the exact peer window.

To repackage existing binaries after frontend edits without recompiling:

```sh
python3 tools/wasm/package_standalone.py
```

The HTML stores binary assets as base64, which increases their uncompressed
size. Decoding them also uses browser memory during startup; none of this
packaging code or storage is part of the device firmware.

## Compatibility and credits

The simulator is pinned in `platformio.preview.ini`. `stubs/BoardConfig.h` and
`stubs/BatteryMonitor.h` cover APIs added to this firmware after that revision.
Remove those bridges when the pinned simulator gains the same interfaces.
The browser entry point replaces `simulator_main.cpp`; the offline adapters
replace `HttpDownloader.cpp` and `CrossPointWebServer.cpp`. The build creates an
ignored simulator overlay in `.cache/wasm/<device>/simulator`,
replacing the FreeRTOS headers with `runtime/freertos/` and routing Arduino
`delay()` / `yield()` through the cooperative scheduler. The overlay is needed
because simulator headers also use relative quoted includes; it leaves the
pinned dependency untouched. Other application
sources continue to come from PlatformIO's dependency graph.

The SDL bridge and standalone MD5 implementation are adapted from
[Mario Ruiz's CrossPlay browser build](https://github.com/ma-r-s/crossplay/tree/caf60082f77c2949eea0a1cdbd3c864e958a6c6c/tools_local/wasm).
Its MIT notice is preserved in `LICENSE.crossplay` and copied into the preview.
The hardware adapters come from
[CrossPoint Simulator](https://github.com/crosspoint-reader/crosspoint-simulator/tree/097f44e08492d9dde40d45c1bce189fd1e782e0e).
