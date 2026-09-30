# Webserver Endpoints

This document describes the HTTP, WebSocket, WebDAV, and discovery endpoints
available while CrossPoint Reader is in File Transfer or Calibre Wireless mode.

- HTTP server: port 80
- WebSocket upload server: port 81
- UDP discovery listener: port 8134
- WebDAV: port 80, handled by the same HTTP server

Examples use `crosspoint.local`. If mDNS does not resolve on your network, use
the IP address shown on the device screen.

## HTTP Pages

| Method | Path | Purpose |
|--------|------|---------|
| `GET` | `/` | Home/status page |
| `GET` | `/files` | File manager page |
| `GET` | `/settings` | Web settings page |
| `GET` | `/fonts` | SD-card font manager page |
| `GET` | `/js/jszip.min.js` | JavaScript asset used by the file manager |

## Device Status

### `GET /api/status`

```bash
curl http://crosspoint.local/api/status
```

Response:

```json
{
  "version": "1.0.0",
  "ip": "192.168.1.100",
  "mode": "STA",
  "rssi": -45,
  "freeHeap": 123456,
  "uptime": 3600,
  "device": "X4"
}
```

| Field | Type | Description |
|-------|------|-------------|
| `version` | string | Firmware version |
| `ip` | string | Device IP address |
| `mode` | string | `"STA"` for joined Wi-Fi or `"AP"` for hotspot mode |
| `rssi` | number | Wi-Fi RSSI in dBm; `0` in AP mode |
| `freeHeap` | number | Free heap in bytes |
| `uptime` | number | Seconds since boot |
| `hardwareMac` | string | Factory MAC, stable across Wi-Fi modes; omitted if unavailable |
| `device` | string | `"X3"` or `"X4"` hardware detection |

## File Management

### `GET /api/files`

Lists files and folders under a directory.

```bash
curl "http://crosspoint.local/api/files?path=/Books"
```

Query parameters:

| Parameter | Required | Default | Description |
|-----------|----------|---------|-------------|
| `path` | No | `/` | Directory to list |

Response:

```json
[
  {"name":"MyBook.epub","size":1234567,"isDirectory":false,"isEpub":true},
  {"name":"Notes","size":0,"isDirectory":true,"isEpub":false}
]
```

Hidden dotfiles are omitted unless the device setting `showHiddenFiles` is
enabled. `System Volume Information` and `XTCache` are always hidden/protected.

### `GET /download`

Downloads a file from the SD card.

```bash
curl -OJ "http://crosspoint.local/download?path=/Books/MyBook.epub"
```

Query parameters:

| Parameter | Required | Description |
|-----------|----------|-------------|
| `path` | Yes | File path to download |

Protected dotfiles, `System Volume Information`, and `XTCache` cannot be
downloaded. EPUB files are served as `application/epub+zip`; other files use
`application/octet-stream`.

### `POST /upload`

Uploads a file with HTTP multipart form data.

```bash
curl -X POST -F "file=@mybook.epub" "http://crosspoint.local/upload?path=/Books"
```

Query parameters:

| Parameter | Required | Default | Description |
|-----------|----------|---------|-------------|
| `path` | No | `/` | Destination directory |

Successful response:

```text
File uploaded successfully: mybook.epub
```

Notes:

- Existing files with the same name are overwritten.
- EPUB cache data for the uploaded path is cleared after a successful upload.
- HTTP upload uses a 4 KB write buffer before flushing to the SD card.

### `POST /mkdir`

Creates a folder.

```bash
curl -X POST -d "name=NewFolder&path=/" http://crosspoint.local/mkdir
```

Form parameters:

| Parameter | Required | Default | Description |
|-----------|----------|---------|-------------|
| `name` | Yes | - | New folder name |
| `path` | No | `/` | Parent folder |

### `POST /rename`

Renames a file.

```bash
curl -X POST -d "path=/Books/old.epub&name=new.epub" http://crosspoint.local/rename
```

Form parameters:

| Parameter | Required | Description |
|-----------|----------|-------------|
| `path` | Yes | Existing file path |
| `name` | Yes | New file name, not a path |

Only files can be renamed through this endpoint. The old EPUB cache path is
cleared before the rename.

### `POST /move`

Moves a file into an existing folder.

```bash
curl -X POST -d "path=/Books/mybook.epub&dest=/Read" http://crosspoint.local/move
```

Form parameters:

| Parameter | Required | Description |
|-----------|----------|-------------|
| `path` | Yes | Existing file path |
| `dest` | Yes | Existing destination folder |

Only files can be moved through this endpoint. The old EPUB cache path is
cleared before the move.

### `POST /delete`

Deletes one or more files or empty folders.

```bash
curl -X POST -d "path=/Books/mybook.epub" http://crosspoint.local/delete
curl -X POST -d 'paths=["/Books/old.epub","/OldFolder"]' http://crosspoint.local/delete
```

Form parameters:

| Parameter | Required | Description |
|-----------|----------|-------------|
| `path` | Yes, unless `paths` is provided | Single path to delete |
| `paths` | Yes, unless `path` is provided | JSON array of paths to delete |

Protected items cannot be deleted. Non-empty folders are rejected. EPUB cache
data for deleted files is cleared.

## Settings API

### `GET /api/settings`

Returns a streamed JSON array of editable settings. Each item contains common
fields plus type-specific fields.

```bash
curl http://crosspoint.local/api/settings
```

Example item:

```json
{
  "key": "fontSize",
  "name": "Reader Font Size",
  "category": "Reader",
  "type": "enum",
  "value": 1,
  "options": ["12 pt", "14 pt", "16 pt", "18 pt"]
}
```

`value` is always an index into `options`, never the option's text. The
`fontSize` options depend on the selected family. A `.cpfont` family installed
at 10/12/14 pt offers those three sizes. TTF/OTF/TTC families offer the
standard 12/14/16/18 pt sizes. The `fontFamily` and `dictionaryName` options
also depend on the SD card contents.

Types:

| Type | Extra fields |
|------|--------------|
| `toggle` | `value` (`0` or `1`) |
| `enum` | `value`, `options` |
| `value` | `value`, `min`, `max`, `step` |
| `string` | `value` |

The font-family setting includes SD-card font families when they are installed.

### `POST /api/settings`

Applies a partial settings update from a JSON object.

```bash
curl -X POST \
  -H "Content-Type: application/json" \
  -d '{"fontSize":2,"showHiddenFiles":1}' \
  http://crosspoint.local/api/settings
```

Successful response:

```text
Applied 2 setting(s)
```

## Font Management API

### `GET /api/fonts`

Lists installed SD-card font families. On devices that load TTF/OTF/TTC files,
these families appear with `sizes: [0]`. The `0` means that the font file has
no fixed point size; the reader offers 12, 14, 16, and 18 pt.

```bash
curl http://crosspoint.local/api/fonts
```

Response:

```json
{
  "maxFamilies": 128,
  "families": [
    {
      "name": "Literata",
      "sizes": [12, 14, 16, 18],
      "files": [
        {"name": "Literata_12.cpfont", "size": 123456}
      ]
    }
  ]
}
```

### `POST /api/fonts/upload`

Uploads one `.cpfont` file into a family folder.

```bash
curl -X POST \
  -F "family=Literata" \
  -F "file=@Literata_12.cpfont" \
  http://crosspoint.local/api/fonts/upload
```

The handler validates the family name, `.cpfont` filename, and `CPFONT` magic
bytes before accepting the file.

Successful response:

```json
{"ok":true}
```

### `POST /api/fonts/delete`

Deletes an installed font family.

This endpoint removes family subfolders. To remove a loose TTF/OTF/TTC file
from a font root, delete the file from the SD card instead.

```bash
curl -X POST \
  -H "Content-Type: application/json" \
  -d '{"family":"Literata"}' \
  http://crosspoint.local/api/fonts/delete
```

Successful response:

```json
{"ok":true}
```

## OPDS Server API

### `GET /api/opds`

Lists saved OPDS servers. Passwords are never returned.

```bash
curl http://crosspoint.local/api/opds
```

Response:

```json
[
  {
    "index": 0,
    "name": "My Catalog",
    "url": "http://calibre.local:8080/opds",
    "username": "reader",
    "hasPassword": true
  }
]
```

### `POST /api/opds`

Adds or updates an OPDS server. Include `index` to update an existing entry.
If `password` is omitted during an update, the existing password is preserved.

```bash
curl -X POST \
  -H "Content-Type: application/json" \
  -d '{"name":"My Catalog","url":"http://calibre.local:8080/opds","username":"reader","password":"secret"}' \
  http://crosspoint.local/api/opds
```

### `POST /api/opds/delete`

Deletes an OPDS server by index.

```bash
curl -X POST \
  -H "Content-Type: application/json" \
  -d '{"index":0}' \
  http://crosspoint.local/api/opds/delete
```

## Wi-Fi Credential API

### `GET /api/wifi`

Lists saved Wi-Fi networks. Passwords are never returned.

```bash
curl http://crosspoint.local/api/wifi
```

Response:

```json
[
  {
    "index": 0,
    "ssid": "HomeWiFi",
    "hasPassword": true,
    "isLastConnected": true
  }
]
```

### `POST /api/wifi`

Adds or updates a saved Wi-Fi network. Include `index` to update an existing
entry. If `password` is omitted during an update, the existing password is
preserved.

```bash
curl -X POST \
  -H "Content-Type: application/json" \
  -d '{"ssid":"HomeWiFi","password":"secret"}' \
  http://crosspoint.local/api/wifi
```

### `POST /api/wifi/delete`

Deletes a saved Wi-Fi network by index.

```bash
curl -X POST \
  -H "Content-Type: application/json" \
  -d '{"index":0}' \
  http://crosspoint.local/api/wifi/delete
```

## WebSocket Upload

### Port 81

The WebSocket path is used for fast binary uploads from the file manager and
Calibre plugin workflows.

Connection:

```text
ws://crosspoint.local:81/
```

Protocol:

1. Client sends text: `START:<filename>:<size>:<path>`
2. Server replies `READY`
3. Client sends binary chunks
4. Server sends `PROGRESS:<received>:<total>` every 64 KB or at completion
5. Server sends `DONE` when complete or `ERROR:<message>` on failure

Example session:

```text
Client -> START:mybook.epub:1234567:/Books
Server -> READY
Client -> [binary chunk]
Server -> PROGRESS:65536:1234567
...
Server -> DONE
```

Error messages include:

| Message | Cause |
|---------|-------|
| `ERROR:Upload already in progress` | A second upload was started before the first completed |
| `ERROR:Invalid START format` | Malformed START message or invalid size token |
| `ERROR:Failed to create file` | Destination file could not be opened |
| `ERROR:No upload in progress` | Binary data arrived without a matching START |
| `ERROR:Upload overflow` | Client sent more bytes than declared |
| `ERROR:Write failed - disk full?` | SD write failed |

Incomplete WebSocket uploads are deleted on disconnect or error.

## WebDAV

The same HTTP server registers a WebDAV-compatible handler for file manager clients.

Supported methods:

```text
OPTIONS, GET, HEAD, PUT, DELETE, PROPFIND, MKCOL, MOVE, COPY, LOCK, UNLOCK
```

Notes:

- `PUT` writes to a temporary `.davtmp` file first, then renames it into place.
- Protected paths are rejected.
- `LOCK` and `UNLOCK` are accepted for client compatibility only. The server
  does not implement full WebDAV Class 2 locking semantics such as persistent
  locks or lock discovery.

## UDP Discovery

The server listens on UDP port `8134`. When it receives the text payload
`hello`, it replies to the sender with:

```text
crosspoint (on <hostname>);81
```

The final field is the WebSocket upload port.

## Network Modes

### Station Mode (STA)

- Device joins an existing 2.4 GHz Wi-Fi network.
- `crosspoint.local` is advertised with mDNS when available.
- `/api/status` returns `"mode": "STA"` and RSSI in dBm.

### Access Point Mode (AP)

- Device creates an open hotspot named `CrossPoint-Reader`.
- The device shows a Wi-Fi QR code and URL QR code.
- The fallback IP is typically `192.168.4.1`.
- `/api/status` returns `"mode": "AP"` and `"rssi": 0`.

### Calibre Wireless

Calibre Wireless starts the same web server in STA mode and displays setup
instructions plus WebSocket upload progress on the device screen.

## Plugin API

Device capabilities for browser-side SD plugins (`plugin.js`). Plugins normally
reach these through the `PluginHost` wrappers (`api.relay`, `api.crypto`,
`api.fetchToSd`, `api.writeFile`); the raw contracts are below. Outbound TLS is
encrypted but the peer is not verified (the transport ships no CA bundle).

### `GET /api/plugins`

Installed plugins that ship a `plugin.js`:
`[{"name":"<folder>","title":"<title>","mount":"settings"}, ...]`. `title` and
`mount` come from the plugin's `manifest.json` when present.

### `GET /plugin?name=<plugin>&file=<file>`

Serves one file from the plugin's folder (first root holding that name). Both
parameters must be single path components (no `/`, `\`, or `..`); 404 when
the plugin, file, or a directory is requested.

### `POST /api/relay`

Makes an outbound HTTP(S) request for a plugin (any method; browsers can't, due
to CORS). Body: `{"plugin":"<name>","method":"GET","url":"https://...","headers":{...},"body":"..."}`.

- **Success:** `200` with the upstream body verbatim. The upstream status is
  in the `X-Relay-Status` response header, and its headers in `X-Relay-Headers`
  as JSON `[["name","value"], ...]` in receive order, duplicates kept (every
  `Set-Cookie`). `api.relay()` resolves to `{status, headers, body}`.
- **Redirects** are not followed: a 3xx comes back with its `location` header.
- **Limits:** the upstream body is capped at 32KB; larger payloads belong on
  `/api/fetch`.
- **Errors:** `400 {"error":"missing plugin/url"}`; `502 {"error":"relay failed; ..."}`
  for a transport failure, a truncated body, a body over the cap, or low memory
  (the device log names which).

### `POST /api/crypto`

Stateless wolfSSL primitives. Body `{"op":"<op>", ...}`; binary fields are
base64 in both directions, each at most 64KB encoded. Replies are `200` with
the result fields, or `200 {"error":"..."}`.

| `op` | Request fields | Result |
| --- | --- | --- |
| `random` | `len` (default 16, max 4096) | `data` |
| `sha1` | `data` | `data` (20 bytes) |
| `aesenc` | `key`, `iv` (16 bytes each), `data` | `data`: AES-128-CBC with PKCS#7 padding |
| `aesdec` | `key`, `iv`, `data` (block-aligned) | `data`: AES-128-CBC, padding left in place |
| `keygen` | none | `public` (SPKI DER), `private` (PKCS#8 DER): RSA key pair |
| `pubencrypt` | `cert` (X.509 DER), `data` | `data`: RSAES-PKCS1-v1_5 to the certificate's key |
| `sign` | `private` (PKCS#8 DER), `hash` (20 bytes) | `data` (128 bytes): raw RSA signature |
| `pkcs12` | `data` (the bundle), `password` | `key`, `cert` (DER) |

### `POST /api/fetch`

Downloads a URL straight to SD, so a large body never passes through the
browser. Body: `{"plugin":"<name>","url":"...","dest":"/abs/path","headers":{...},"offset":0,"maxBytes":0}`.

- `dest` must be absolute with no `..`; missing parent folders are created.
- The transfer is staged in `<dest>.part` and replaces `dest` only when the
  whole body has arrived, so a failed update keeps the previous file.
- Up to 5 redirects are followed. `headers` (typically the plugin's
  `Authorization`) are sent only while the target keeps the starting URL's
  scheme, host, and port; a redirect elsewhere, including https→http, gets none.
- A body cut short mid-transfer resumes on its own with a Range request.
- **Segments:** `maxBytes` (at most 4MB) ends the request after that many
  bytes with `complete:false`; send the next request with `offset` set to the
  returned `bytes` to continue the same `.part`. `api.fetchToSd()` does this
  loop, keeping each browser request short.
- A long transfer answers early with `200` chunked and sends whitespace every
  5s to keep the browser connection open; the JSON result follows at the end.
- **Result:** `{"status":200,"bytes":N,"complete":true,"total":N}` (`total`
  when the server reported a size). `error` is set to `transport failure`,
  `http status`, or `sd write failed` when the file was not installed.
- **Errors:** `400 {"error":"bad url/dest"}`; `409 {"error":"offset mismatch","bytes":N}`
  when `offset` does not match the `.part` size; `502 {"error":"sd write failed" |
  "range unsupported" | "download truncated","bytes":N,"complete":false}`.

### `POST /api/plugin-fs?plugin=<name>&path=<abs path>`

Writes one small file to SD. The content is sent as a multipart file part
(`api.writeFile()` builds it), so binary data, including NUL bytes, arrives
intact. It streams to `<path>.tmp` and replaces `path` only after a complete,
non-empty body.

- **Success:** `200 {"ok":true,"bytes":N}`.
- **Errors:** `400` (`bad path`, `empty body`, `missing file part`,
  `upload aborted`); `413 {"error":"too large"}` over 256KB; `500
  {"error":"cannot write" | "sd write failed"}`. The previous file is kept on
  every error.

## Plugin Job Queue

External systems trigger SD-plugin actions without the web UI. The firmware
stores opaque `{plugin, action, args}` blobs (fixed 6-slot pool; args/result
< 192 bytes JSON); an open page hosting the plugin executes them. See
`docs/sd-plugins.md` for the full contract.

### `POST /api/plugin-jobs`

```bash
curl -X POST http://crosspoint.local/api/plugin-jobs \
  -d '{"plugin":"<name>","action":"<action>","args":{"path":"/Books/somefile"}}'
# -> {"id":3}         (503 {"error":"job queue full"} when all slots busy)
```

### `GET /api/plugin-jobs/claim?plugin=<name>`

Executor-side (used by the plugin host page): returns the next pending job
`{"id":3,"claim":7,"action":"<action>","args":{...}}` and marks it running, or `{"id":0}`.
A running job whose lease expires returns to pending and gets a new `claim` when re-claimed.

### `POST /api/plugin-jobs/complete`

Executor-side: `{"id":3,"claim":7,"ok":true,"result":{...}}` -> `{"ok":true}`. The `claim`
must match the one from the claim response; a stale claim (the lease expired and another
executor re-claimed the job) gets `409 {"error":"stale claim"}` and changes nothing.

### `GET /api/plugin-jobs/status?id=<n>`

```bash
curl "http://crosspoint.local/api/plugin-jobs/status?id=3"
# -> {"id":3,"state":"done","result":{"title":"...","dest":"/Books/Book.epub"}}
```

States: `pending`, `running`, `done`, `error`, `unknown` (slot recycled —
poll promptly after completion).

### `GET /plugins-run`

Headless executor page: loads every plugin with its UI hidden so registered
actions run. Keep it open (browser tab or embedded webview) while jobs are
queued; jobs enqueued with no page open stay pending until one opens.
