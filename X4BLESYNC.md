# BLE reading-progress sync

This branch adds an interoperable BLE transport for exchanging KOReader-style
reading positions with a compatible client. The CrossPoint firmware is the BLE
peripheral; client support is a separate release effort.

The intended rollout is deliberately staged:

1. land and release the CrossPoint transport and position bridge;
2. add compatible client support to KOReader;
3. support the same public protocol in other reading apps later.

There is no released companion app dependency in this patch. The protocol is
the integration boundary; client implementations should not depend on a
particular application or operating system.

## What we add

### 1. BLE reading-position sync — `src/ble_sync/`
The X4 is a BLE **peripheral**; the compatible client is the central. Positions cross as
MANIFEST → WANT → PROGRESS with newest-wins conflict resolution and a **persisted
clock floor** (the C3 has no RTC — deep-sleep resets `time()` to 0). Key pieces:
- `BleProgressBridge.{h,cpp}` — read/apply a book's position, reusing the exact
  KOReader-sync machinery (`ProgressMapper`, `progress.bin`, title/doc hashes).
- `BleSyncManager`, `BleSyncProtocol`, `BleSyncIndicator`, `BleClock`.
Protocol evolution and client work are tracked separately from this firmware
implementation.

### 2. Wi-Fi endpoints — `src/network/CrossPointWebServer.cpp`
Added to the existing HTTP file server so compatible clients can carry positions
and match books:
- **`GET /api/progress?path=<epub>`** → `{document, progress(xpath), percentage,
  timestamp, titleHash}` via `BleProgress::getForPath`. Loads epub metadata only
  — **no renderer, no re-pagination** → safe inside the web task. Returns 204 for
  an unread book. Lets a client seed a downloaded book's resume position.
- **`GET /api/manifest`** → `[{path, titleHash}]` via
  `BleProgress::pathTitleHashes` (cached metadata, no epub load). Lets a client
  match a book by title hash even when the filename differs.

**Not added on purpose: `POST /api/progress` (client→X4 position push).** Applying
a remote position (`BleProgress::applyRemote`) maps xpath→page by paginating the
book with a `GfxRenderer` — heavy and watchdog-risky in an HTTP handler. By
design the X4 pulls position from the BLE peer when it opens a book instead. Don't
add the POST without first solving pagination-in-the-web-task.

## Position storage (both channels speak this)
`/.crosspoint/epub_<hash>/progress.bin` — 6 bytes LE: spineIndex, pageNumber,
pageCount. `progress-time.bin` — 8-byte LE unix clock (0 = unclocked).
`hash = std::hash<std::string>(fullEpubPath)`.
Book identity for matching = `titleHashFromMeta(title, author)` = MD5 of
normalized `title\x1Fauthor`. Client implementations must produce the same bytes.

## Build & flash
```bash
~/.platformio/penv/bin/pio run -e default                                  # build
~/.platformio/penv/bin/pio run -e default -t upload --upload-port <PORT>   # flash
```
The X4 is an ESP32-C3 (`VID:PID=303A:1001`). **Confirm the port with
`esptool --port <PORT> chip-id` before flashing.** BLE and Wi-Fi can't run
together on the C3 (one radio) — the
X4 uses one at a time. Back up stock first; see the parent repo's
`docs/SAFE-FLASH.md`.
