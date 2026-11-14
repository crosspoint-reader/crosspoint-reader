# TinyUSB device build

The `usb_msc` PlatformIO profile keeps the prebuilt ESP32-S3 SDK, including
its PSRAM configuration, and replaces only `libarduino_tinyusb.a` with a
project-local build. X4 Pro, X4 Classic, and Paper Mono inherit this profile.
C3 and Sticky use their existing SDK builds.

The SDK's default USB driver tables retain host drivers and unused device
classes even when USB Drive is inactive. With Arduino 3.3.11, the original
TinyUSB component occupies 30,927 bytes of static DRAM. The device build
occupies 5,735 bytes: 25,192 fewer bytes of permanent internal-RAM storage.
These are linker-map measurements, not on-device free-heap measurements.

`scripts/tinyusb/crosspoint_tusb_config.h` includes the SDK configuration
and disables USB host support and unused device classes. MSC, CDC, endpoint
sizes, alignment, and FreeRTOS settings come from the SDK. The same header
is selected through `CFG_TUSB_CONFIG_FILE` when compiling the Arduino core
and the replacement TinyUSB archive. Normal serial uses the separate USB
Serial/JTAG peripheral.

PlatformIO installs TinyUSB at the full commit pinned in `platformio.ini`.
`lib_ignore` prevents automatic library compilation; `scripts/build_tinyusb.py`
builds only the required sources and replaces every packaged TinyUSB link
entry. Objects and the archive live under `.pio/build/<environment>/`.
SCons tracks their source, header, and compiler-command dependencies using
the existing build cache. A pre-build hook adds the overlay's content hash
to compiler flags because SCons does not follow macro-based includes; editing
the overlay invalidates both the core and component objects. These hooks do
not modify shared SDK files.

The hook checks the SDK's `versions.txt` and stops on an unsupported SDK
revision. When upgrading Arduino, inspect the new SDK's TinyUSB and
lib-builder revisions, update the source pin and guard together, and compare
the upstream configuration and build recipe. Do not bypass the guard or
link an older TinyUSB archive against newer headers.

Every affected firmware link checks for MSC/CDC symbols and rejects unused
USB class and host symbols. Host tests run in CI:

```sh
python3 -m unittest discover -s test/tinyusb -v
pio run -e x4pro-gh_release
pio run -e papermono -e x4c -e default
```

Before release, test on an S3 device:

1. Record total/free internal heap and PSRAM after a fresh boot at Home.
2. Enter USB Drive; confirm enumeration and read/write a file, checking its hash.
3. Eject, exit USB Drive, and confirm the reader reboots into normal serial mode.
4. Confirm SD access and serial screenshots still work after reconnecting.

The fix does not reduce IPC task stacks or alter the SDK's IRAM placement.
