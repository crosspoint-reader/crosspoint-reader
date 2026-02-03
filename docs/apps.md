# Apps (Extensions) and Developer Workflow

CrossPoint supports **apps/extensions** that live on the SD card and are installed by flashing their firmware image into the device's unused OTA partition.

This is intentionally designed so out-of-scope apps (games, puzzles, etc.) do **not** need to be included in the core reader firmware.

## What is an "app"?

An app is a **standalone firmware binary** (ESP32-C3) with a small manifest.

SD card layout:

```
/.crosspoint/apps/<appId>/
  app.bin   # app firmware image (ESP32-C3; starts with magic byte 0xE9)
  app.json  # manifest (name, version, ...)
```

The CrossPoint launcher discovers apps by scanning `/.crosspoint/apps/*/app.json`.

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
