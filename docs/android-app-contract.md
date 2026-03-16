# Android Companion App — Firmware Contract

This file defines exactly what the Android companion app (ForkDriftApp) expects
from the firmware. It is the authoritative reference for closing gaps between
the two codebases. Keep it up to date when either side changes.

---

## Transport overview

| Transport | Android class | Status |
|-----------|--------------|--------|
| Wi-Fi HTTP/WS | `WifiTransport` | Implemented both sides |
| USB serial JSON-RPC | `UsbTransport` | Implemented both sides |
| USB mass storage | `UsbMassStorageTransport` | Implemented both sides (libaums) |
| BLE provisioning | `BleTransport` | Implemented both sides |

For the machine-readable HTTP contract, see [docs/http-api.openapi.yaml](./http-api.openapi.yaml).
This document remains the umbrella transport contract for HTTP, UDP discovery,
USB serial JSON-RPC, BLE provisioning, and other non-OpenAPI surfaces.

---

## 1. UDP device discovery

**Android:** `DeviceDiscovery.startScan()` broadcasts `"hello"` to every usable IPv4
interface broadcast address plus `255.255.255.255` on UDP 8134, then listens for
replies.

**Firmware must respond with (one UDP packet back to sender):**
```
crosspoint (on <hostname>);<wsPort>
```
Example: `crosspoint (on MyDevice);81`

The Android parser splits on `;`, takes the WS port from part[1], keeps part[0]
as the display name, and uses the sender IP as the connection host.

**Current status:** ✅ Firmware implements this on UDP 8134.

---

## 2. mDNS hostname

**Android:** `DeviceDiscovery.startScan()` also probes `http://crosspoint.local/api/status`
(2 s timeout). If it responds 200 the device appears as `"crosspoint.local"` in the
discovered device list.

**Firmware must:** call `MDNS.begin(<hostname>)` after WiFi connects so the device
responds to `crosspoint.local` on the LAN.

**Current status:** ✅ Implemented. `MDNS.begin(hostname)` is called after WiFi connects in all
three network entry points (BackgroundWebServer, CrossPointWebServerActivity, CalibreConnectActivity).

**Note:** The advertised hostname is dynamic — `crosspoint-{deviceName}` when a device name is
configured, or `crosspoint-{last4mac}` otherwise. Android discovery should use the hostname
returned by UDP discovery (§1) rather than assuming a fixed `crosspoint.local`.

---

## 3. GET /api/status

**Android DTO** (`StatusDto` in `WifiTransport.kt`):
```json
{
  "version":            "string",
  "protocolVersion":    1,
  "wifi_status":        "Connected | Disconnected | AP Mode",
  "ip":                 "192.168.x.x",
  "mode":               "STA | AP",
  "rssi":               -50,
  "free_heap":          204800,
  "uptime":             3600,
  "otaSelectedBundle":  "string",
  "otaInstalledBundle": "string"
}
```

**Firmware response** (`handleStatus()`) — canonical format:
```json
{
  "version":              "string",
  "protocolVersion":      1,
  "wifiStatus":           "Connected | Disconnected | AP Mode",
  "ip":                   "192.168.x.x",
  "mode":                 "STA | AP",
  "rssi":                 -50,
  "freeHeap":             204800,
  "uptime":               3600,
  "openBook":             "/Books/book.epub",
  "otaSelectedBundle":    "string",
  "otaInstalledBundle":   "string",
  "otaInstalledFeatures": "string"
}
```

**Current status:** ✅ Fixed on Android side — `StatusDto` now uses camelCase field names
matching the firmware directly (no `@SerializedName` needed for `wifiStatus`/`freeHeap`).
Android only reads the stable subset above and safely ignores extra fields such
as `openBook`.

---

## 4. GET /api/files?path=<path>

**Firmware response** (`handleFileListData()`) — this is the canonical format:
```json
[
  {
    "name":        "book.epub",
    "size":        1048576,
    "isDirectory": false,
    "isEpub":      true
  }
]
```

**Android adapter** (`WifiTransport.listFiles()`): deserialises the bare array directly,
constructs `path` as `listingPath.trimEnd('/') + "/" + name` (same logic the web UI uses),
and ignores `isEpub`.

**Current status:** ✅ Fixed on Android side — no firmware change needed.

Fields Android reads: `name`, `isDirectory`, `size`, `modified` (defaults to 0 if absent).
`isEpub` and any other extra fields are silently ignored by Gson.

---

## 5. GET /api/recent

**Android DTO** (`RecentDto` in `WifiTransport.kt`):
```json
[
  {
    "path":          "/Books/book.epub",
    "title":         "Title",
    "author":        "Author",
    "last_position": "epubcfi(...)",
    "last_opened":   1700000000
  }
]
```

**Current firmware response** (`handleRecentBooks()`):
```json
[
  {
    "path":          "/Books/book.epub",
    "title":         "Title",
    "author":        "Author",
    "last_position": "Ch 3 4/12 27%",
    "last_opened":   0,
    "hasCover":      true,
    "progress": {
      "format":     "epub",
      "percent":    27.32,
      "page":       4,
      "pageCount":  12,
      "position":   "Ch 3 4/12 27%",
      "spineIndex": 2
    },
    "pokemon": {
      "id": 4,
      "name": "charmander",
      "speciesName": "Charmander",
      "evolutionChain": [...]
    }
  }
]
```

**Current status:** ✅ Implemented. Firmware now returns real cached
`last_position` values when progress exists and keeps `last_opened` at `0` for
compatibility. When no progress cache exists yet, `last_position` remains `""`
and `progress` is `null`.

Android only depends on `path`, `title`, `author`, `last_position`, and
`last_opened`. Extra fields such as `hasCover`, `progress`, and `pokemon` are
ignored by Gson and are safe to add. `pokemon` is only present when the
`pokemon_party` firmware module is compiled in and that book has an assignment.
The `pokemon` field contains the full metadata object saved for the book.

---

## 6. GET /api/book-progress?path=<encoded-path>

Returns normalized cached progress for a single book.

**Response:**
```json
{
  "path": "/Books/book.epub",
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

**Current status:** ✅ Implemented. Returns `{"path": "...", "progress": null}`
when the book exists but has no cached progress yet.

---

## 7. GET /api/book-pokemon?path=<encoded-path>

Returns saved Pokemon metadata for a single book. Only available if `pokemon_party` is enabled.

**Response:**
```json
{
  "path": "/Books/book.epub",
  "pokemon": { ... }
}
```

**Current status:** ✅ Implemented. Returns `{"pokemon": null}` when no pokemon
assignment exists for the book yet.

---

## 8. PUT /api/book-pokemon

Stores Pokemon metadata for a book.

**Request body:**
```json
{
  "path": "/Books/book.epub",
  "pokemon": { ... }
}
```

**Response:**
```json
{
  "ok": true,
  "path": "/Books/book.epub",
  "pokemon": { ... }
}
```

**Current status:** ✅ Implemented.

---

## 9. DELETE /api/book-pokemon?path=<encoded-path>

Clears Pokemon metadata for a book.

**Response:**
```json
{
  "ok": true,
  "path": "/Books/book.epub"
}
```

**Current status:** ✅ Implemented.

---

## 10. GET /api/plugins

**Android DTO** (`PluginFlags` in `WifiTransport.kt`):
```json
{
  "web_wifi_setup": false,
  "ota_updates":    false,
  "remote_keyboard_input": true,
  "remote_open_book": true,
  "remote_page_turn": true,
  "user_fonts":     false,
  "todo_planner":   false
}
```

**Current firmware response** (`handlePlugins()` → `FeatureModules::getFeatureMapJson()`):
Returns the full feature catalog as a flat JSON object with snake_case keys.
The keys above are included when the corresponding feature is enabled.

**Current status:** ✅ Matches as long as firmware uses the exact key names above.
Verify: `web_wifi_setup`, `ota_updates`, `remote_keyboard_input`, `remote_open_book`, `remote_page_turn`, `user_fonts`, `todo_planner`.

`GET /api/features` is a supported alias for `GET /api/plugins` and returns the
same JSON object.

---

## 7. GET /api/settings

**Android DTO** (`SettingDto` in `WifiTransport.kt`):
```json
[
  {
    "key":      "sleepScreen",
    "type":     "toggle | enum | value | string",
    "value":    1,
    "options":  ["opt1", "opt2"],
    "min":      0.0,
    "max":      100.0,
    "step":     1.0,
    "category": "Display",
    "hasValue": true
  }
]
```

**Current status:** ✅ Firmware returns this format from `handleGetSettings()`.

---

## 8. POST /api/settings

**Android sends:** JSON body `{"key": value, ...}` (flat map, only changed keys).

**Current status:** ✅ Firmware handles this in `handlePostSettings()`.

---

## 9. GET /api/cover?path=<encoded-path>

**Android:** expects raw image bytes (any format). Uses it as a `Bitmap`.

**Current status:** ✅ Firmware returns BMP bytes from cached cover path.

---

## 10. GET /download?path=<encoded-path>

**Android:** expects raw file bytes as the response body.

**Current status:** ✅ Implemented in `handleDownload()`.

---

## 11. POST /mkdir, /rename, /move, /delete

**Android sends:**

`/mkdir`
```
name=<name>&path=<parent-path>  (form-encoded)
```

`/rename`
```json
{"from": "/old/path", "to": "/new/path"}
```

`/move`
```json
{"from": "/src/path", "to": "/dst/dir"}
```

`/delete`
```
paths=<json-encoded-array>  (form field, value is a JSON string)
```

**Current status:** ✅ All four implemented on firmware.
- `/mkdir` and `/delete` remain form-based as above.
- `/rename` and `/move` now accept both:
  - existing web UI form contract (`path` + `name`, `path` + `dest`)
  - JSON body contract (`from` + `to`)

---

## 12. WebSocket upload (port 81)

**Android:** `WifiTransport.uploadFile()` connects to `ws://host:81`.

**Protocol:**
1. Client → `START:<filename>:<totalBytes>:<destPath>` (text)
2. Server → `READY` (text)
3. Client → binary chunks (4 KB each)
4. Server → `PROGRESS:<receivedBytes>:<totalBytes>` (text, every 64 KB)
5. Server → `DONE` (text) on completion
6. Server → `ERROR:<message>` (text) on failure

**Current status:** ✅ Firmware WebSocket server on port 81 implements this protocol.

---

## 13. GET /api/wifi/scan, POST /api/wifi/connect, POST /api/wifi/forget

**Android sends for connect:**
```json
{"ssid": "MyNetwork", "password": "secret"}
```

**Android sends for forget:**
```json
{"ssid": "MyNetwork"}
```

**Scan response:**
```json
[
  {
    "ssid":      "MyNetwork",
    "rssi":      -60,
    "encrypted": true,
    "saved":     false,
    "secured":   true,
    "connected": false
  }
]
```

**Current status:** ✅ All three implemented (gated on `WebWifiSetupApi` feature flag).
`secured` aliases `encrypted`, and `connected` marks the active network match.

### GET /api/wifi/status

Returns the current Wi-Fi mode and connection state.

**Connected STA response:**
```json
{
  "connected": true,
  "mode": "STA",
  "ssid": "MyNetwork",
  "ip": "192.168.1.20",
  "rssi": -60
}
```

**Disconnected STA response:**
```json
{
  "connected": false,
  "mode": "STA",
  "status": "connecting | failed | no_ssid | disconnected"
}
```

**AP mode response:**
```json
{
  "connected": false,
  "mode": "AP",
  "ssid": "crosspoint-ABCD"
}
```

**Current status:** ✅ Implemented (same `web_wifi_setup` gate as scan/connect/forget).

---

## 14. POST /api/ota/check + GET /api/ota/check

**POST** starts the check; **GET** polls the result.

**GET response:**
```json
{
  "currentVersion":  "1.0.0",
  "status":         "idle | checking | done | error",
  "available":      false,
  "latestVersion":  "1.2.0",
  "latest_version": "1.2.0",
  "errorCode":      0,
  "error_code":     0,
  "message":        ""
}
```

**Current status:** ✅ Implemented (gated on `OtaApi` feature flag). Firmware
returns both camelCase and snake_case fields for compatibility.

---

## 15. POST /api/user-fonts/upload + POST /api/user-fonts/rescan

**Upload:** multipart form, field name `file`, `.cpf` font file.
**Rescan:** no body required.

**Current status:** ✅ Implemented (gated on `UserFontsApi` feature flag).

---

## 16. POST /api/todo/entry, GET /api/todo/today, POST /api/todo/today

All three endpoints are gated on the `todo_planner` feature flag / `TodoPlanner` capability.

### Storage format

Entries are appended to `/daily/YYYY-MM-DD.md` (or `.txt` if markdown is disabled):

| Entry type | Markdown enabled | Plain text (`.txt`) |
|------------|-----------------|---------------------|
| Todo (unchecked) | `- [ ] text` | `- [ ] text` |
| Todo (checked) | `- [x] text` | `- [x] text` |
| Agenda entry | `> text` | `text` (plain line) |

When markdown is enabled the `.md` file extension is used and agenda entries are stored as
blockquotes (`> text`), which renders as a visually distinct non-checkbox line in markdown viewers
and on the e-ink display. Without markdown support, agenda entries are stored as plain lines.

Calendar events sent from the Android app use the format `[MM-dd HH:mm] Title`
(all-day events omit the time: `[MM-dd] Title`).

---

**POST /api/todo/entry** — Append a single entry to today's daily file.

Android sends (form-encoded):
```
text=<entry text>&type=<"todo"|"agenda">
```

- Max text length: 300 characters (enforced firmware-side).
- `type=agenda` stores the text as a plain line; `type=todo` stores it as `- [ ] text`.

Response: `{"ok":true}` or HTTP 400/404/503 on error.

---

**GET /api/todo/today** — Fetch today's parsed todo list.

Response:
```json
{
  "ok":   true,
  "date": "2024-01-15",
  "path": "/daily/2024-01-15.md",
  "items": [
    { "text": "Buy milk",        "type": "todo",   "checked": false, "isHeader": false },
    { "text": "[10:00] Standup", "type": "agenda", "checked": false, "isHeader": true  },
    { "text": "plain heading",   "type": "text",   "checked": false, "isHeader": true  }
  ]
}
```

`type` values:
- `"todo"` — checkbox item (`- [ ]` / `- [x]`)
- `"agenda"` — blockquote line (`> text`, written when markdown is enabled)
- `"text"` — plain line (free text, written when markdown is disabled or manually entered)

`isHeader` is `true` for both `"agenda"` and `"text"` items (backward compatibility).

---

**POST /api/todo/today** — Save the full reordered/edited list for today.

Android sends (JSON body):
```json
{"items": [{"text": "Buy milk", "isHeader": false, "checked": false}, ...]}
```

Both `isHeader` and `is_header` are accepted. Response: `{"ok":true}`.

---

**Current status:** ✅ All three HTTP endpoints implemented.
USB serial `todo_add` command also implemented (see §19).

---

## Additional HTTP control endpoints

### POST /api/open-book

Android sends:
```json
{"path": "/Books/book.epub"}
```

Success response:
```json
{"status":"opening"}
```

**Current status:** ✅ Implemented. Returns HTTP `202` on success, `400` for
invalid JSON or path, and `404` when the file does not exist.

### POST /api/remote/button

Android sends:
```json
{"button": "page_forward"}
```

Accepted button values:
- `page_forward`
- `next`
- `page_back`
- `prev`
- `previous`

Success response:
```json
{"status":"ok"}
```

**Current status:** ✅ Implemented. Returns HTTP `202` on success and `400` for
unknown button values.

---

## 18. BLE WiFi provisioning

**Service UUID:** `41cb0001-b8f4-4e4a-9f49-ecb9d6fd4b90`
**Characteristic UUID:** `41cb0002-b8f4-4e4a-9f49-ecb9d6fd4b90`

**Android sends** (write to characteristic, JSON format):
```json
{"ssid": "MyNetwork", "password": "secret"}
```

**Current status:** ✅ Firmware `BleWifiProvisioner` implements this service.

**Note on legacy UUIDs:** The Android app also recognises an older UUID pair
(`CCF00001-A1A2-B3B4-C5C6-D7D8E9F0A1B2` / `CCF00002-...`) for backward compatibility with
pre-release firmware builds. The fork-drift firmware only advertises the `41cb…` UUIDs — no
firmware change needed.

---

## 19. USB serial JSON-RPC

**Android:** `UsbTransport` at 115200 baud, 8N1. Each message is a single JSON
object terminated by `\n`. The firmware must reply with a single JSON object
terminated by `\n`.

**Command format:**
```json
{"cmd": "<command>", "arg": <argument>}
```

### Commands the Android app sends

| cmd | arg | Expected response |
|-----|-----|-------------------|
| `status` | — | `{"ok":true,"version":"...","protocolVersion":1,"freeHeap":...,"uptime":...,"openBook":"...","otaSelectedBundle":"...","otaInstalledBundle":"..."}` |
| `plugins` | — | `{"ok":true,"plugins":{"remote_open_book":true,"remote_page_turn":true,...}}` |
| `list` | `"/path"` | `{"ok":true,"files":[{"name":"...","path":"...","dir":false,"size":...,"modified":0}]}` |
| `download` | `"/path/file.epub"` | `{"ok":true,"data":"<base64>"}` |
| `upload_start` | `{"name":"file.epub","path":"/dir","size":1234}` | `{"ok":true}` |
| `upload_chunk` | `{"data":"<base64-chunk>"}` | `{"ok":true}` |
| `upload_done` | — | `{"ok":true}` |
| `delete` | `["/path/a", "/path/b"]` | `{"ok":true}` |
| `mkdir` | `"/new/dir"` | `{"ok":true}` |
| `rename` | `{"from":"/old","to":"/new"}` | `{"ok":true}` |
| `move` | `{"from":"/src","to":"/dst"}` | `{"ok":true}` |
| `settings_get` | — | `{"ok":true,"settings":{"key":value,...}}` |
| `settings_set` | `{"key":value,...}` | `{"ok":true}` |
| `recent` | — | `{"ok":true,"books":[{"path":"...","title":"...","author":"...","last_position":"1/12 8%","last_opened":0,"cover":"<base64-optional>"}]}` |
| `cover` | `"/path/file.epub"` | `{"ok":true,"data":"<base64>"}` or `{"ok":false}` |
| `wifi_connect` | `{"ssid":"...","password":"..."}` | `{"ok":true}` |
| `wifi_status` | — | `{"ok":true,"connected":bool,"ssid":"...","ip":"...","rssi":-60}` (when connected) or `{"ok":true,"connected":false,"status":"disconnected\|failed\|no_ssid\|connecting"}` |
| `open_book` | `"/path/file.epub"` | `{"ok":true}` |
| `remote_button` | `"page_forward"\|"page_back"` | `{"ok":true}` |
| `remote_keyboard_session_get` | — | `{"ok":true,"active":false}` or `{"ok":true,"active":true,"id":42,"title":"...","text":"...","maxLength":64,"isPassword":true,"claimedBy":"android"}` |
| `remote_keyboard_claim` | `{"id":42,"client":"android"}` | same payload shape as `remote_keyboard_session_get` for the claimed session |
| `remote_keyboard_submit` | `{"id":42,"text":"..."}` | `{"ok":true}` |
| `todo_add` | `{"text":"...","type":"todo"\|"agenda"}` | `{"ok":true}` |

**Error response** (for any command): `{"ok":false,"error":"<message>"}\n`

**Notes:**
- Upload is chunked: app sends base64 in 512-character string chunks, so each
  `upload_chunk` carries ≈384 bytes of actual data.
- The app reads with a 3 s timeout per window, up to 3 windows (9 s total) before
  giving up on a response.
- The `list` response must use `"dir"` not `"isDirectory"` (matches the HTTP contract).

**Current status:** ✅ Implemented on fork-drift. Full protocol implemented in
`src/UsbSerialProtocol.cpp` covering all commands in the table above.

---

## 20. Remote Keyboard Input

The Android app now supports a global remote keyboard handoff for any on-device text prompt.

**Capability discovery:**
- HTTP/WiFi transports read `remote_keyboard_input` from `GET /api/plugins`.
- USB serial transports read `remote_keyboard_input` from the `plugins` command response.

**HTTP endpoints:**
- `GET /api/remote-keyboard/session` returns the active session snapshot or `{"active":false}`.
- `POST /api/remote-keyboard/claim` accepts `{"id":<sessionId>,"client":"android"}` and returns the updated snapshot.
- `POST /api/remote-keyboard/submit` accepts `{"id":<sessionId>,"text":"..."}` and completes the session.

**Session fields used by Android:**
- `id`
- `title`
- `text`
- `maxLength`
- `isPassword`
- `claimedBy`

**USB serial commands:**
- `remote_keyboard_session_get`
- `remote_keyboard_claim`
- `remote_keyboard_submit`

**Runtime behavior on firmware:**
1. Opening the device keyboard starts a remote keyboard session when `remote_keyboard_input` is compiled in.
2. If the Android app is already connected, it can claim and answer the session immediately.
3. When the remote network session is ready, the device renders QR/browser fallback information and serves `/remote-input`.
4. If WiFi is unavailable, the device starts a hotspot automatically and exposes the same browser flow there; Android can still answer over USB in parallel.
5. Pressing on-device confirm switches back to the local keyboard; pressing back cancels the keyboard as usual.

**Current status:** ✅ Implemented on both WiFi and USB transports, including contract-test coverage via `WifiContractTransportTest`.

---

## Summary of remaining gaps (firmware work needed)

No known backend gaps remain for the Android HTTP contract documented here.

Items resolved on the firmware side (fork-drift):
- USB serial JSON-RPC — **implemented** in `src/UsbSerialProtocol.cpp`
- mDNS hostname — **implemented**; hostname is `crosspoint-{name}` or `crosspoint-{last4mac}`
- USB serial `status` now returns `otaSelectedBundle` and `otaInstalledBundle` — **fixed in `src/UsbSerialProtocol.cpp`**
- HTTP `handleOpenBook` and `handleRemoteButton` now gate on `remote_open_book` / `remote_page_turn` feature flags — **fixed in `src/network/CrossPointWebServer.cpp`**

Items previously listed as gaps that are now resolved on the Android side:
- `/api/files` format (bare array, `isDirectory` field, path construction) — **fixed in Android**
- `/api/status` camelCase field names (`wifiStatus`, `freeHeap`) — **fixed in Android**
