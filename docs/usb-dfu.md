# Background USB firmware updates for X3, X4 and X4 Pro

On an X3 or X4, software USB is automatically armed when any of `DIS_USB_SERIAL_JTAG`, `DIS_USB_SERIAL_JTAG_DOWNLOAD_MODE`, or `DIS_DOWNLOAD_MODE` is set. `DIS_USB_JTAG` alone does not enable it. Other C3 boards and unlocked X3/X4 readers keep the native USB path. The code only reads eFuses.

While awake, a USB host reset on GPIO18/19 starts a background listener. Connecting a computer or charger leaves the current reader screen open. There is no settings switch. Only a valid `X3DFU001` firmware header requests the full-screen update activity. Book and network activity are stopped at that point; the existing reader exit saves its position. Opening the browser's device picker, enumeration, status queries, and idle ABORT do not open the update screen.

The background listener uses an 8 KiB FreeRTOS task stack, allocated only after host detection and released on disconnect. The PHY stays resident in its locked instruction-cache lines. Enumeration and recent control exchanges get 100 ms listening windows; configured idle operation listens for 8 ms and yields for one tick to reading/rendering. Standard descriptor transfers get the same bounded interrupt protection as firmware downloads. An unconfigured connection is restarted after 15 seconds, even if SOF frames continue. The CPU stays at normal speed and automatic sleep is deferred while the listener is active. Explicit user-requested sleep still stops USB. The 16 KiB receive buffer is allocated only after the reader exits. UI handoff failure or insufficient heap rejects the update without selecting a boot partition. Sleep stops the worker before reconfiguring USB pins. These scheduling changes require on-device validation with real books and host controllers.

During reception the reader shows a static “Receiving firmware” screen and a do-not-power-off message. The screen is painted once before the first firmware data block is accepted. There are no byte/percentage updates, progress callbacks, screen refreshes or display-related USB busy allowances during reception. The browser still reports live transfer progress. The reader redraws for verification and completion; interrupted transfers return to Home. Unplugging while idle preserves the current activity. Throughput still requires a hardware measurement.

USB is available whenever the application is awake. Software cannot service USB while the CPU is in deep sleep: wake the reader if cable insertion does not wake it. USB-powered cold boot is kept awake on affected devices. Device testing must confirm power/wake behavior on the particular X3 hardware revision.

The interface is DFU, not Web Serial or the ESP ROM bootloader. Use the CrossPoint web flasher’s USB update (DFU) connection option. VID/PID `1209:0001`, device version `0x0700`, 4096-byte control-transfer blocks, `X3DFU001` metadata. Compression is disabled. A single 16 KiB receive buffer is allocated only in the transfer activity, released on abort and before final readback verification. TinyUSB itself remains upstream commit `840ffe3ac152394347d27a0d4841f4c3c49ac944`.

The writer erases/writes only the inactive OTA application slot. Boot selection changes only after structural, chip, board, checksum, embedded SHA and whole-image SHA verification of the stored flash. A cancelled/interrupted update keeps the current application bootable. This uses normal alternating CrossPoint OTA slots: the next update may replace a separate Escape Hatch image in the other slot. CrossPoint's existing SD recovery flow remains in this build. No partition-table, bootloader, or eFuse writes are added.

Installing an unmodified upstream firmware later removes this added DFU service. Keep this custom build available for SD recovery, or use a future build containing the same integration.

## SRAM lifetime

The packet tables, response policy, software-controller state and TinyUSB 4096-byte transfer buffer share one zero-initialized, nothrow heap allocation per USB connection. This allocation replaces permanent BSS storage: it is absent while the USB listener is stopped, including on devices excluded by the eFuse gate. It is internal SRAM on C3, not flash. A large stack object would overflow the worker stack; multiple small allocations would increase fragmentation. Allocation failure returns an error before USB initialization or flash writing. The 8 KiB worker stack and 16 KiB installation buffer retain their existing lifetimes and sizes.

TinyUSB's upstream sources remain unchanged. The local DFU translation-unit wrapper converts its private transfer array into a pointer to an array of the same size; it is bound before initialization and cleared after deinitialization. The software DCD state follows the same lifetime. The packet engine is stopped, TinyUSB is deinitialized and both pointers are cleared before the workspace is freed. Timing-critical IRAM functions and the assembly PHY stay in their existing memory classes. This reduces permanent SRAM use, not the working memory required during an active connection.

## Verification

- Host suites currently require macOS/Xcode Command Line Tools (CommonCrypto SHA-256 test shim).
- `sh test/usb_dfu/run.sh /absolute/path/to/application.bin` exercises the eFuse decision table, actual flash writer and production validator with address/undefined-behavior sanitizers, storage failures, corrupt images, cancel/restart, the raw DFU sink, static reception with no UI work or display delays, and background UI handoff/shutdown. It also exercises the production TinyUSB wrapper with full 4096-byte blocks, invalid buffer sizes/alignment, error recovery and 32 reconnect/abort/free cycles, plus 32 workspace lifetimes through the production software DCD.
- Build with `pio run -e gh_release`; use the repository formatter wrapper.
- Hardware checks still required: automatic enumeration on plug-in/cold boot; full-battery attachment; a charger leaving reading usable; normal reading heap and page-turn responsiveness during enumeration; unplug/reconnect and host sleep; cancelled transfer then retry; full install and reboot; all display orientations. On an unlocked X3, confirm native USB remains available.
- `/usb-connection-result.json` records whether configuration was acknowledged, address, SOF/reset counts, errors, mismatches termination reason and allocated workspace bytes after every listener session (exit 1: missing frames; exit 2: enumeration timeout). It is written once at session exit, not from the hot packet loop. `/usb-update-result.json` records completion, byte counts, errors and erase/write/verify timings after an actual transfer. `ui_ms`, `ui_callbacks` and `ui_max_ms` record stage-callback counts and duration; the initial activity entry screen is excluded. The Receiving callback sets state without repainting. These let device logs expose rendering overhead. No speed claim is made before hardware measurement.

## X4 Pro

X4 Pro builds enable native USB DFU through the existing Arduino TinyUSB stack.
The board exposes device version `0x0800`; C3 uses `0x0700`. X4 Pro accepts
ESP32-S3 application images with a matching `x4pro` board tag. USB OTG must not
be disabled by eFuse. No eFuse is changed. Native USB takes over the shared
USB pins, so hardware USB Serial/JTAG is ended while DFU is active.

The native driver owns two static 4096-byte buffers and a static mutex. USB
callbacks copy requests to a mailbox; the DFU worker performs storage and UI
work. USB Drive stops and drains the worker before exposing the SD card;
DFU writes remain disabled during that activity. Test switching between USB
Drive and DFU, including disconnect and retry, before release.

Build `gh_release` for the shared X3/X4 image and `x4pro-gh_release` for X4 Pro.
`sh test/usb_dfu/run_native.sh /absolute/path/to/x4pro-application.bin` tests
the native mailbox, cancellation, storage exclusion and completion handling.
The USB names are CrossPoint X3 USB Update, CrossPoint X4 USB Update and
CrossPoint X4 Pro DFU. X4 hardware verification remains outstanding.

The current development VID/PID is a test identity. A production allocation
must be agreed with maintainers before distribution; update both transports
and the web flasher together if it changes.
