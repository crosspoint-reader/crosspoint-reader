# Serial Transfer (USB-C file manager)

For boards whose USB-C port is a USB-to-serial bridge (reTerminal Sticky), the SD card cannot be
exported as a drive and WiFi may be unavailable. **File Transfer > Serial Transfer** lets a
computer manage the SD card over the USB cable.

## Use

1. On the reader: *File Transfer > Serial Transfer*. Keep the USB-C cable plugged in.
2. On the computer, open `tools/serial_bridge/crosspoint-serial.html` in **Chrome or Edge**
   (double-click works; Web Serial is allowed on `file://`).
3. Click **Choose serial port** and pick the reader's port (CH343 / USB-Serial). The page
   handshakes at 921600 baud (fallback 115200) and shows the regular CrossPoint file manager:
   browse, upload, download, create folder, rename, move, delete.
4. Press Back on the reader to leave the mode (logging resumes).

Regenerate the page after changing the web UI: `python tools/serial_bridge/build_page.py`.

The Home, Files, Settings and Fonts pages work; Plugins, WiFi and OPDS pages need WiFi.
Limits: folders cannot be moved/renamed (as in the web UI).
The reader's log output is muted while the mode is active.

## Protocol

Request: `A5 5A | cmd | id | len(u16 LE) | payload | crc32(u32 LE)`;
response: `A5 5A | status | id | len(u16 LE) | payload | crc32`. CRC32 (zlib) covers
cmd/status, id, length and payload. Max payload 4096. Stop-and-wait; the host retries on timeout;
a corrupt frame gets no reply.

The host picks a request `id` (1-255) per request and reuses it for that request's retries. The
reader echoes it, and the host drops any reply carrying another id, so a late answer to an
earlier attempt is never taken for the answer to the next request. A repeated WRITE_END with the
id of the one that was just committed returns the original result instead of failing with
"bad state", so a lost acknowledgement does not turn a successful upload into an error.

The host runs each multi-frame operation (open/read/close, open/write/end, API + body reads) as
one unit, because the reader has a single read handle, write handle and API buffer.

BAUD: the reader switches speed after its reply. If it receives no valid frame at the new speed
within 4 s it returns to 115200 by itself, so the host can always reconnect at 115200.

Upload safety: data goes to a staging file (`<path>.part`, or `.part1`... if that name is already
in use, which is never deleted). WRITE_END moves an existing target aside as `<path>.bak`,
renames the staged file into place and only then deletes the old file; if the rename fails the
original is put back. CLOSE abandons an unfinished upload.

| cmd | name | payload -> response |
|---|---|---|
| 1 | HELLO | - -> `CPXFER1\0` |
| 2 | LIST | u32 start index + path -> u8 more, entries (u8 flags bit0=dir, u32 size, u8 len, name) |
| 3 | READ_OPEN | path -> u32 size |
| 4 | READ | u32 offset, u16 len -> data |
| 5 | CLOSE | - |
| 6 | WRITE_OPEN | u8 overwrite + path (writes to `<path>.part`) |
| 7 | WRITE | u32 offset + data (a repeated chunk is accepted) |
| 8 | WRITE_END | - (renames `.part` to the target) |
| 9 | MKDIR | path |
| 10 | RENAME | old NUL new |
| 11 | DELETE | path (empty folders only) |
| 12 | BAUD | u32 baud (applied after the reply) |
| 13 | STATUS | - -> JSON |
| 14 | API | u8 method (0 GET, 1 POST) + uri [+ NUL + body] -> u16 HTTP status, u32 body length (`/api/settings`, `/api/fonts`, `/api/fonts/delete`) |
| 15 | API_READ | u32 offset, u16 len -> bytes of the last API body |

Font upload: WRITE_OPEN on `/@font/<family>/<file>` installs the file in the font folder (same validation as the web uploader).

Status: 0 OK, 1 bad cmd, 2 bad arg, 3 not found, 4 exists/not empty, 5 protected, 6 I/O, 7 state.
Protected/credential paths and `.`/`..` segments are refused as in the web server.

## Code shared with the web server

Serial mode has no HTTP server, so the Settings and Fonts pages cannot call the web handlers
directly. In `src/network/CrossPointWebServer.cpp` each of those handlers was split in two: the
logic moved into a static function (declared in `CrossPointWebServer.h`) and the handler keeps only
the HTTP part (`server->arg`, `server->send`, `ChunkedReply`). `SerialTransferActivity` calls the
same static functions through the `API` command, so both transports behave the same.

| HTTP handler (unchanged behaviour) | Shared function |
|---|---|
| `handleGetSettings` | `writeSettingsJson(Print&, keys)` |
| `handlePostSettings` | `applySettingsJson(body, error)` (returns -1 on error) |
| `handleFontList` | `writeFontListJson(Print&)` (returns false on out-of-memory) |
| `handleFontDelete` | `deleteFontFamily(name)` |

Status codes and messages are unchanged on the HTTP side. On a font-list out-of-memory error the
JSON is still left unterminated so the page reports a failed load.
