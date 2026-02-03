# App / Extension Architecture

This document explains how CrossPoint apps (aka extensions) work today, and how we can evolve toward an on-device app store.

## Current Model: Partition-based apps

CrossPoint runs as the main firmware, and apps are installed by flashing a standalone firmware image into the device's *other* OTA partition.

### Components

- **Apps directory** (SD): `/.crosspoint/apps/<appId>/`
  - `app.json`: manifest (name/version/author/minFirmware...)
  - `app.bin`: firmware image for the app (ESP32-C3)
- **Launcher UI** (device): Home → Apps
  - lists apps discovered on SD
  - installs/launches apps by flashing `app.bin`
- **AppLoader**: SD scan + OTA flashing
- **File Transfer Web UI**: developer workflow for uploading apps without removing SD

### End-to-end flow

```text
Developer builds app firmware -> app.bin
          |
          | (WiFi) upload via File Transfer /apps (developer page)
          v
SD card: /.crosspoint/apps/<appId>/{app.bin, app.json}
          |
          | (device UI) Home -> Apps -> select app -> Install
          v
AppLoader flashes app.bin to next OTA partition, sets boot partition, reboots
          |
          v
Device boots into the app firmware
```

### Why this aligns with extension requests

This keeps out-of-scope features (games/puzzles, experimental tools) out of the core reader firmware while still allowing the community to build and run them.

This is directly aligned with:
- Discussion #257: "extension/plugin support" requests
- Discussion #575: chess interest + feedback that games should not live in the core firmware

## Future: On-device app store (proposed)

The current system requires getting `app.bin` onto the SD card.
An on-device app store would automate that step by downloading releases and assets over WiFi.

### Proposed pieces

1) **Registry**

- `apps.json` listing available apps with metadata and download URLs.
- Maintainer choice:
  - separate repo (recommended) `crosspoint-reader/app-registry`
  - or keep `apps.json` in the firmware repo

2) **Release assets**

- Required: `app.bin`
- Optional: `assets.zip` (unpacks to `/.crosspoint/<appId>/...`)
- Optional: `app.json` (manifest) if authors prefer to author it directly

3) **Device-side store UX** (follow-up work)

- Fetch registry over HTTPS
- Download `app.bin` + optional `assets.zip` to SD
- Unpack assets to expected paths
- Surface install/update actions via Home → Apps

### Why assets.zip matters

Many apps need more than a binary (sprites, fonts, puzzle packs).
For production UX, users should not have to manually place these files.
Bundling assets as a separate downloadable archive keeps the app firmware small while enabling a 1-click install experience.
