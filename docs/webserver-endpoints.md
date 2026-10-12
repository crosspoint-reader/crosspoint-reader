# Webserver Endpoints

This document describes all HTTP, WebDAV and WebSocket endpoints available on the CrossPoint Reader webserver.

- [Webserver Endpoints](#webserver-endpoints)
  - [Overview](#overview)
  - [Pages](#pages)
  - [Status](#status)
    - [GET `/api/status` - Device Status](#get-apistatus---device-status)
    - [GET `/api/status/fast` - Device Status without SD stats](#get-apistatusfast---device-status-without-sd-stats)
    - [GET `/api/diagnostics/boot` - Boot and Sleep History](#get-apidiagnosticsboot---boot-and-sleep-history)
  - [Files](#files)
    - [GET `/api/files` - List Files](#get-apifiles---list-files)
    - [GET `/download` - Download File](#get-download---download-file)
    - [POST `/upload` - Upload File](#post-upload---upload-file)
    - [POST `/mkdir` - Create Folder](#post-mkdir---create-folder)
    - [POST `/rename` - Rename File or Folder](#post-rename---rename-file-or-folder)
    - [POST `/move` - Move File or Folder](#post-move---move-file-or-folder)
    - [POST `/delete` - Delete Files or Folders](#post-delete---delete-files-or-folders)
  - [Settings](#settings)
    - [GET `/api/settings` - List Settings](#get-apisettings---list-settings)
    - [POST `/api/settings` - Change Settings](#post-apisettings---change-settings)
  - [Reading Stats](#reading-stats)
    - [GET `/api/stats` - Dashboard Data](#get-apistats---dashboard-data)
    - [GET `/api/stats/export` - Download a Backup](#get-apistatsexport---download-a-backup)
    - [POST `/api/stats/remove` - Remove a Book from the Stats](#post-apistatsremove---remove-a-book-from-the-stats)
  - [Fonts](#fonts)
  - [Saved Networks and Servers](#saved-networks-and-servers)
    - [Wi-Fi networks](#wi-fi-networks)
    - [OPDS servers](#opds-servers)
  - [Plugin Endpoints](#plugin-endpoints)
  - [WebDAV](#webdav)
  - [WebSocket Endpoint](#websocket-endpoint)
    - [Port 81 - Fast Binary Upload](#port-81---fast-binary-upload)
  - [Network Modes](#network-modes)
    - [Station Mode (STA)](#station-mode-sta)
    - [Access Point Mode (AP)](#access-point-mode-ap)
  - [Notes](#notes)


## Overview

The CrossPoint Reader exposes a webserver for file management and device monitoring:

- **HTTP Server**: Port 80 (the routes below, plus [WebDAV](#webdav) on any other path)
- **WebSocket Server**: Port 81 (for fast binary uploads)

The routes are registered in `CrossPointWebServer::begin()`.

When the largest free heap block is too small, the JSON endpoints answer
`503` with `{"error":"low memory"}` and a `Retry-After: 5` header. Retry after a moment.

---

## Pages

Each page is one HTML file from `src/network/html/`, gzip-encoded
(`Content-Encoding: gzip`). The pages call the API below.

Only the setup page is in the firmware. The others are a bundle on the SD card,
in `/.crosspoint/web/`, which the setup page installs (see
[Web Interface Bundle](#web-interface-bundle)). Until it is installed, `/` serves
the setup page and every other page redirects (`302`) to `/setup`.

| Path          | Page                                       |
| ------------- | ------------------------------------------ |
| `/`           | Welcome page, with links to the others; the setup page while the bundle is not installed |
| `/setup`      | Setup page: installs the web interface, and uploads books without it |
| `/files`      | File Manager                               |
| `/settings`   | Settings                                   |
| `/stats`      | Reading Stats                              |
| `/fonts`      | Font Manager                               |
| `/dictionary` | Dictionaries: lists, installs and removes StarDict dictionaries |
| `/plugins`    | Plugins: install, switch off and remove web-interface plugins |
| `/systeminfo` | System Info                                |
| `/diagnostics`| Diagnostics: device status, boot and sleep history, and a report to download |

The Dictionaries page needs no endpoint of its own. It unpacks archives and
compressed files in the browser, then works through
[`/api/files`](#get-apifiles---list-files), [`/download`](#get-download---download-file)
(to read each `.ifo`), [`/mkdir`](#post-mkdir---create-folder),
[`/upload`](#post-upload---upload-file) and [`/delete`](#post-delete---delete-files-or-folders),
one request at a time. The route is singular so it is never mistaken for the
`/dictionaries` folder. See [docs/dictionary.md](dictionary.md#install-from-the-web-interface).

`GET /js/<name>` serves a script from the bundle by its name, gzip-encoded
(`Content-Encoding: gzip`), or `404` when the bundle is not installed or has no such
script: `/js/jszip.min.js`, the JSZip library the File Manager and the Dictionaries and
Plugins pages load, and `/js/metadata-editor.js`, the File Manager's Details dialog,
loaded on first use. Pages are not served under `/js/`. Any
other path that WebDAV does not claim answers `404` with a plain-text body.

Pages and scripts from the bundle carry `Cache-Control: no-cache` and the bundle's name as
their `ETag`. A browser asks again before each use, sending `If-None-Match`, and gets a bodiless
`304` while the same bundle is installed; after another one is installed, it gets the new file
straight away.

```bash
curl http://crosspoint.local/
```

---

## Web Interface Bundle

The pages other than `/setup` are built into one file, `web-<hash>.bin`, which
is attached to each GitHub release. The firmware knows the exact hash of its own
bundle and installs only that file, from wherever it comes.

### GET `/api/web/status` - Bundle State

```bash
curl http://crosspoint.local/api/web/status
```

```json
{"state":"installed","release":"2.38","repo":"jpirnay/witchhunt-reader","asset":"web-996781e9ef9c.bin","online":true,"onCard":false}
```

| Field     | Type    | Description |
| --------- | ------- | ----------- |
| `state`   | string  | `installed`; `missing` (nothing installed, or a file is gone); `stale` (a bundle from another firmware build) |
| `release` | string  | Release this firmware is published under, empty for development builds |
| `repo`    | string  | GitHub repository of that release |
| `asset`   | string  | File name of this firmware's bundle |
| `online`  | boolean | `false` in hotspot mode, where the device cannot download |
| `onCard`  | boolean | A file named `asset` is in the SD card's top folder |
| `rssi`    | number  | Wi-Fi signal in dBm; absent in hotspot mode |
| `lastUpload` | object | Only after a `/upload` broke off, until the next one starts: `name`, `received` (file bytes), `expected` (request bytes), `ms`, `idleMs` (since data last arrived), `reason` (`stalled`: no data for 10 s; `closed`; `cancelled` on the device), and `rssi` at that moment |

The device drops an upload that breaks off without answering the request, so the browser sees only a
broken connection; the setup page reads `lastUpload` to say what happened.

### POST `/api/web/install` - Install the Bundle

| Parameter | Description |
| --------- | ----------- |
| `source`  | `release` downloads `asset` from `release`; `card` installs `/<asset>` from the SD card and deletes it |

```bash
# Download and install from the release
curl -X POST "http://crosspoint.local/api/web/install?source=release"

# Upload the file to the card's top folder, then install it
curl -F "file=@web-996781e9ef9c.bin" "http://crosspoint.local/upload?path=/"
curl -X POST "http://crosspoint.local/api/web/install?source=card"
```

Answers `{"ok":true}`, or `500` with `{"ok":false,"error":"..."}` when the
download fails or the file is not this firmware's bundle. `source=release` in
hotspot mode answers `409`.

---

## Status

### GET `/api/status` - Device Status

Returns JSON with device status information. The plain endpoint never touches
the SD card and responds immediately; pass `phase=full` to additionally
collect SD usage stats (`sdTotal`/`sdUsed`/`sdFree`), which scans the FAT and
can take tens of seconds on large cards.

**Request:**
```bash
curl http://crosspoint.local/api/status
# Include SD usage stats (slow):
curl "http://crosspoint.local/api/status?phase=full"
```

**Query Parameters:**

| Parameter | Required | Default | Description                                        |
| --------- | -------- | ------- | -------------------------------------------------- |
| `phase`   | No       | (fast)  | `full` = also collect SD usage stats (slow)        |

**Response (200 OK):**
```json
{
  "version": "1.0.0",
  "deviceType": "X4",
  "device": "X4",
  "displayWidth": 800,
  "displayHeight": 480,
  "chipVersion": "ESP32-C3 rev 4",
  "cpuMHz": 160,
  "ip": "192.168.1.100",
  "mode": "STA",
  "rssi": -45,
  "macAddress": "AA:BB:CC:DD:EE:FF",
  "freeHeap": 123456,
  "minFreeHeap": 98304,
  "maxAllocHeap": 61440,
  "flashTotal": 16777216,
  "appPartitionSize": 6553600,
  "batteryPercent": 87,
  "charging": false,
  "uptime": 3600,
  "sdReady": false,
  "sdTotal": 0,
  "sdUsed": 0,
  "sdFree": 0
}
```

| Field              | Type   | Description                                                                |
| ------------------ | ------ | -------------------------------------------------------------------------- |
| `version`          | string | CrossPoint firmware version                                                |
| `deviceType`       | string | Board name: `"X3"`, `"X4"`, `"X4 Pro"` or `"T5 S3 Pro"`; other boards report their own name |
| `device`           | string | Same value as `deviceType`, kept for the Calibre plugin's model detection  |
| `displayWidth`, `displayHeight` | number | Panel size in pixels                                  |
| `chipVersion`      | string | SoC model and revision                                                     |
| `cpuMHz`           | number | CPU clock in MHz                                                           |
| `ip`               | string | Device IP address                                                          |
| `mode`             | string | `"STA"` (connected to WiFi) or `"AP"` (access point mode)                  |
| `rssi`             | number | WiFi signal strength in dBm (0 in AP mode)                                 |
| `macAddress`       | string | WiFi MAC address                                                           |
| `freeHeap`         | number | Free heap memory in bytes                                                  |
| `minFreeHeap`      | number | Lowest free heap since boot                                                |
| `maxAllocHeap`     | number | Largest single block that can be allocated now                             |
| `flashTotal`       | number | Flash size in bytes                                                        |
| `appPartitionSize` | number | Size of the running app partition in bytes                                 |
| `batteryPercent`   | number | Battery charge in percent                                                  |
| `charging`         | bool   | `true` while charging                                                      |
| `uptime`           | number | Seconds since device boot                                                  |
| `sdReady`          | bool   | `true` only with `phase=full`; the `sd*` fields are valid only then        |
| `sdTotal`, `sdUsed`, `sdFree` | number | SD card bytes; ignore unless `sdReady` is `true`                |

### GET `/api/status/fast` - Device Status without SD stats

The same JSON as `/api/status`, with `sdReady` always `false`. It takes no
parameters and never scans the card. The System Info page uses it for its first paint.

```bash
curl http://crosspoint.local/api/status/fast
```

### GET `/api/diagnostics/boot` - Boot and Sleep History

What the device's **Settings → System → Boot Diagnostics** screen shows: the
hardware variant, how this boot started, and the last 16 sleep and boot events
kept on the card (`/.crosspoint/bootdiag.bin`). Values are the same short English
tokens the screen prints, untranslated, so reports from different devices compare.
The Diagnostics page reads it and saves it, with `/api/status`, as a bug report.
Streamed; no parameters.

```bash
curl http://crosspoint.local/api/diagnostics/boot
```

**Response (200 OK)**, abridged:
```json
{
  "hardware": {"board": "xteink_x4", "displayController": "SSD1677", "promotedByProbe": false,
               "panelProbe": "ver 00 00 00 00 00 flg 00 v1"},
  "thisBoot": {
    "resetReason": "deep-sleep", "wakeCause": "gpio", "previousSessionSlept": true,
    "wakeGate": {"verdict": "long-hold", "heldMs": 310, "decidedAtMs": 402, "requiredMs": 300},
    "firstPaintMs": 1830,
    "phases": [{"name": "entry", "ms": 80}, {"name": "gate", "ms": 402}, {"name": "sd", "ms": 650}],
    "abortedBoots": {"thisRun": 0, "lifetime": 0, "undrained": 0},
    "lightSleep": {"sleptMs": 120000, "awakeMs": 40000, "attempts": 55, "gpioWakes": 2}
  },
  "history": [
    {"seq": 41, "kind": "boot", "resetReason": "deep-sleep", "wakeCause": "gpio", "gate": "long-hold",
     "gateDecidedMs": 402, "gateHeldMs": 310, "sdMountMs": 650},
    {"seq": 40, "kind": "sleep", "outcome": "slept", "stage": "wake-armed", "trigger": "power-hold",
     "latchKept": false, "fromReader": true, "releaseWaitMs": 120, "releaseTimedOut": false, "uptimeS": 1200},
    {"seq": 39, "kind": "aborted", "trigger": "gate-rejected", "gate": "released-early", "count": 3, "capped": false}
  ]
}
```

| Field | Meaning |
| ----- | ------- |
| `hardware.panelProbe` | The display-bus probe's raw bytes, or `null` when no probe ran |
| `thisBoot.previousSessionSlept` | `false` when this boot directly followed another boot, so the session between them never reached the sleep path (a reset or a crash); `null` while the history holds fewer than two records |
| `thisBoot.phases` | The `millis()` stamp of each boot phase this boot reached (`BootDiag::BootPhase`); the page shows the differences |
| `thisBoot.abortedBoots` | Wakes refused since the last completed boot (`thisRun`, from the card), ever (`lifetime`, NVS), and counted but not yet written to the card (`undrained`) |
| `history` | Newest first. `kind` is `sleep`, `boot` or `aborted` (a run of wakes that went straight back to sleep, `count` times; `capped` once the count stopped at its limit) |
| `outcome` | `slept`, `release-timeout`, `powered-off` (the rail was cut, as the sleep asked), `powered-off-unasked`, `did-not-sleep` or `unfinished` (no boot after it yet) |

Token lists: `stage` and `trigger` are `BootDiag::stageName()` / `triggerName()`,
`resetReason` and `wakeCause` are `BootDiag::resetReasonName()` / `wakeCauseName()`,
`gate` and `wakeGate.verdict` are `HalGPIO::wakeVerdictName()`. Nothing secret is
included.

---

## Files

### GET `/api/files` - List Files

Returns a JSON array of files and folders in the specified directory. The array
is streamed in chunks.

**Request:**
```bash
# List root directory
curl http://crosspoint.local/api/files

# List specific directory
curl "http://crosspoint.local/api/files?path=/Books"
```

**Query Parameters:**

| Parameter | Required | Default | Description            |
| --------- | -------- | ------- | ---------------------- |
| `path`    | No       | `/`     | Directory path to list |

**Response (200 OK):**
```json
[
  {"name": "MyBook.epub", "size": 1234567, "isDirectory": false, "isEpub": true},
  {"name": "Notes", "size": 0, "isDirectory": true, "isEpub": false},
  {"name": "document.pdf", "size": 54321, "isDirectory": false, "isEpub": false}
]
```

| Field         | Type    | Description                              |
| ------------- | ------- | ---------------------------------------- |
| `name`        | string  | File or folder name                      |
| `size`        | number  | Size in bytes (0 for directories)        |
| `isDirectory` | boolean | `true` if the item is a folder           |
| `isEpub`      | boolean | `true` if the file has `.epub` extension |

**Notes:**
- Hidden files (starting with `.`) are filtered out unless **Show Hidden Files** is on
- System folders (`System Volume Information`, `XTCache`) are always hidden
- Every path parameter on this page is normalised first, so `..` cannot step outside the folder it is joined to
- Listing a protected folder answers `403 Cannot access protected items`; see [Protected paths](#protected-paths)

---

### GET `/download` - Download File

Streams one file to the client as an attachment.

**Request:**
```bash
curl -OJ "http://crosspoint.local/download?path=/Books/mybook.epub"
```

**Query Parameters:**

| Parameter | Required | Description                 |
| --------- | -------- | --------------------------- |
| `path`    | Yes      | Path of the file to fetch   |

**Response (200 OK):** the file bytes, with `Content-Disposition: attachment`.
`Content-Type` is `application/epub+zip` for `.epub` files, else `application/octet-stream`.

**Error Responses:**

| Status | Body                            | Cause                          |
| ------ | ------------------------------- | ------------------------------ |
| 400    | `Missing path`                  | `path` not provided            |
| 400    | `Invalid path`                  | Empty path or `/`              |
| 400    | `Path is a directory`           | Only files can be downloaded   |
| 403    | `Cannot access protected items` | A [protected path](#protected-paths) |
| 404    | `Item not found`                | Path does not exist            |
| 500    | `Failed to open file`           | SD card error                  |

---

### POST `/upload` - Upload File

Uploads a file to the SD card via multipart form data.

**Request:**
```bash
# Upload to root directory
curl -X POST -F "file=@mybook.epub" http://crosspoint.local/upload

# Upload to specific directory
curl -X POST -F "file=@mybook.epub" "http://crosspoint.local/upload?path=/Books"
```

**Query Parameters:**

| Parameter | Required | Default | Description                                                  |
| --------- | -------- | ------- | ------------------------------------------------------------ |
| `path`    | No       | `/`     | Target directory for the upload                              |
| `t`       | No       | -       | Client Unix time in seconds; used to set the device clock when it is not synced (AP mode) |

**Response (200 OK):**
```
File uploaded successfully: mybook.epub
```

**Error Responses:**

| Status | Body                                            | Cause                       |
| ------ | ----------------------------------------------- | --------------------------- |
| 400    | `Invalid file name`                             | Name has a separator, or is `.` or `..` |
| 400    | `Cannot write to a protected location`          | Folder or target is a [protected path](#protected-paths) |
| 400    | `Failed to create file on SD card`              | Cannot create file          |
| 400    | `Failed to write to SD card - disk may be full` | Write error during upload   |
| 400    | `Failed to write final data to SD card`         | Error flushing final buffer |
| 400    | `Upload aborted`                                | Client aborted the upload   |
| 400    | `Unknown error during upload`                   | Unspecified error           |

**Notes:**
- Existing files with the same name are replaced
- Uses a 4KB buffer for efficient SD card writes
- The book's layout cache is cleared on success, so an overwritten book is re-indexed
- Pressing **Back** on the device cancels the upload. The server checks for it on every
  received chunk (`CrossPointWebServer::setUploadCancelCheck()`), closes the connection,
  deletes the partial file and leaves File Transfer. The client sees the connection drop,
  with no response.

---

### POST `/mkdir` - Create Folder

Creates a new folder on the SD card.

**Request:**
```bash
curl -X POST -d "name=NewFolder&path=/" http://crosspoint.local/mkdir
```

**Form Parameters:**

| Parameter | Required | Default | Description                  |
| --------- | -------- | ------- | ---------------------------- |
| `name`    | Yes      | -       | Name of the folder to create |
| `path`    | No       | `/`     | Parent directory path        |

**Response (200 OK):**
```
Folder created: NewFolder
```

**Error Responses:**

| Status | Body                          | Cause                         |
| ------ | ----------------------------- | ----------------------------- |
| 400    | `Missing folder name`         | `name` parameter not provided |
| 400    | `Folder name cannot be empty` | Empty folder name             |
| 400    | `Invalid folder name`         | Name has a separator, or is `.` or `..` |
| 403    | `Cannot create a protected folder` | Parent or new folder is a [protected path](#protected-paths) |
| 400    | `Folder already exists`       | Folder with same name exists  |
| 500    | `Failed to create folder`     | SD card error                 |

---

### POST `/rename` - Rename File or Folder

Renames an item in place. Books keep their reading position, bookmarks, place in
the recent list and reading history: everything the reader keeps under a book's
path moves with it (`BookRelocation`). Sidecar files are not renamed with the book.

**Request:**
```bash
curl -X POST -d "path=/Books/old.epub&name=new.epub" http://crosspoint.local/rename
```

**Form Parameters:**

| Parameter | Required | Description                                          |
| --------- | -------- | ---------------------------------------------------- |
| `path`    | Yes      | Current path of the item                             |
| `name`    | Yes      | New name only, with no `/` or `\`; surrounding spaces are trimmed |

**Response (200 OK):** `Renamed successfully`, or `Name unchanged` when the name is the same.

**Error Responses:**

| Status | Body                                | Cause                                         |
| ------ | ----------------------------------- | --------------------------------------------- |
| 400    | `Missing path or new name`          | A parameter is absent                         |
| 400    | `Invalid path`                      | Empty path or `/`                             |
| 400    | `New name cannot be empty`          | Empty name                                    |
| 400    | `Invalid file name`                 | Name contains `/` or `\`                      |
| 403    | `Cannot rename to protected name`   | New name starts with `.`, is a protected folder, or lands on a [protected path](#protected-paths) |
| 403    | `Cannot rename protected item`      | The item is a [protected path](#protected-paths) |
| 404    | `Item not found`                    | Path does not exist                           |
| 409    | `Target already exists`             | Something already has the new name            |
| 500    | `Failed to open file`, `Failed to rename` | SD card error                           |

Sidecar files are not renamed with the book (see [sidecar-files.md](sidecar-files.md)).

---

### POST `/move` - Move File or Folder

Moves an item into another folder, keeping its name. As for `/rename`, books
keep their reading position, bookmarks, recent-list row and reading history.

**Request:**
```bash
curl -X POST -d "path=/Books/mybook.epub&dest=/Books/SciFi" http://crosspoint.local/move
```

**Form Parameters:**

| Parameter | Required | Description                       |
| --------- | -------- | --------------------------------- |
| `path`    | Yes      | Path of the item to move          |
| `dest`    | Yes      | Destination folder (must exist)   |

**Response (200 OK):** `Moved successfully`, or `Already in destination`.

**Error Responses:**

| Status | Body                              | Cause                                 |
| ------ | --------------------------------- | ------------------------------------- |
| 400    | `Missing path or destination`     | A parameter is absent                 |
| 400    | `Invalid path`, `Invalid destination` | Empty path or `/` as the item     |
| 400    | `Cannot move folder into itself`  | `dest` is inside the item             |
| 400    | `Destination is not a folder`     | `dest` is a file                      |
| 403    | `Cannot move protected item`      | Item, destination or result is a [protected path](#protected-paths) |
| 404    | `Item not found`, `Destination not found` | Path does not exist           |
| 409    | `Target already exists`           | Name already used in `dest`           |
| 500    | `Failed to open file`, `Failed to move` | SD card error                   |

Sidecar files are not moved with the book (see [sidecar-files.md](sidecar-files.md)).

---

### POST `/delete` - Delete Files or Folders

Deletes one or more files or empty folders from the SD card. A deleted book's
layout cache is cleared too; its sidecar files are not touched.

**Request:**
```bash
# Delete one item
curl -X POST -d "path=/Books/mybook.epub" http://crosspoint.local/delete

# Delete several items
curl -X POST --data-urlencode 'paths=["/Books/a.epub","/OldFolder"]' http://crosspoint.local/delete
```

**Form Parameters** (send exactly one of the two):

| Parameter | Description                                  |
| --------- | -------------------------------------------- |
| `path`    | Path to one item                             |
| `paths`   | JSON array of paths                          |

Files and folders are told apart on the device, so no `type` is needed.

**Response (200 OK):**
```
All items deleted successfully
```

**Error Responses:**

| Status | Body                                               | Cause                                  |
| ------ | -------------------------------------------------- | -------------------------------------- |
| 400    | ``Missing `path` or `paths` argument``             | Neither given                          |
| 400    | `Provide either 'path' or 'paths', not both`       | Both given                             |
| 400    | `Invalid paths format`, `No paths provided`        | `paths` is not a non-empty JSON array  |
| 500    | `Failed to delete some items: <list>`              | At least one item failed               |

The 500 body lists each failure as `<path> (<reason>)`, where the reason is one of
`cannot delete root`, `protected item`, `not found`, `folder not empty` or
`deletion failed`. The other items in the request are still processed.

### Protected paths

Every path a request names is checked segment by segment, before the card is touched,
by the file endpoints above, `/upload`, the WebSocket upload, the plugin write
endpoints and WebDAV (except `PROPFIND`; see [WebDAV](#webdav)). The rules are in
`ProtectedPaths` (`lib/FsHelpers/ProtectedPaths.h`); `isProtectedWebPath()`
(`src/network/WebPathGuard.cpp`) applies them with the on-card alias lookup described below.

- The credential stores (`/.crosspoint/wifi.json`, `opds.json`, `koreader.json`)
  are never reachable, and neither is a file whose path starts with one of them, such as
  a temporary or backup copy beside it. Their passwords are obfuscated with a key derived
  from the device's MAC address, which any client on the network can see. With **Show
  Hidden Files** on, the listing of `/.crosspoint` still shows them, but opening one is refused.
- `System Volume Information` and `XTCache` are never reachable, at any depth.
- A segment starting with `.` is refused at any depth, unless **Show Hidden Files**
  is on. Then dot folders can be listed and walked through, so caches under
  `/.crosspoint` can be cleaned up. But an item whose own name starts with `.` still
  cannot be read, written, renamed, moved or deleted. WebDAV ignores the setting
  and always refuses dot segments.
- Names are judged as the SD library opens them: leading spaces and trailing dots
  and spaces are dropped, so `/ .crosspoint` and `/.crosspoint.` are
  `/.crosspoint`. Matching ignores case.
- A path that names an entry by its 8.3 short alias rather than its real name, such
  as `/CROSSP~1` for `/.crosspoint`, is refused. A file whose real name happens to
  look like an alias stays reachable.

---

## Settings

The Settings page edits the same settings as the on-device Settings screens.

### GET `/api/settings` - List Settings

Returns a streamed JSON array with one object per setting. The rows are visited one at a
time (`forEachSetting()`) and only the ones this board offers are listed: a setting for
hardware the board lacks, such as a front light or a touch panel, is left out.

```bash
curl http://crosspoint.local/api/settings
```

**Response (200 OK)**, abridged and with illustrative values:
```json
[
  {"key": "fontFamily", "name": "EPUB Font Family", "category": "Reader", "subcategory": "Reader Font",
   "submenu": "Reader Font", "type": "enum", "value": 0, "options": ["Bookerly", "Noto Sans"]},
  {"key": "fontSize", "name": "EPUB Font Size", "category": "Reader", "subcategory": "",
   "submenu": "Reader Font", "type": "enum", "value": 1,
   "options": ["10pt", "12pt", "14pt", "16pt", "18pt", "20pt", "22pt", "24pt", "26pt"]},
  {"key": "sleepTimeoutMinutes", "name": "Time to Sleep", "category": "Display", "subcategory": "",
   "submenu": "", "type": "value", "value": 10, "min": 0, "max": 60, "step": 1}
]
```

| Field                      | Type   | Description                                                       |
| -------------------------- | ------ | ----------------------------------------------------------------- |
| `key`                      | string | Setting key; the name used by `POST /api/settings`                |
| `name`, `category`, `subcategory`, `submenu` | string | Labels in the device language; the last two may be empty |
| `type`                     | string | `toggle`, `enum`, `value` or `string`                             |
| `value`                    | number or string | Current value. Toggles are `0` or `1`; enums are an index into `options` |
| `options`                  | array  | `enum` only: option labels. For `fontFamily` and `txtFontFamily` this includes the families found on the SD card |
| `min`, `max`, `step`       | number | `value` only: allowed range                                       |
| `obfuscated`, `isSet`      | bool   | `string` only, for credentials: `value` is always `""`; `isSet` says whether one is stored |

Actions without a key are not listed.

`fontSize` and `txtFontSize` list the sizes the selected family offers, read when the
request is served (`SettingInfo::withDynamicOptions`). Their `value` is the position of the
stored size in that list, or of the closest one when the family does not offer it. The
settings file stores the point size itself under the same keys; only this API deals in
positions.

### POST `/api/settings` - Change Settings

Applies the keys present in a JSON body and saves. Unknown keys, keys for settings this
board does not offer, and out-of-range values are skipped silently.

`fontFamily` and `txtFontFamily` are applied after every other key. A `fontSize` or
`txtFontSize` in the same body is therefore read against the sizes of the family that was
selected before the request, which is the list the page was showing. After saving a family
change the Settings page fetches `/api/settings` again, because the size lists follow the family.

```bash
curl -X POST -H "Content-Type: application/json" \
  -d '{"sleepTimeoutMinutes": 15, "fontFamily": 1}' http://crosspoint.local/api/settings
```

**Response (200 OK):** `Applied <n> setting(s)`, where `<n>` counts the keys accepted.

| Status | Body                    | Cause                    |
| ------ | ----------------------- | ------------------------ |
| 400    | `Missing JSON body`     | No body                  |
| 400    | `Invalid JSON: <error>` | Body does not parse      |

For a credential (`obfuscated`) string, sending the key replaces the stored
value, so the page sends it only when the user typed something.

---

## Reading Stats

The numbers behind the Reading Stats screens. The history is read one book at a
time and streamed, never loaded whole.

### GET `/api/stats` - Dashboard Data

```bash
curl http://crosspoint.local/api/stats
```

**Response (200 OK)**, abridged:
```json
{
  "totalSeconds": 86400, "totalSessions": 41, "totalPagesTurned": 5200,
  "bookCount": 12, "finishedBookCount": 3, "todayDayIndex": 20733,
  "currentStreak": 4, "longestStreak": 9,
  "globalDays": [],
  "books": [
    {"docId": "...", "title": "...", "author": "...", "totalSeconds": 7200, "pagesTurned": 300,
     "sessions": 5, "firstReadEpoch": 0, "lastReadEpoch": 0, "progress": 42,
     "finishedCount": 0, "lastFinishedEpoch": 0, "finished": false, "days": [],
     "etaSeconds": 9000}
  ]
}
```

`currentStreak` and `longestStreak` appear only when the clock is set and there is history.
`todayDayIndex` is the device's current local day; the browser uses it to place the
`days` arrays on a calendar. `etaSeconds` is present when an estimate is possible.
With no history at all the body is `{"totalSeconds":0,"books":[]}`.
A history that cannot be read answers `500` with `{"error":"Reading stats could not be read"}`.

### GET `/api/stats/export` - Download a Backup

Streams the history in the `reading-stats.json` format older firmware reads, as an
attachment named `reading-stats.json`. It is the backup, and the way back after a
firmware downgrade.

```bash
curl -OJ http://crosspoint.local/api/stats/export
```

`404` with `{}` when there is no history; `500` with `{}` when it cannot be read.

### POST `/api/stats/remove` - Remove a Book from the Stats

Does what the on-device per-book stats screen does: the book's entry goes and its
time comes out of the totals.

```bash
curl -X POST -H "Content-Type: application/json" -d '{"docId":"..."}' http://crosspoint.local/api/stats/remove
```

| Status | Body                                                  | Cause                      |
| ------ | ----------------------------------------------------- | -------------------------- |
| 200    | `{"ok":true}`                                         | Removed                    |
| 400    | `{"error":"Invalid request"}`                         | No body, or no `docId`     |
| 404    | `{"error":"Book not found"}`                          | Unknown `docId`            |
| 500    | `{"error":"Could not update the reading stats"}`      | Write failed or no memory  |

---

## Fonts

Manages the `.cpfont` families on the SD card. Bodies are JSON. `/api/fonts/manifest`
and `/api/fonts/download` need the device to be online, because the device itself fetches
from the font server over TLS.

| Method | Path                  | Request                                                 | Response                                                                 |
| ------ | --------------------- | ------------------------------------------------------- | ------------------------------------------------------------------------ |
| GET    | `/api/fonts`          | -                                                       | `{"families":[{"name","sizes":[pt...],"files":[{"name","size"}]}],"maxFamilies":N}` |
| GET    | `/api/fonts/manifest` | -                                                       | `{"ok":true,"baseUrl","families":[{"name","description","installed","hasUpdate","totalSize","fileCount"}]}` |
| POST   | `/api/fonts/download` | `{"family":"Name"}` or `{"all":true}`                   | `{"ok":true,"installedCount":N}`                                         |
| POST   | `/api/fonts/upload`   | multipart: one `.cpfont` file; `?family=Name` in the URL | `{"ok":true}`                                                           |
| POST   | `/api/fonts/delete`   | `{"family":"Name"}`                                     | `{"ok":true}`                                                            |

Notes:
- `/api/fonts` rescans the SD card first. `maxFamilies` is the registry's limit on installed SD families.
- `download` with `all` installs every family that is missing or has an update; with `family`, that one.
  The files of a batch share one TLS connection.
- `upload` takes the family name from the query string, because multipart fields are not
  available until the file has arrived. The family name must be valid, and the file must end in `.cpfont`,
  carry no path separators or `..`, and start with the `CPFONT\0\0` magic. A request refused on its names
  (or because the family folder cannot be created) writes nothing and leaves the installed fonts alone; a file
  that fails the magic check, or whose transfer breaks off, is deleted.
- Pressing **Back** on the device cancels an `upload` as it does for [`/upload`](#post-upload---upload-file):
  the connection is closed and the partial file deleted.

**Errors:**

| Status | Body                                                    | Cause                                      |
| ------ | ------------------------------------------------------- | ------------------------------------------ |
| 400    | `{"ok":false,"error":"Invalid request"}`                | `download` body is not JSON                |
| 400    | `{"ok":false,"error":"Missing family"}`                 | `download` with neither `family` nor `all` |
| 400    | `{"error":"Invalid .cpfont file"}`                      | `upload` failed validation                 |
| 400    | `{"error":"Invalid request"}`                           | `delete` body has no `family`              |
| 404    | `{"ok":false,"error":"Family not found in manifest"}`   | `download` of an unknown family            |
| 500    | `{"ok":false,"error":"<message>"}`                      | Manifest fetch or install failed; install failures also carry `family` and `installedCount` |
| 500    | `{"ok":false,"error":"Out of memory"}`                  | `download` could not build its list        |
| 500    | `{"error":"Delete failed"}`                             | `delete` failed                            |

---

## Saved Networks and Servers

Both stores follow the same pattern. A saved entry is addressed by its `index` in the
list. Passwords are never returned: the list only says whether one is set. In a POST,
leaving `password` out keeps the stored one; sending `""` sets an empty one.
The delete routes are POSTs because the device's HTTP server cannot take a body on `DELETE`.

### Wi-Fi networks

| Method | Path               | Body                                            | Response                                   |
| ------ | ------------------ | ----------------------------------------------- | ------------------------------------------ |
| GET    | `/api/wifi`        | -                                               | `[{"index","ssid","hasPassword","isLastConnected"}]` |
| POST   | `/api/wifi`        | `{"ssid","password"?,"index"?}`                 | `OK`                                       |
| POST   | `/api/wifi/delete` | `{"index"}`                                     | `OK`                                       |

POST with `index` edits that entry (the SSID can change); without it, adds a new one.
Errors are `400` with a plain-text body: `Missing JSON body`, `Invalid JSON: ...`, `SSID is required`,
`Invalid network index`, `Missing index`, `Failed to update Wi-Fi network`,
`Cannot add network (limit reached)`, `Failed to delete Wi-Fi network`.

### OPDS servers

| Method | Path                | Body                                                   | Response                                    |
| ------ | ------------------- | ------------------------------------------------------ | ------------------------------------------- |
| GET    | `/api/opds`         | -                                                      | `[{"index","name","url","username","hasPassword"}]` |
| POST   | `/api/opds`         | `{"name","url","username","password"?,"index"?}`       | `OK`                                        |
| POST   | `/api/opds/delete`  | `{"index"}`                                            | `OK`                                        |

POST with `index` edits that entry; without it, adds one. The URL is normalised and
must be valid. Errors are `400` with a plain-text body: `Missing JSON body`, `Invalid JSON: ...`,
`Invalid URL`, `Server name too long`, `URL too long`, `Username too long`, `Password too long`,
`Invalid server index`, `Cannot add server (limit reached)`, `Missing index`;
`500` for `Failed to save server` and `Failed to delete server`.

---

## Plugin Endpoints

Discovery and file serving for SD-card web plugins. The firmware only enumerates
plugin folders and serves their bytes — plugin code runs in the browser. See
[sd-plugins.md](sd-plugins.md) for the folder layout and the JS contract.

### GET `/api/plugins` - List Plugins

Scans `/.crosspoint/plugins`, `/plugins`, and `/.plugins` and returns every
folder holding a `plugin.js`.

**Request:**
```bash
curl http://crosspoint.local/api/plugins
```

**Response (200 OK):**
```json
[{"name":"organize-by-author","title":"Organize by Author","mount":"files","root":"/.crosspoint/plugins",
  "disabled":false,"description":"Sorts the EPUBs of one folder ...","version":"1.0.0"}]
```

| Field          | Description                                                              |
| -------------- | ------------------------------------------------------------------------ |
| `name`         | Folder name; the value to pass to `/plugin`                              |
| `title`        | From `manifest.json`, falling back to `name`                             |
| `mount`        | `settings` (default) or `files` — which page loads the plugin            |
| `root`         | The plugins folder it was found in                                       |
| `disabled`     | Switched off on the Plugins page; the pages do not load it               |
| `description`, `version`, `requires`, `allowedHosts` | From `manifest.json`, when present |

Always 200 with an array; an unreadable or absent plugins folder yields `[]`.

### GET `/api/book/opf` - An EPUB's Own Metadata

`?path=<epub>` answers the OPF package document from inside the book, as it is
(`application/oebps-package+xml`), without the `.opf` sidecar that may override
it. The File Manager's Details dialog reads a book's own metadata from it instead
of downloading the whole book. `404` for a path that is not an EPUB, `422` when
the book has no OPF. A read that fails part-way ends the response early, which a
client sees as malformed XML.

```bash
curl "http://crosspoint.local/api/book/opf?path=/Books/mybook.epub"
```

### POST `/api/book/replace` - Swap In an Optimized Copy of a Book

What the File Manager's in-place **Optimize** uses: it downloads a book, optimizes it in the
browser and sends the result here as the raw request body (any content type except
`multipart/form-data`).

```bash
curl -X POST -H 'Content-Type: application/epub+zip' --data-binary @optimized.epub \
  "http://crosspoint.local/api/book/replace?path=%2FBooks%2Fbook.epub&keep=1"
# -> {"ok":true,"path":"/Books/book.epub","backup":"/Books/book.epub.bak","keptOlderBackup":false,"keptReadingState":true}
```

| Parameter | Description |
| --------- | ----------- |
| `path`    | An existing EPUB the file endpoints may write (the same check as `/upload`'s target) |
| `keep`    | `1` when the new copy has the same chapter list: the reading position and bookmarks stay, and only the laid-out chapters, indexes and thumbnails are dropped. Without it the book starts afresh, as after any re-upload, and its entries leave the bookmark index. |

The body is written to `<path>.new` and swapped in only once complete. The original is kept as
`<path>.bak`; if a `.bak` already exists, it holds the older original and stays, and the current
file is replaced. Reading stats follow the file name, so they always stay.

| Status | Cause |
| ------ | ----- |
| 400    | No such EPUB, a refused path, or the upload was cut off |
| 413    | Body over 512 MB |
| 415    | A `multipart/form-data` body |
| 500    | Could not write the new file, set the original aside, or swap the new one in |

### Installing and managing plugins

What the Plugins page (`GET /plugins`) uses. Installs go to
`/.crosspoint/plugins/<name>`, which the file endpoints leave alone unless
**Show Hidden Files** is on. `name` and `file` must each be a single path
component.

| Endpoint | Does |
| --- | --- |
| `POST /api/plugins/begin?name=` | Starts an install: an empty staging folder `<name>.new` |
| `POST /api/plugins/file?name=&file=` | One file into the staging folder; the raw request body is the file (binary-safe, at most 512 KB) |
| `POST /api/plugins/commit?name=` | Replaces the installed plugin with the staging folder in one rename; `400` without a `plugin.js` |
| `POST /api/plugins/remove?name=` | Removes the plugin from every plugins folder; `404` if there is none |
| `POST /api/plugins/enable?name=&on=0` | Switches the plugin off (a `disabled` file in its folder); `on=1` switches it back on |

```bash
curl -X POST "http://crosspoint.local/api/plugins/begin?name=my-plugin"
curl -X POST -H "Content-Type: application/octet-stream" --data-binary @plugin.js \
  "http://crosspoint.local/api/plugins/file?name=my-plugin&file=plugin.js"
curl -X POST "http://crosspoint.local/api/plugins/commit?name=my-plugin"
```

Each answers `{"ok":true}`, or an error status with `{"error":"..."}`.

### GET `/api/relay` - Fetch a URL for a Plugin

Fetches a URL the browser cannot reach itself and streams the body back. A page
served from the device may not read a cross-origin response unless the remote
sends CORS headers, and most do not; the device is not a browser, so it fetches
on the page's behalf and answers same-origin.

**Request:**
```bash
curl "http://crosspoint.local/api/relay?plugin=metadata-editor&url=https%3A%2F%2Fcovers.example.org%2Fb%2F1.jpg"
```

**Query Parameters:**

| Parameter | Required | Description                                   |
| --------- | -------- | --------------------------------------------- |
| `plugin`  | Yes      | Plugin folder name; its manifest is the allowlist |
| `url`     | Yes      | Absolute `http://` or `https://` URL           |

The body streams back as `application/octet-stream` — response headers are not
forwarded, so the caller infers the type from what it asked for.

**Constraints:**

- **GET only.** A plugin cannot make the device POST anywhere.
- **Allowlisted per plugin.** The host must appear in that plugin's
  `manifest.json` `allowedHosts`. A bare entry matches exactly; one starting
  with a dot matches that suffix. No list, no access.
- **Redirects stay inside the allowlist.** A redirect is followed only to a host
  the same `allowedHosts` list allows; one to any other host fails the request
  (`502`). Following them blindly would mean the host actually fetched was never
  checked against the allowlist.
- **Streamed, not buffered**, with a 4MB ceiling. Nothing large is resident:
  during a web session the largest free block is around 53KB, so a buffered
  response would fail on an ordinary cover image.

**Error Responses:**

| Status | Cause                                                        |
| ------ | ------------------------------------------------------------ |
| 400    | Missing `plugin`/`url`, or a non-http(s) URL                  |
| 403    | Host not listed in the plugin's `allowedHosts`                |
| 502    | The fetch failed before any data arrived                      |
| 503    | Heap too low to serve the request                             |

A transfer that fails or hits the ceiling *after* data has been sent ends as a
truncated 200; the failure is logged (`[WEB]`).

### POST `/api/fetch` - Download to the SD Card

Downloads a URL straight to the card. Relaying through the browser and uploading
the bytes back would move the file twice over WiFi and hold all of it in browser
memory — tolerable for a cover, prohibitive for a book.

```bash
curl -X POST "http://crosspoint.local/api/fetch?plugin=my-plugin&url=https%3A%2F%2Fexample.org%2Fbook.epub&dest=%2FBooks%2Fbook.epub"
# -> {"ok":true,"dest":"/Books/book.epub"}
```

Same allowlist and redirect rules as `/api/relay`. `dest` is normalised, so `..`
cannot climb above the card root, must name a file (not `/` and no trailing `/`), and is
judged as an item under [Protected paths](#protected-paths), as `/upload` judges its target
(`pluginWriteTarget()`). The book's layout cache is invalidated on success.

| Status | Cause                                          |
| ------ | ---------------------------------------------- |
| 400    | Missing `plugin`/`url`/`dest`, bad scheme, or a refused destination |
| 403    | Host not in the plugin's `allowedHosts`        |
| 502    | Download failed                                 |

### POST `/api/plugin-fs` - Write a Small File

Writes one small file, with the raw request body as its contents. `/upload`
already does this; it exists for compatibility with plugins written against the
upstream API.

```bash
curl -X POST --data-binary @cover.jpg \
  "http://crosspoint.local/api/plugin-fs?plugin=my-plugin&path=%2FBooks%2Fbook.jpg"
# -> {"ok":true,"path":"/Books/book.jpg"}
```

The body is streamed to the card as it arrives, so binary files arrive intact,
with any content type except `multipart/form-data` (which the web server parses
as a form; send the bytes themselves). It is written to `<path>.part` and renamed
over `path` at the end, so a body that is too large or cut off never replaces an
existing file. Capped at 64KB, to keep it to small files; bulk transfers belong
in `/upload`. `path` passes the same check as `dest` above.

| Status | Cause                                   |
| ------ | --------------------------------------- |
| 400    | Missing `plugin`/`path`, or a refused path; or the upload was cut off |
| 413    | Body over 64KB — use `/upload`          |
| 415    | A `multipart/form-data` body            |
| 500    | Could not create the file, short write, or could not replace the existing file |

### GET `/plugin` - Serve a Plugin File

Serves one file from a plugin's folder.

**Request:**
```bash
curl "http://crosspoint.local/plugin?name=organize-by-author&file=plugin.js"
```

**Query Parameters:**

| Parameter | Required | Description                          |
| --------- | -------- | ------------------------------------ |
| `name`    | Yes      | Plugin folder name                   |
| `file`    | Yes      | File within that folder (flat, no subdirectories) |

Content-Type is derived from the extension (`.js`, `.css`, `.html`, `.json`,
`.svg`), else `application/octet-stream`.

**Error Responses:**

| Status | Body                | Cause                                                   |
| ------ | ------------------- | ------------------------------------------------------- |
| 400    | `Bad plugin path`   | `name` or `file` empty, containing `/` or `\`, or exactly `.` or `..` |
| 404    | `Plugin not found`  | No such folder in any plugins root                      |
| 404    | `File not found`    | No such file in that folder, or it is a directory       |

---

## WebDAV

A WebDAV class 1 handler (`WebDAVHandler`) answers the methods below on any path that no
route above claims, so a file manager or a mounted network drive can use the card directly:

```bash
curl -X PROPFIND -H "Depth: 1" http://crosspoint.local/Books/
```

| Method           | Behaviour                                                                                  |
| ---------------- | ------------------------------------------------------------------------------------------ |
| `OPTIONS`        | Returns `DAV: 1` and the `Allow` list                                                      |
| `PROPFIND`       | Lists a file or folder. `Depth` 0 or 1 (anything else counts as 1). Children whose names start with `.`, and the system folders, are left out. `207` multistatus; `404` if absent |
| `GET`, `HEAD`    | Streams a file. A folder answers `405`                                                     |
| `PUT`            | Writes `<name>.davtmp`, then renames it over the target, so a failed upload keeps the old file. The parent folder must exist. `201` new, `204` replaced |
| `DELETE`         | Removes a file or an **empty** folder (`409` otherwise). `204`. A deleted book's layout cache is cleared; sidecars are not removed |
| `MKCOL`          | Creates a folder. `201`; `405` if it exists; `409` if the parent is missing; `415` if the request has a body |
| `MOVE`           | Needs a `Destination` header; `Overwrite: F` is honoured (`412`). `201` or `204`           |
| `COPY`           | As `MOVE`, but files only: copying a folder answers `403`                                  |
| `LOCK`, `UNLOCK` | Dummy: returns a fixed token so clients that insist on locking keep working. Nothing is locked |

For `GET`, `HEAD`, `PUT`, `DELETE` and `MKCOL`, and for both ends of `MOVE` and `COPY`, a
path with a hidden segment (starting with `.`) or inside `System Volume Information` or
`XTCache` answers `403`, at any depth, including spellings the SD library maps onto one
(see [Protected paths](#protected-paths)). WebDAV refuses dot segments whatever **Show
Hidden Files** says (`WebDAVHandler::isProtectedPath()`). `PROPFIND` does not judge the path
it is asked about: pointed at a protected folder, it lists that folder's entries (names and
sizes, not contents), leaving out only the dot-named and system ones. The root cannot be
deleted or moved.

Pressing **Back** on the device does not interrupt a `PUT`: the body is read whole inside one
request, and File Transfer is left once it has arrived.

---

## WebSocket Endpoint

### Port 81 - Fast Binary Upload

A WebSocket endpoint for high-speed binary file uploads. More efficient than HTTP multipart for large files.

**Connection:**
```
ws://crosspoint.local:81/
```

**Protocol:**

1. **Client** sends TEXT message: `START:<filename>:<size>:<path>`
2. **Server** responds with TEXT: `READY`
3. **Client** sends BINARY messages with file data chunks
4. **Server** sends TEXT progress updates: `PROGRESS:<received>:<total>`
5. **Server** sends TEXT when complete: `DONE` or `ERROR:<message>`

**Example Session:**

```
Client -> "START:mybook.epub:1234567:/Books"
Server -> "READY"
Client -> [binary chunk 1]
Client -> [binary chunk 2]
Server -> "PROGRESS:65536:1234567"
Client -> [binary chunk 3]
...
Server -> "PROGRESS:1234567:1234567"
Server -> "DONE"
```

**Error Messages:**

| Message                           | Cause                              |
| --------------------------------- | ---------------------------------- |
| `ERROR:Failed to create file`     | Cannot create file on SD card      |
| `ERROR:Invalid START format`      | Malformed START message            |
| `ERROR:Invalid file name`         | File name has a separator, or is `.` or `..` |
| `ERROR:Cannot write to a protected location` | Folder or target is a [protected path](#protected-paths) |
| `ERROR:Upload already in progress` | A second START arrived while an upload was active |
| `ERROR:No upload in progress`     | Binary data received without START |
| `ERROR:Upload overflow`           | More data arrived than the size in START; the partial file is deleted |
| `ERROR:Write failed - disk full?` | SD card write error; the partial file is deleted |

**Example with `websocat`:**
```bash
# Interactive session
websocat ws://crosspoint.local:81

# Then type:
START:mybook.epub:1234567:/Books
# Wait for READY, then send binary data
```

**Notes:**
- Progress updates are sent every 64KB or at completion
- A START with size `0` creates an empty file and answers `DONE` at once, without `READY`
- Disconnection during upload will delete the incomplete file
- Existing files with the same name will be overwritten

---

## Network Modes

The device can operate in two network modes:

### Station Mode (STA)
- Device connects to an existing WiFi network
- IP address assigned by router/DHCP
- `mode` field in `/api/status` returns `"STA"`
- `rssi` field shows signal strength

### Access Point Mode (AP)
- Device creates its own WiFi hotspot
- Default IP is typically `192.168.4.1`
- `mode` field in `/api/status` returns `"AP"`
- `rssi` field returns `0`

---

## Notes

- These examples use `crosspoint.local`. If your network does not support mDNS or the address does not resolve, replace it with the specific **IP Address** displayed on your device screen (e.g., `http://192.168.1.102/`).
- All paths on the SD card start with `/`
- Trailing slashes are automatically stripped (except for root `/`)
- The webserver uses chunked transfer encoding for file listings
