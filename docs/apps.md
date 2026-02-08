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

## Packaging (recommended)

For uploads over WiFi, the recommended format is a ZIP containing:

```
app.bin
app.json
```

Where `app.json` must include at least:
- `id` (folder-safe app id)
- `name`
- `version`

## How it boots (high level)

```text
SD app.bin -> CrossPoint boots it (flash only if needed) -> reboot into extension
```

Note: CrossPoint OTA updates may overwrite the currently-installed extension slot (two-slot OTA). The extension remains on SD and can be reinstalled.

## Installing an app (on device)

1. Home → Apps
2. Select the app
3. Press Install

CrossPoint will:
- If this exact `app.bin` is already installed: switch boot partition and reboot (no flash)
- Otherwise: flash `app.bin` to the other OTA slot, then reboot

Notes:
- Installing requires battery >= 20%.
- CrossPoint tracks the last installed app in `/.crosspoint/apps/.installed.json` (by appId + SHA256 of `app.bin`).

## Fast iteration: upload apps over WiFi (no SD card removal)

Use the File Transfer feature:

1. On device: Home → File Transfer
2. Connect to WiFi (STA) or create a hotspot (AP)
3. From your computer/phone browser, open the URL shown on the device
4. Open **Apps**
5. Upload either:
   - a ZIP containing `app.bin` + `app.json` (recommended, zero fields), or
   - `app.bin` (requires App ID; other metadata optional)
7. On device: Home → Apps → select app → Install

Notes:
- This page is upload-only. Installing always happens on device.
- For ZIP uploads, `app.json` is taken from the ZIP.
- For BIN uploads, the server will generate `app.json` (Name defaults to App ID; Version defaults to 0.0.0).
- You can disable the entire Apps feature via Settings → System → Enable Apps.

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
- Rename/copy your output to `app.bin`, then upload via the Apps page.

## Example: Hello World app

This repo includes a minimal Hello World app that can be built as a standalone firmware image and installed via the Apps menu.

Build:

```bash
.venv/bin/pio run -e hello-world
```

Upload the output:

- File: `.pio/build/hello-world/firmware.bin`
- Upload via: File Transfer → Apps
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
