# X4-BLESYNC fork notes

This is a **fork of CrossPoint** (the community e-reader firmware for the Xteink
X4 / ESP32-C3) carrying the firmware half of the **x4-blesync** project — sync
with a companion iPhone app, **X4 Books**.

Upstream docs (`README.md`, `CLAUDE.md`) are CrossPoint's own — unchanged. This
file describes only what x4-blesync **adds on top**. The cross-device picture,
protocol, and the iOS app live in the parent project:
github.com/ComicBit/x4-blesync.

> ⚠️ This checkout's `origin` points at **upstream** crosspoint-reader (which we
> can't push to) and it's on a **detached HEAD**. To publish these changes, add
> your own fork remote and a branch first — don't `git push origin`.

## What we add

### 1. BLE reading-position sync — `src/ble_sync/`
The X4 is a BLE **peripheral**; the phone is the central. Positions cross as
MANIFEST → WANT → PROGRESS with newest-wins conflict resolution and a **persisted
clock floor** (the C3 has no RTC — deep-sleep resets `time()` to 0). Key pieces:
- `BleProgressBridge.{h,cpp}` — read/apply a book's position, reusing the exact
  KOReader-sync machinery (`ProgressMapper`, `progress.bin`, title/doc hashes).
- `BleSyncManager`, `BleSyncProtocol`, `BleSyncIndicator`, `BleClock`.
Protocol evolution: parent repo `docs/PROTOCOL-v1..v3.md` (v3 = current).

### 2. Wi-Fi endpoints — `src/network/CrossPointWebServer.cpp`
Added to the existing HTTP file server so the iOS Wi-Fi sync can carry positions
and match books:
- **`GET /api/progress?path=<epub>`** → `{document, progress(xpath), percentage,
  timestamp, titleHash}` via `BleProgress::getForPath`. Loads epub metadata only
  — **no renderer, no re-pagination** → safe inside the web task. Returns 204 for
  an unread book. Lets the phone seed a downloaded book's resume position.
- **`GET /api/manifest`** → `[{path, titleHash}]` via
  `BleProgress::pathTitleHashes` (cached metadata, no epub load). Lets the phone
  match a book by title hash even when the filename differs.

**Not added on purpose: `POST /api/progress` (phone→X4 position push).** Applying
a remote position (`BleProgress::applyRemote`) maps xpath→page by paginating the
book with a `GfxRenderer` — heavy and watchdog-risky in an HTTP handler. By
design the X4 pulls position from the phone when it opens a book instead. Don't
add the POST without first solving pagination-in-the-web-task.

## Position storage (both channels speak this)
`/.crosspoint/epub_<hash>/progress.bin` — 6 bytes LE: spineIndex, pageNumber,
pageCount. `progress-time.bin` — 8-byte LE unix clock (0 = unclocked).
`hash = std::hash<std::string>(fullEpubPath)`.
Book identity for matching = `titleHashFromMeta(title, author)` = MD5 of
normalized `title\x1Fauthor`, byte-identical to the iOS `TitleHash`.

## Build & flash
```bash
~/.platformio/penv/bin/pio run -e default                                  # build
~/.platformio/penv/bin/pio run -e default -t upload --upload-port <PORT>   # flash
```
The X4 is an ESP32-C3 (`VID:PID=303A:1001`). The iPhone also shows as
`/dev/cu.usbmodem*` — **confirm the port with `esptool --port <PORT> chip-id`
before flashing.** BLE and Wi-Fi can't run together on the C3 (one radio) — the
X4 uses one at a time. Back up stock first; see the parent repo's
`docs/SAFE-FLASH.md`.
