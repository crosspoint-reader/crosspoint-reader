# Webserver Endpoints

This document describes all HTTP and WebSocket endpoints available on the Marginalia webserver.

- [Webserver Endpoints](#webserver-endpoints)
  - [Overview](#overview)
  - [HTTP Endpoints](#http-endpoints)
    - [GET `/` - Home Page](#get----home-page)
    - [GET `/files` - File Browser Page](#get-files---file-browser-page)
    - [GET `/packages` - Package Manager Page](#get-packages---package-manager-page)
    - [GET `/api/status` - Device Status](#get-apistatus---device-status)
    - [GET `/api/files` - List Files](#get-apifiles---list-files)
    - [GET `/api/packages` - List Packages](#get-apipackages---list-packages)
    - [POST `/api/packages/upload` - Upload Package Folder File or Archive](#post-apipackagesupload---upload-package-folder-file-or-archive)
    - [POST `/api/packages/download` - Download Package Archive](#post-apipackagesdownload---download-package-archive)
    - [POST `/api/packages/install` - Install Inbox Package](#post-apipackagesinstall---install-inbox-package)
    - [POST `/api/packages/enable` - Enable or Disable Package](#post-apipackagesenable---enable-or-disable-package)
    - [POST `/api/packages/uninstall` - Uninstall Package](#post-apipackagesuninstall---uninstall-package)
    - [POST `/upload` - Upload File](#post-upload---upload-file)
    - [POST `/mkdir` - Create Folder](#post-mkdir---create-folder)
    - [POST `/delete` - Delete File or Folder](#post-delete---delete-file-or-folder)
  - [WebSocket Endpoint](#websocket-endpoint)
    - [Port 81 - Fast Binary Upload](#port-81---fast-binary-upload)
  - [Network Modes](#network-modes)
    - [Station Mode (STA)](#station-mode-sta)
    - [Access Point Mode (AP)](#access-point-mode-ap)
  - [Notes](#notes)


## Overview

Marginalia exposes a webserver for file management and device monitoring:

- **HTTP Server**: Port 80
- **WebSocket Server**: Port 81 (for fast binary uploads)

---

## HTTP Endpoints

### GET `/` - Home Page

Serves the home page HTML interface.

**Request:**
```bash
curl http://crosspoint.local/
```

**Response:** HTML page (200 OK)

---

### GET `/files` - File Browser Page

Serves the file browser HTML interface.

**Request:**
```bash
curl http://crosspoint.local/files
```

**Response:** HTML page (200 OK)

---

### GET `/api/status` - Device Status

Returns JSON with device status information.

**Request:**
```bash
curl http://crosspoint.local/api/status
```

**Response (200 OK):**
```json
{
  "version": "1.0.0",
  "ip": "192.168.1.100",
  "mode": "STA",
  "rssi": -45,
  "freeHeap": 123456,
  "uptime": 3600
}
```

| Field      | Type   | Description                                               |
| ---------- | ------ | --------------------------------------------------------- |
| `version`  | string | Marginalia firmware version                               |
| `ip`       | string | Device IP address                                         |
| `mode`     | string | `"STA"` (connected to WiFi) or `"AP"` (access point mode) |
| `rssi`     | number | WiFi signal strength in dBm (0 in AP mode)                |
| `freeHeap` | number | Free heap memory in bytes                                 |
| `uptime`   | number | Seconds since device boot                                 |

---

### GET `/packages` - Package Manager Page

Serves the package manager page for installed packages, the package inbox, hub catalog downloads, `.mpkg.zip` archive
uploads, and legacy package folder uploads.

**Request:**
```bash
curl http://crosspoint.local/packages
```

**Response:** HTML page (200 OK)

---

### GET `/api/files` - List Files

Returns a JSON array of files and folders in the specified directory.

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
- Hidden files (starting with `.`) are automatically filtered out
- System folders (`System Volume Information`, `XTCache`) are hidden

---

### GET `/api/packages` - List Packages

Returns active packages and packages waiting in the inbox.

**Request:**
```bash
curl http://crosspoint.local/api/packages
```

**Response (200 OK):**
```json
{
  "root": "/.marginalia/packages",
  "inboxRoot": "/.marginalia/inbox",
  "activeScanError": false,
  "inboxScanError": false,
  "active": [
    {
      "id": "org.example.theme",
      "directoryName": "org.example.theme",
      "name": "Example Theme",
      "version": "0.1.0",
      "kind": "theme",
      "execution": "module",
      "summary": "Example package",
      "author": "Example",
      "manifestPath": "/.marginalia/packages/org.example.theme/manifest.json",
      "enabled": true,
      "compatible": true,
      "compatibilityError": ""
    }
  ],
  "inbox": []
}
```

---

### POST `/api/packages/upload` - Upload Package Folder File or Archive

Uploads either one file into the package inbox or one complete `.mpkg.zip` archive. Folder upload mode is kept for local
development. Archive upload mode is the package format used by the SDK, registry, and hub.

**Folder upload request:**
```bash
curl -X POST \
  -F "file=@manifest.json" \
  "http://crosspoint.local/api/packages/upload?package=org.example.theme&path=manifest.json"
```

**Archive upload request:**
```bash
curl -X POST \
  -F "file=@org.example.theme-0.1.0.mpkg.zip" \
  "http://crosspoint.local/api/packages/upload?archive=1"
```

**Query Parameters:**

| Parameter | Required | Description |
| --------- | -------- | ----------- |
| `archive` | No | Set to `1` to upload and extract a `.mpkg.zip` archive. |
| `package` | Folder mode only | Inbox folder name. Must be a safe package id. |
| `path` | Folder mode only | Relative path inside the package folder. Must not contain `..`, hidden components, or unsafe path characters. |
| `reset` | Folder mode only | Set to `1` on the first file of a folder upload to replace any previous inbox copy. |

**Folder upload response (200 OK):**
```json
{"ok": true}
```

**Archive upload response (200 OK):**
```json
{"ok": true, "id": "org.example.theme", "name": "Example Theme"}
```

**Archive constraints:**
- The archive must contain `manifest.json` at the root.
- Entries are extracted into `/.marginalia/inbox/<manifest id>/`.
- Unsafe paths, hidden path components, `..`, absolute paths, unsupported compression methods, too many entries, and
  oversized entries are rejected before install.
- Uploading an archive only stages it in the inbox. Use `/api/packages/install` to activate it.

---

### POST `/api/packages/download` - Download Package Archive

Downloads a `.mpkg.zip` archive from a registry or hub URL, verifies optional size and SHA-256 metadata, extracts it into
the inbox, and returns the staged package identity.

**Request:**
```bash
curl -X POST \
  -H "Content-Type: application/json" \
  -d '{"url":"https://marginalia-hub.vercel.app/packages/org.example.theme-0.1.0.mpkg.zip","sha256":"...","size":1234}' \
  http://crosspoint.local/api/packages/download
```

**JSON Body:**

| Field | Required | Description |
| ----- | -------- | ----------- |
| `url` | Yes | HTTP or HTTPS URL ending in `.mpkg.zip`. |
| `sha256` | No | Expected archive SHA-256 hex digest. If present, the downloaded archive must match. |
| `size` | No | Expected archive byte size. If present, the downloaded archive must match. |

**Response (200 OK):**
```json
{"ok": true, "id": "org.example.theme", "name": "Example Theme"}
```

**Notes:**
- The package page uses this endpoint for hub catalog installs.
- Downloaded archives are staged in the inbox, then installed through `/api/packages/install`.
- Catalog signatures are not enforced yet; archive hash verification is the current trust check.

---

### POST `/api/packages/install` - Install Inbox Package

Validates an inbox package manifest and activates it under `/.marginalia/packages/<manifest id>/`.

**Request:**
```bash
curl -X POST \
  -H "Content-Type: application/json" \
  -d '{"package":"org.example.theme"}' \
  http://crosspoint.local/api/packages/install
```

**Response (200 OK):**
```json
{"ok": true, "id": "org.example.theme", "name": "Example Theme"}
```

**Notes:**
- The install flow stages the package before activation.
- If an older active package exists, it is backed up during the rename transaction and restored if activation fails.
- Packages with incompatible `target` metadata are rejected before activation.
- This does not execute package code yet.

---

### POST `/api/packages/enable` - Enable or Disable Package

Updates firmware-owned lifecycle state for an installed package.

**Request:**
```bash
curl -X POST \
  -H "Content-Type: application/json" \
  -d '{"id":"org.example.theme","enabled":false}' \
  http://crosspoint.local/api/packages/enable
```

**Response (200 OK):**
```json
{"ok": true, "id": "org.example.theme", "enabled": false}
```

**Notes:**
- State is stored outside the package folder under `/.marginalia/package-state/`.
- Disabling a package does not remove package files.
- Runtime loaders should skip disabled packages when execution support is added.

---

### POST `/api/packages/uninstall` - Uninstall Package

Removes an installed package and its local lifecycle state.

**Request:**
```bash
curl -X POST \
  -H "Content-Type: application/json" \
  -d '{"id":"org.example.theme"}' \
  http://crosspoint.local/api/packages/uninstall
```

**Response (200 OK):**
```json
{"ok": true, "id": "org.example.theme"}
```

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

| Parameter | Required | Default | Description                     |
| --------- | -------- | ------- | ------------------------------- |
| `path`    | No       | `/`     | Target directory for the upload |

**Response (200 OK):**
```
File uploaded successfully: mybook.epub
```

**Error Responses:**

| Status | Body                                            | Cause                       |
| ------ | ----------------------------------------------- | --------------------------- |
| 400    | `Failed to create file on SD card`              | Cannot create file          |
| 400    | `Failed to write to SD card - disk may be full` | Write error during upload   |
| 400    | `Failed to write final data to SD card`         | Error flushing final buffer |
| 400    | `Upload aborted`                                | Client aborted the upload   |
| 400    | `Unknown error during upload`                   | Unspecified error           |

**Notes:**
- Existing files with the same name will be overwritten
- Uses a 4KB buffer for efficient SD card writes

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
| 400    | `Folder already exists`       | Folder with same name exists  |
| 500    | `Failed to create folder`     | SD card error                 |

---

### POST `/delete` - Delete File or Folder

Deletes a file or folder from the SD card.

**Request:**
```bash
# Delete a file
curl -X POST -d "path=/Books/mybook.epub&type=file" http://crosspoint.local/delete

# Delete an empty folder
curl -X POST -d "path=/OldFolder&type=folder" http://crosspoint.local/delete
```

**Form Parameters:**

| Parameter | Required | Default | Description                      |
| --------- | -------- | ------- | -------------------------------- |
| `path`    | Yes      | -       | Path to the item to delete       |
| `type`    | No       | `file`  | Type of item: `file` or `folder` |

**Response (200 OK):**
```
Deleted successfully
```

**Error Responses:**

| Status | Body                                          | Cause                         |
| ------ | --------------------------------------------- | ----------------------------- |
| 400    | `Missing path`                                | `path` parameter not provided |
| 400    | `Cannot delete root directory`                | Attempted to delete `/`       |
| 400    | `Folder is not empty. Delete contents first.` | Non-empty folder              |
| 403    | `Cannot delete system files`                  | Hidden file (starts with `.`) |
| 403    | `Cannot delete protected items`               | Protected system folder       |
| 404    | `Item not found`                              | Path does not exist           |
| 500    | `Failed to delete item`                       | SD card error                 |

**Protected Items:**
- Files/folders starting with `.`
- `System Volume Information`
- `XTCache`

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
| `ERROR:No upload in progress`     | Binary data received without START |
| `ERROR:Write failed - disk full?` | SD card write error                |

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
