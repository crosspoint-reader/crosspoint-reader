# Extensions (Apps)

CrossPoint supports **extensions** implemented as standalone firmware images installed from the SD card.

This is intended to keep non-core features (games/tools/experiments) out of the main reader firmware.

## SD layout

An extension is a firmware binary plus a small manifest:

```
/.crosspoint/apps/<appId>/
  app.bin   # ESP32-C3 firmware image (starts with magic byte 0xE9)
  app.json  # manifest (name, version, ...)
```

CrossPoint discovers extensions by scanning `/.crosspoint/apps/*/app.json`.

## How it boots (high level)

```text
SD app.bin -> CrossPoint flashes to the other OTA slot -> reboot into extension
```

Note: CrossPoint OTA updates may overwrite the currently-installed extension slot (two-slot OTA). The extension remains on SD and can be reinstalled.

## Installing an app (on device)

1. Home → Apps
2. Select the app
3. Press Launch/Install

CrossPoint will flash `app.bin` to the OTA partition and reboot.

## Fast iteration: upload apps over WiFi (no SD card removal)

Use the File Transfer feature:

1. On device: Home → File Transfer
2. Connect to WiFi (STA) or create a hotspot (AP)
3. From your computer/phone browser, open the URL shown on the device
4. Open **Apps (Developer)**
5. Fill in:
   - App ID (e.g. `chess-puzzles` or `org.example.myapp`)
   - Name
   - Version
   - Optional: author, description, minFirmware
6. Upload your app binary (`app.bin`)
7. On device: Home → Apps → select app → Install

Notes:
- This page is upload-only. Installing always happens on device.
- The Apps (Developer) page writes to `/.crosspoint/apps/<appId>/` and generates `app.json`.

## Building apps with the community SDK

Recommended SDK: `https://github.com/open-x4-epaper/community-sdk`

Typical setup (in your app repo):

1. Add the SDK as a submodule:
   ```bash
   git submodule add https://github.com/open-x4-epaper/community-sdk.git open-x4-sdk
   ```
2. In `platformio.ini`, add SDK libs as `lib_deps` (symlink form), for example:
   ```ini
   lib_deps =
     BatteryMonitor=symlink://open-x4-sdk/libs/hardware/BatteryMonitor
     EInkDisplay=symlink://open-x4-sdk/libs/display/EInkDisplay
     SDCardManager=symlink://open-x4-sdk/libs/hardware/SDCardManager
     InputManager=symlink://open-x4-sdk/libs/hardware/InputManager
   ```
3. Build with PlatformIO:
   ```bash
   pio run
   ```
4. The firmware binary will usually be:
   - `.pio/build/<env>/firmware.bin`

For CrossPoint app uploads:
- Rename/copy your output to `app.bin`, then upload via the Apps (Developer) page.

## Example: Hello World app

This repo includes a minimal Hello World app that can be built as a standalone firmware image and installed via the Apps menu.

Build:

```bash
.venv/bin/pio run -e hello-world
```

Upload the output:

- File: `.pio/build/hello-world/firmware.bin`
- Upload via: File Transfer → Apps (Developer)
- Suggested App ID: `hello-world`

Then install on device:

Home → Apps → Hello World → Install

## Distribution (proposed)

Apps should live in their own repositories and publish binaries via GitHub Releases.

For safety/auditability, registry listings should reference a public source repository (e.g. GitHub URL) so maintainers and users can review the code that produced the release.

Release assets:
- Required: `app.bin`
- Optional: `app.json`

Registry location (maintainer choice):
1. Separate repo (recommended): `crosspoint-reader/app-registry` containing `apps.json`
2. Or keep `apps.json` in the main firmware repo

The on-device store UI can be built later on top of this ecosystem.
