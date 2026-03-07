# Webserver Endpoints

This document describes all HTTP and WebSocket endpoints available on the CrossPoint Reader webserver.

- [Webserver Endpoints](#webserver-endpoints)
  - [Overview](#overview)
  - [HTTP Endpoints](#http-endpoints)
    - [GET `/` - Home Page](#get----home-page)
    - [GET `/files` - File Browser Page](#get-files---file-browser-page)
    - [GET `/api/status` - Device Status](#get-apistatus---device-status)
    - [GET `/api/plugins` - Compile-Time Feature Manifest](#get-apiplugins---compile-time-feature-manifest)
    - [GET `/api/files` - List Files](#get-apifiles---list-files)
    - [GET `/api/recent` - Recent Books](#get-apirecent---recent-books)
    - [GET `/api/book-progress` - Book Progress](#get-apibook-progress---book-progress)
    - [GET `/api/book-pokemon` - Book Pokemon Metadata](#get-apibook-pokemon---book-pokemon-metadata)
    - [PUT `/api/book-pokemon` - Save Book Pokemon Metadata](#put-apibook-pokemon---save-book-pokemon-metadata)
    - [DELETE `/api/book-pokemon` - Clear Book Pokemon Metadata](#delete-apibook-pokemon---clear-book-pokemon-metadata)
    - [POST `/api/todo/entry` - Add TODO or Agenda Entry](#post-apitodoentry---add-todo-or-agenda-entry)
    - [GET `/api/todo/today` - Read Daily Planner Entries](#get-apitodotoday---read-daily-planner-entries)
    - [POST `/api/todo/today` - Save Daily Planner Entries](#post-apitodotoday---save-daily-planner-entries)
    - [GET `/download` - Download File](#get-download---download-file)
    - [POST `/upload` - Upload File](#post-upload---upload-file)
    - [POST `/api/user-fonts/rescan` - Rescan SD User Fonts](#post-apiuser-fontsrescan---rescan-sd-user-fonts)
    - [GET `/api/sleep-images` - List Sleep Images](#get-apisleep-images---list-sleep-images)
    - [GET `/api/sleep-cover` - Get Pinned Sleep Cover](#get-apisleep-cover---get-pinned-sleep-cover)
    - [POST `/api/sleep-cover/pin` - Pin Sleep Cover](#post-apisleep-coverpin---pin-sleep-cover)
    - [POST `/mkdir` - Create Folder](#post-mkdir---create-folder)
    - [POST `/delete` - Delete File or Folder](#post-delete---delete-file-or-folder)
  - [WebSocket Endpoint](#websocket-endpoint)
    - [Port 81 - Fast Binary Upload](#port-81---fast-binary-upload)
  - [Network Modes](#network-modes)
    - [Station Mode (STA)](#station-mode-sta)
    - [Access Point Mode (AP)](#access-point-mode-ap)
  - [Notes](#notes)


## Overview

The CrossPoint Reader exposes a webserver for file management and device monitoring:

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
  "protocolVersion": 1,
  "ip": "192.168.1.100",
  "mode": "STA",
  "rssi": -45,
  "freeHeap": 123456,
  "uptime": 3600
}
```

| Field             | Type   | Description                                               |
| ----------------- | ------ | --------------------------------------------------------- |
| `version`         | string | CrossPoint firmware version                               |
| `protocolVersion` | number | Transport schema version for app integrations             |
| `ip`              | string | Device IP address                                         |
| `mode`            | string | `"STA"` (connected to WiFi) or `"AP"` (access point mode) |
| `rssi`            | number | WiFi signal strength in dBm (0 in AP mode)                |
| `freeHeap`        | number | Free heap memory in bytes                                 |
| `uptime`          | number | Seconds since device boot                                 |

---

### GET `/api/plugins` - Compile-Time Feature Manifest

Returns JSON booleans describing which compile-time features are included in this firmware build.

**Note:** `/api/features` is available as a backward-compatible alias for this endpoint.

**Request:**
```bash
curl http://crosspoint.local/api/plugins
```

**Response (200 OK, example):**
```json
{
  "markdown": true,
  "remote_open_book": true,
  "remote_page_turn": true,
  "todo_planner": true,
  "web_pokedex_plugin": false
}
```

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

### GET `/api/recent` - Recent Books

Returns the current recent-books list with derived reading progress. When the
Pokemon Party module is compiled in and a book has an assignment, the same
response also includes the saved Pokemon metadata for that book.

**Request:**
```bash
curl http://crosspoint.local/api/recent
```

**Response (200 OK, example):**
```json
[
  {
    "path": "/Books/MyBook.epub",
    "title": "My Book",
    "author": "A. Author",
    "last_position": "Ch 3 4/12 27%",
    "last_opened": 0,
    "hasCover": true,
    "progress": {
      "format": "epub",
      "percent": 27.32,
      "page": 4,
      "pageCount": 12,
      "position": "Ch 3 4/12 27%",
      "spineIndex": 2
    },
    "pokemon": {
      "id": 4,
      "name": "charmander"
    }
  }
]
```

| Field | Type | Description |
| ----- | ---- | ----------- |
| `path` | string | Absolute SD path of the recent book |
| `title` | string | Cached display title |
| `author` | string | Cached author |
| `last_position` | string | Human-readable cached progress label, or `""` when no progress is cached |
| `last_opened` | number | Compatibility placeholder, currently always `0` |
| `hasCover` | boolean | `true` when the recent-book cache already has a cover BMP |
| `progress` | object or `null` | Derived progress payload from the book cache |
| `pokemon` | object | Saved Pokemon metadata when `ENABLE_POKEMON_PARTY` is enabled and the book has an assignment |

**`progress` fields:**

| Field | Type | Description |
| ----- | ---- | ----------- |
| `format` | string | One of `epub`, `txt`, `markdown`, or `xtc` |
| `percent` | number | Reading progress percentage, rounded to 2 decimals |
| `page` | number | Current 1-based display page |
| `pageCount` | number | Total pages for the current section/book |
| `position` | string | Same formatted progress label used by `last_position` |
| `spineIndex` | number | EPUB-only 0-based spine/chapter index |

**Notes:**
- `pokemon` is omitted entirely unless the Pokemon Party API is compiled in and
  metadata exists for that book.
- Supported cached progress sources are `.epub`, `.txt`, `.md`, `.xtc`, and `.xtch`.

---

### GET `/api/book-progress` - Book Progress

Returns normalized cached progress for a single supported book file.

**Request:**
```bash
curl "http://crosspoint.local/api/book-progress?path=/Books/MyBook.epub"
```

**Query Parameters:**

| Parameter | Required | Description |
| --------- | -------- | ----------- |
| `path` | Yes | Absolute SD path to a supported book |

**Response (200 OK, example):**
```json
{
  "path": "/Books/MyBook.epub",
  "progress": {
    "format": "epub",
    "percent": 27.32,
    "page": 4,
    "pageCount": 12,
    "position": "Ch 3 4/12 27%",
    "spineIndex": 2
  }
}
```

If no cached progress exists yet, the endpoint still returns `200 OK` with:

```json
{
  "path": "/Books/MyBook.epub",
  "progress": null
}
```

**Error Responses:**

| Status | Body | Cause |
| ------ | ---- | ----- |
| 400 | `Missing path` | `path` parameter missing |
| 400 | `Invalid path` | Path fails SD path validation |
| 400 | `Unsupported book type` | File extension is not one of `.epub`, `.txt`, `.md`, `.xtc`, `.xtch` |
| 403 | `Cannot access protected items` | Path targets protected storage |
| 404 | `Book not found` | File does not exist |

**Notes:**
- `page` is always 1-based for display.
- `spineIndex` is present only for EPUB progress and remains 0-based.

---

### GET `/api/book-pokemon` - Book Pokemon Metadata

Returns saved Pokemon metadata for a single supported book.

This route is only registered when `ENABLE_POKEMON_PARTY` is enabled. When the
feature is disabled the route does not exist and the webserver returns `404`.

**Request:**
```bash
curl "http://crosspoint.local/api/book-pokemon?path=/Books/MyBook.epub"
```

**Response (200 OK, example):**
```json
{
  "path": "/Books/MyBook.epub",
  "pokemon": {
    "id": 25,
    "name": "pikachu",
    "types": ["electric"],
    "evolutionChain": [
      {"id": 172, "name": "pichu", "minLevel": null, "trigger": "friendship"},
      {"id": 25, "name": "pikachu", "minLevel": null, "trigger": null},
      {"id": 26, "name": "raichu", "minLevel": null, "trigger": "use-item"}
    ]
  }
}
```

If the book has no assignment yet, the endpoint returns `200 OK` with
`"pokemon": null`.

**Error Responses:**

| Status | Body | Cause |
| ------ | ---- | ----- |
| 400 | `Missing path` | `path` parameter missing |
| 400 | `Invalid path` | Path fails SD path validation |
| 400 | `Unsupported book type` | File extension is not one of `.epub`, `.txt`, `.md`, `.xtc`, `.xtch` |
| 403 | `Cannot access protected items` | Path targets protected storage |
| 404 | `Book not found` | File does not exist |

---

### PUT `/api/book-pokemon` - Save Book Pokemon Metadata

Stores or replaces Pokemon metadata for a supported book. The payload is saved
in the book's cache sidecar (`pokemon.json`).

This route is only registered when `ENABLE_POKEMON_PARTY` is enabled.

**Request:**
```bash
curl -X PUT \
  -H "Content-Type: application/json" \
  -d '{"path":"/Books/MyBook.epub","pokemon":{"id":1,"name":"bulbasaur","types":["grass","poison"]}}' \
  http://crosspoint.local/api/book-pokemon
```

**JSON Body:**

| Field | Required | Description |
| ----- | -------- | ----------- |
| `path` | Yes | Absolute SD path to a supported book |
| `pokemon` | Yes | Arbitrary JSON object to persist as the book's Pokemon metadata |

**Response (200 OK):**
```json
{
  "ok": true,
  "path": "/Books/MyBook.epub",
  "pokemon": {
    "id": 1,
    "name": "bulbasaur",
    "types": ["grass", "poison"]
  }
}
```

**Error Responses:**

| Status | Body | Cause |
| ------ | ---- | ----- |
| 400 | `Missing body` | No request body was sent |
| 400 | `Invalid JSON body` | Request body is not valid JSON |
| 400 | `Missing path` | `path` missing or empty |
| 400 | `Missing pokemon object` | `pokemon` is absent or not an object |
| 400 | `Invalid path` | Path fails SD path validation |
| 400 | `Unsupported book type` | File extension is not one of `.epub`, `.txt`, `.md`, `.xtc`, `.xtch` |
| 403 | `Cannot access protected items` | Path targets protected storage |
| 404 | `Book not found` | File does not exist |
| 500 | `Failed to save pokemon data` | Sidecar write failed |

---

### DELETE `/api/book-pokemon` - Clear Book Pokemon Metadata

Deletes the saved Pokemon assignment for a supported book.

This route is only registered when `ENABLE_POKEMON_PARTY` is enabled.

**Request:**
```bash
curl -X DELETE "http://crosspoint.local/api/book-pokemon?path=/Books/MyBook.epub"
```

**Response (200 OK):**
```json
{
  "ok": true,
  "path": "/Books/MyBook.epub"
}
```

**Error Responses:**

| Status | Body | Cause |
| ------ | ---- | ----- |
| 400 | `Missing path` | `path` parameter missing |
| 400 | `Invalid path` | Path fails SD path validation |
| 400 | `Unsupported book type` | File extension is not one of `.epub`, `.txt`, `.md`, `.xtc`, `.xtch` |
| 403 | `Cannot access protected items` | Path targets protected storage |
| 404 | `Book not found` | File does not exist |
| 500 | `Failed to delete pokemon data` | Sidecar delete failed |

---

### POST `/api/todo/entry` - Add TODO or Agenda Entry

Appends an entry to today's daily TODO file when the TODO planner feature is compiled in.

**Request:**
```bash
# Add a task entry
curl -X POST -d "type=todo&text=Buy milk" http://crosspoint.local/api/todo/entry

# Add an agenda/note entry
curl -X POST -d "type=agenda&text=Meeting at 14:00" http://crosspoint.local/api/todo/entry
```

**Form Parameters:**

| Parameter | Required | Description |
| --------- | -------- | ----------- |
| `text`    | Yes      | Entry text (1-300 chars, newlines are normalized to spaces) |
| `type`    | No       | `todo` (default) or `agenda` |

**Storage selection:**
- If today's `.md` file exists, append there.
- Else if today's `.txt` file exists, append there.
- Else create `.md` when markdown support is enabled, or `.txt` when markdown support is disabled.

**Response (200 OK):**
```json
{"ok":true}
```

**Error Responses:**

| Status | Body | Cause |
| ------ | ---- | ----- |
| 400 | `Missing text` | `text` parameter missing |
| 400 | `Invalid text` | Empty or too long text |
| 404 | `TODO planner disabled` | Feature is not compiled in |
| 503 | `Date unavailable` | Device date could not be resolved |
| 500 | `Failed to write TODO entry` | SD write failed |

---

### GET `/api/todo/today` - Read Daily Planner Entries

Returns today's TODO/agenda entries in structured form for web UI editing.

**Request:**
```bash
curl http://crosspoint.local/api/todo/today
```

**Response (200 OK):**
```json
{
  "ok": true,
  "date": "2026-02-27",
  "path": "/daily/2026-02-27.md",
  "items": [
    {"text": "Buy milk", "checked": false, "isHeader": false},
    {"text": "Meeting at 14:00", "checked": false, "isHeader": true}
  ]
}
```

| Field | Type | Description |
| ----- | ---- | ----------- |
| `date` | string | Current device date used for daily file selection |
| `path` | string | Resolved daily planner file path (`.md`/`.txt`) |
| `items[].text` | string | Entry text |
| `items[].checked` | boolean | Checkbox state for TODO entries |
| `items[].isHeader` | boolean | `true` for agenda/note entries |

**Error Responses:**

| Status | Body | Cause |
| ------ | ---- | ----- |
| 404 | `TODO planner disabled` | Feature is not compiled in |
| 503 | `Date unavailable` | Device date could not be resolved |

---

### POST `/api/todo/today` - Save Daily Planner Entries

Rewrites today's TODO/agenda list from a structured JSON payload.

**Request:**
```bash
curl -X POST \
  -H "Content-Type: application/json" \
  -d '{"items":[{"text":"Buy milk","checked":false,"isHeader":false},{"text":"Meeting at 14:00","isHeader":true}]}' \
  http://crosspoint.local/api/todo/today
```

**JSON Body:**

| Field | Required | Description |
| ----- | -------- | ----------- |
| `items` | Yes | Array of planner entries |
| `items[].text` | Yes | Entry text (trimmed, max 300 chars, empty entries ignored) |
| `items[].checked` | No | Checkbox state for TODO entries |
| `items[].isHeader` | No | `true` for agenda/note entries |

**Response (200 OK):**
```json
{"ok":true,"date":"2026-02-27","path":"/daily/2026-02-27.md"}
```

**Error Responses:**

| Status | Body | Cause |
| ------ | ---- | ----- |
| 400 | `Missing body` | Request body missing |
| 400 | `Invalid JSON body` | Invalid JSON payload |
| 400 | `Missing items array` | `items` key missing/not an array |
| 404 | `TODO planner disabled` | Feature is not compiled in |
| 503 | `Date unavailable` | Device date could not be resolved |
| 500 | `Failed to write TODO file` | SD write failed |

---

### GET `/download` - Download File

Downloads a file from the SD card.

**Request:**
```bash
# Download from root
curl -L "http://crosspoint.local/download?path=/mybook.epub" -o mybook.epub

# Download from a nested folder
curl -L "http://crosspoint.local/download?path=/Books/Fiction/mybook.epub" -o mybook.epub
```

**Query Parameters:**

| Parameter | Required | Description              |
| --------- | -------- | ------------------------ |
| `path`    | Yes      | Absolute file path on SD |

**Response (200 OK):**
- Binary file stream (`application/octet-stream` for most files)
- `application/epub+zip` for `.epub` files
- `Content-Disposition: attachment; filename="..."`

**Error Responses:**

| Status | Body                                | Cause                           |
| ------ | ----------------------------------- | ------------------------------- |
| 400    | `Missing path`                      | `path` parameter not provided   |
| 400    | `Invalid path`                      | Invalid or root path            |
| 400    | `Path is a directory`               | Attempted to download a folder  |
| 403    | `Cannot access system files`        | Hidden file (starts with `.`)   |
| 403    | `Cannot access protected items`     | Protected system file/folder    |
| 404    | `Item not found`                    | Path does not exist             |
| 500    | `Failed to open file`               | SD card access/open error       |

**Protected Items:**
- Files/folders starting with `.`
- `System Volume Information`
- `XTCache`

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

### POST `/api/user-fonts/rescan` - Rescan SD User Fonts

Rescans the `/fonts` directory for `.cpf` fonts and reloads the currently selected external font if enabled.

**Request:**
```bash
curl -X POST http://crosspoint.local/api/user-fonts/rescan
```

**Response (200 OK):**
```json
{
  "families": 3,
  "activeLoaded": true
}
```

| Field          | Type    | Description |
| -------------- | ------- | ----------- |
| `families`     | number  | Number of discovered font families |
| `activeLoaded` | boolean | `true` when the active external font could be loaded after rescan |

---

### GET `/api/sleep-images` - List Sleep Images

Returns a JSON array of images in the `/sleep/` folder on the SD card.

**Request:**
```bash
curl http://crosspoint.local/api/sleep-images
```

**Response (200 OK):**
```json
[
  {"path": "/sleep/foo.bmp", "name": "foo.bmp"},
  {"path": "/sleep/bar.bmp", "name": "bar.bmp"}
]
```

**Notes:**
- Returns an empty array `[]` if the `/sleep/` folder does not exist or contains no images.

---

### GET `/api/sleep-cover` - Get Pinned Sleep Cover

Returns the currently pinned sleep cover image path.

**Request:**
```bash
curl http://crosspoint.local/api/sleep-cover
```

**Response (200 OK):**
```json
{
  "path": "/sleep/foo.bmp",
  "name": "foo.bmp"
}
```

**Notes:**
- If no image is pinned, `path` and `name` will be empty strings.

---

### POST `/api/sleep-cover/pin` - Pin Sleep Cover

Sets a specific image or book cover to be displayed every time the device sleeps.

**Request (Mode A - Pin Sleep Folder Image):**
```bash
# Pin an image from the /sleep/ folder
curl -X POST -H "Content-Type: application/json" -d '{"path": "/sleep/foo.bmp"}' http://crosspoint.local/api/sleep-cover/pin

# Clear the pin (revert to random rotation)
curl -X POST -H "Content-Type: application/json" -d '{"path": ""}' http://crosspoint.local/api/sleep-cover/pin
```

**Request (Mode B - Pin Book Cover):**
```bash
# Pin the cover of a specific book
curl -X POST -H "Content-Type: application/json" -d '{"bookPath": "/books/mybook.epub"}' http://crosspoint.local/api/sleep-cover/pin
```

**Response (200 OK):**
```json
{
  "pinnedPath": "/sleep/foo.bmp"
}
```

**Notes:**
- **Mode A (Image Path):** Pins the specified file. If an empty path is sent, the pin is cleared. On success, the clearing request returns the plain text "Cleared".
- **Mode B (Book Path):** Copies the book's cover BMP to `/sleep/.pinned-cover.bmp` and sets that as the pinned image.
- When a cover is pinned, it will be used regardless of the random rotation setting, provided the **Sleep Screen** is set to **Custom**.

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
