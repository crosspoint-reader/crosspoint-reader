## Project Architecture

### Build System: PlatformIO

**PlatformIO is BOTH a VS Code extension AND a CLI tool**:

1. **VS Code Extension** (Recommended):

   * Use the [pioarduino IDE](https://github.com/pioarduino/pioarduino-vscode-ide)
     and the pinned core described in [getting started](../../docs/contributing/getting-started.md).

   * Provides: Toolbar buttons, IntelliSense, integrated build/upload/monitor

   * Configuration: `.vscode/c_cpp_properties.json`, `.vscode/tasks.json`

   * Usage: Click Build (✓), Upload (→), or Monitor (🔌) buttons

2. **CLI Tool** (`pio` command):

   * **First-time setup**: Use the pinned pioarduino Core from
     [getting started](../../docs/contributing/getting-started.md#first-time-toolchain-setup),
     matching CI. For routine builds, use the
     [existing environment](environment.md#build-in-an-existing-environment).

   * **Location**: The activated Python environment or the IDE's configured core

   * **Verify**: `which pio` (Git Bash) or `where.exe pio` (cmd)

   * **Usage**: `pio run`, `pio run -t upload`, etc.

**Configuration Files**:

* `platformio.ini`: Main build configuration (committed to git)
* `platformio.local.ini`: Local overrides (gitignored, create if needed)
* `partitions.csv`: ESP32 flash partition layout

### Build Environment

* **Standard**: C++20 (`-std=c++2a`). No Exceptions, No RTTI.
* **Logging**: ALWAYS use `LOG_INF`, `LOG_DBG`, or `LOG_ERR` from `Logging.h`. Raw Serial output is deprecated.
* **Environments** (in `platformio.ini`):
  * `default`: Development (LOG_LEVEL=2, serial enabled)
  * `gh_release`: Production (LOG_LEVEL=1)
  * `gh_release_rc`: Release candidate (LOG_LEVEL=1)
  * `slim`: Minimal build (no serial logging)

These are the C3 profiles. Select the matching board environment in
`platformio.ini` for Sticky, X4 Pro, X4 Classic, Paper Mono, or Metalio E-Ink 4;
other SDK boards may need local profiles. Check the environment's inheritance, not just
its name: USB-MSC profiles use the prebuilt Arduino/TinyUSB graph rather than
the `firmware_tuned` core rebuild. Preserve the repository's pinned platform,
dependencies, and patch scripts when changing build configuration.

### Critical Build Flags

These flags in `platformio.ini` fundamentally affect firmware behavior:

```cpp
-DEINK_DISPLAY_SINGLE_BUFFER_MODE=1  // One framebuffer; size depends on the selected panel
-DARDUINO_USB_MODE=1                 // Select native USB Serial/JTAG on applicable boards
-DARDUINO_USB_CDC_ON_BOOT=1          // Serial available immediately at boot
-DXML_CONTEXT_BYTES=1024             // XML parser memory limit (EPUB parsing)
-DUSE_UTF8_LONG_NAMES=1              // SD card long filename support
-DXML_GE=0                           // Disable XML general entities (security)
-DDESTRUCTOR_CLOSES_FILE=1           // FsFile destructor auto-closes (SdFat)
```

This is a subset, not a replacement for the selected environment's flags.
SdFat now uses `USE_SPI_ARRAY_TRANSFER` and `USE_SEPARATE_FAT_CACHE` with
`scripts/patch_sdfat.py`; preserve their dependency pin and patches. SPI array
transfers add send-path stack use, while SDMMC profiles use the block-device
interface instead. For miniz names, check the local defines in
`lib/miniz/src/MinizConfig.h` rather than adding an obsolete global build flag.

**DESTRUCTOR_CLOSES_FILE implications**:

- SdFat's `FsBaseFile` destructor calls `close()` automatically when the object goes out of scope
- **Do NOT add explicit `file.close()` calls** for local `FsFile` variables — the destructor handles it
- Explicit `close()` is still required in these cases:

  1. **Close before delete**: Must close before `Storage.remove()` on the same path

  2. **Close before reopen**: Must close before reopening the same `FsFile` variable (e.g., write then reopen for read, or rewrite the same path)

  3. **Member variables**: `FsFile` members persist beyond any single function scope, so close at the intended release point (e.g., in `onExit()`)

**SINGLE_BUFFER_MODE implications**:

- Only ONE framebuffer exists (not double-buffered)
- The store/restore grayscale path uses temporary chunked buffers through
  `renderer.storeBwBuffer()` and must pair success with `restoreBwBuffer()`
  (or discard when the underlying page changed). Other boards support strip
  grayscale; use the renderer/display capability checks for that path.
- `restoreBwBuffer(false)` is used when the panel still shows newer overlay
  content; restoring the differential baseline then would leave stale pixels.
  Check the contract in [GfxRenderer.h](../../lib/GfxRenderer/GfxRenderer.h).
- `GfxRenderer::FrameBufferLoan` lends the existing buffer during a build
  without freeing it. No drawing/display is allowed during a loan; consumers
  claim scratch storage through `lib/Memory/BuildScratch.h`, release it before
  restoration, and redraw afterwards. Reuse this protocol rather than allocating
  another full-frame buffer.

### Directory Structure

* lib/: Internal libraries (Epub engine, GfxRenderer, UITheme, I18n)
  * lib/hal/: Hardware Abstraction Layer (HalDisplay, HalGPIO, HalStorage)
  * lib/I18n/: Internationalization (translations in `translations/*.yaml`, generated string tables)
* src/activities/: UI logic using the Activity Lifecycle (onEnter, loop, onExit)
* freeink-sdk/: Low-level SDK (EInkDisplay, InputManager, BatteryMonitor, SDCardManager)
* .crosspoint/: SD-based binary cache for EPUB metadata and pre-rendered layout sections

### Hardware Abstraction Layer (HAL)

**CRITICAL**: Always use HAL classes, NOT SDK classes directly.

| HAL Class    | Wraps SDK Class | Purpose               | Singleton Macro |
| ------------ | --------------- | --------------------- | --------------- |
| `HalDisplay` | `EInkDisplay`   | E-ink display control | *(none)*        |
| `HalGPIO`    | `InputManager`  | Button input handling | *(none)*        |
| `HalStorage` | `SDCardManager` | SD card file I/O      | `Storage`       |

The HAL also exposes memory, power, system, time, frontlight, and tilt services
under `lib/hal/`. For example, `HalMemory` separates internal and PSRAM heap
statistics and provides an explicitly PSRAM-only buffer allocator. Extend the
relevant HAL when exposing a new SDK capability to firmware code.

**Location**: [lib/hal/](../../lib/hal/)

**Why HAL?**

- Provides consistent error logging per module
- Abstracts SDK implementation details
- Centralizes resource management

**Example - HalStorage**:

```cpp
#include <HalStorage.h>

// Use Storage singleton (defined via macro)
HalFile file;
if (Storage.openFileForRead("MODULE", "/path/to/file.bin", file)) {
  // Read from file
  // No file.close() needed — DESTRUCTOR_CLOSES_FILE=1 handles it at scope exit
}
```

**Usage**: Use `HalFile` (the mutex-wrapping handle), NOT raw SdFat `FsFile` or Arduino `File`. Do NOT add `file.close()` for local variables (see DESTRUCTOR_CLOSES_FILE above).

**SdFat is not thread-safe; all SD access MUST go through HalStorage**:

- SdFat's `SdSpiCard` tracks SPI bus state with an unsynchronized `m_spiActive` bool. Two tasks calling SdFat concurrently can confuse that state machine and end with one task calling `SPIClass::endTransaction()` against a paramLock the *other* task is holding. That trips FreeRTOS's `xTaskPriorityDisinherit` assert (`tasks.c:5156, pxTCB == pxCurrentTCBs[0]`) and panics the system. See SdFat issue #518.
- `HalStorage` serializes everything via `storageMutex`. Downstream code uses `HalFile` (declared in `<HalStorage.h>`); every method call (read, write, seek, close) takes the mutex. `HalFile`'s destructor also takes the mutex before letting the underlying SdFat `FsFile` close.
- **Never** call into `SdFat` / `SdSpiCard` / `FsBaseFile` / `SDCardManager` / raw `FsFile` directly — that bypasses the mutex.

USB Drive is an exclusive-storage mode: the host owns the raw SD card, so
normal filesystem users and pending navigation must remain stopped. Use the
protocol in `UsbDriveActivity` and `ActivityManager::loop()`; exit by restarting
as that flow requires, rather than re-enabling filesystem access prematurely.

### Service and plugin contracts

Keep service-specific code in SD-card plugins. Firmware supplies generic web
endpoints, a bounded job queue, declarative catalog screens, event delivery,
and content-protection primitives. Browser `plugin.js` runs in the browser;
the device interprets manifests and request templates, not arbitrary JS.

Read the contract that matches the change:

- [docs/sd-plugins.md](../../docs/sd-plugins.md): discovery, manifest/catalog
  behavior, jobs, authentication, download paths, per-book keys, and isolation.
- [docs/plugin-events.md](../../docs/plugin-events.md): whitelisted semantic
  events, deferred outboxes, sleep delivery, reading sessions, and metadata
  sidecars. Preserve documented delivery and compatibility semantics.
- [docs/webserver-endpoints.md](../../docs/webserver-endpoints.md): request
  and response contracts. Normalize user paths and escape displayed filenames
  using the existing helpers; preserve plugin filesystem isolation.
- For protected book reads, inspect `Epub` and the SDK's `ContentProtection`
  sources at the pinned SDK revision before changing decrypt-on-read or key
  handling. Keep service formats and activation policy in plugins.

---
