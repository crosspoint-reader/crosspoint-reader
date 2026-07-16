# BLE reading-progress sync protocol

This document is the public interoperability contract for CrossPoint's
Bluetooth reading-progress sync. It is intended to contain enough information
for KOReader or any other reading system to implement a compatible client
without depending on a particular application, SDK, or operating system.

CrossPoint implements the reader/peripheral side. Compatible KOReader client
support is a separate release effort. Other clients can implement the same
contract later.

## Status and scope

- Wire version: `protocolVersion = 1`.
- Device role: CrossPoint is the BLE peripheral/GATT server.
- Client role: the syncing system is the BLE central/GATT client.
- Data synchronized: reading position and its last-change time.
- Reconciliation scope: up to the 20 most-recent CrossPoint books per window.
- Conflict policy: strictly newer `updatedAt` wins.
- Feature state: opt-in. With BLE sync disabled, CrossPoint does not start the
  radio or add sync writes to the reading path.

Bookmarks, highlights, annotations, files, and account credentials are outside
this protocol.

## Purpose and user experience

The purpose of this transport is to keep reading position synchronized between
an X4 and a nearby compatible reader—for example KOReader on iOS or Android,
another e-reader, or a future reading app—without Wi-Fi, internet access, a
cloud service, or a shared KOReader Sync server.

The user can stop reading on one system and continue on the other. During a
sync, each side announces the books it knows and when each position last
changed. Only differing positions are transferred, and the newest edit wins.
Moving backward in a book is still a new edit, so it synchronizes correctly.

CrossPoint does not keep BLE running continuously. Instead, it opens short,
low-cost rendezvous windows at the moments a reading position is most useful:

- after boot or wake, to reconcile recent books;
- before opening a book when the last successful sync is stale or absent, so
  the book can open at the peer's newer position;
- after closing a book, to publish the position just reached;
- when BLE sync is enabled or Pair is selected, for an immediate reconcile;
- immediately before sleep, to make a final best-effort push.

A compatible central scans in the background or foreground, reconnects during
one of these windows, exchanges only compact JSON deltas, and disconnects after
the libraries converge. CrossPoint then deinitializes BLE so the radio and heap
are released. In normal use this gives the effect of the X4 and companion reader
staying synchronized at reading boundaries while avoiding an always-on radio
and requiring no network infrastructure.

## Security model

The current implementation does not request BLE bonding, authenticated pairing,
or application-layer encryption. `PAIR_HELLO` is a logical peer-identification
message, not a security boundary. The displayed "Pair" action starts a discovery
window; it does not establish a durable cryptographic relationship.

Implementers must therefore treat the link as unauthenticated. Do not add secret
tokens or private account credentials to payloads. A future secured revision
must extend the protocol explicitly instead of silently changing version 1.

## GATT service

CrossPoint advertises the local name `X4 Sync` and the service UUID below.
Clients should discover by service UUID, not by local name.

| Purpose | UUID | Properties | Direction |
|---|---|---|---|
| Reading sync service | `7ea41000-b5a3-4f21-9c7d-1a2b3c4d5e6f` | Service | — |
| Capabilities | `7ea41001-b5a3-4f21-9c7d-1a2b3c4d5e6f` | Read | X4 → client |
| X4 messages | `7ea41002-b5a3-4f21-9c7d-1a2b3c4d5e6f` | Notify | X4 → client |
| Client messages | `7ea41003-b5a3-4f21-9c7d-1a2b3c4d5e6f` | Write with response | client → X4 |
| Debug state | `7ea41004-b5a3-4f21-9c7d-1a2b3c4d5e6f` | Read | X4 → client |

CrossPoint requests ATT MTU 517. Every notification or write contains exactly
one complete UTF-8 JSON object. Version 1 defines no byte-stream chunk framing,
so each serialized object must fit the negotiated ATT payload (`MTU - 3`). A
client should request the largest MTU it supports and keep messages below that
limit. CrossPoint paginates manifests to four rows per notification.

Subscribe to the X4 message characteristic before writing the first client
message. CrossPoint resends its manifest every 700 ms until it receives a client
manifest, which closes the usual subscribe race.

## Read characteristics

### Capabilities

Example value from the capabilities characteristic:

```json
{
  "protocol": 1,
  "device": "xteink-x4",
  "firmware": "crosspoint-ble-v1",
  "supports": ["progress", "manifest", "want", "peer_clock"]
}
```

A client must not start a version-1 exchange if `protocol` is greater than the
highest version it understands. Unknown `supports` values are ignored.

### Debug sync state

The sync-state characteristic is diagnostic and is not needed to reconcile:

```json
{
  "paired": true,
  "lastPeerId": "koreader-7f3a",
  "lastPhoneId": "koreader-7f3a",
  "lastEvent": "PROGRESS",
  "lastAck": "2026-07-16T18:40:12Z"
}
```

`lastPhoneId` is retained as a legacy field. New clients should use
`lastPeerId`. The state is not proof of identity or authorization.

## Common message envelope

Messages on both data characteristics are JSON objects. Real sync messages use
this common envelope:

```json
{
  "protocolVersion": 1,
  "messageId": "846c40c4-b268-4c54-ae4b-306fe1426c9d",
  "type": "MANIFEST",
  "source": "koreader",
  "deviceId": "koreader-7f3a",
  "timestamp": "2026-07-16T18:40:10Z"
}
```

| Field | Required | Meaning |
|---|---|---|
| `protocolVersion` | yes | Integer wire version; send `1`. |
| `messageId` | yes | Unique message identifier, normally UUID v4. Used by `ACK`. |
| `type` | yes | Message type listed below. |
| `source` | yes | Sender class, such as `x4` or `koreader`. |
| `deviceId` | yes | Stable identifier chosen by the sender. CrossPoint uses `x4-` plus the last three BLE MAC bytes. |
| `timestamp` | yes | Informational UTC ISO-8601 event time. Conflict resolution uses `updatedAt`, not this field. |
| `now` | recommended from client | Current Unix seconds. Lets the RTC-less X4 advance its clock. |

CrossPoint drops invalid JSON and messages whose `protocolVersion` is newer than
its own. Future clients should require the full envelope even though the current
firmware parser is deliberately tolerant of missing non-`type` fields.

## Book identity

`titleHash` is the required cross-device match key. It survives filename changes
and X4 EPUB optimization that changes file bytes.

Compute it as:

```text
normalizedTitle  = normalize(title)
normalizedAuthor = normalize(author)
titleHash = lowercase_hex(MD5(
  UTF8(normalizedTitle) + byte(0x1F) + UTF8(normalizedAuthor)
))
```

`normalize` is intentionally byte-simple and must be reproduced exactly:

1. Convert ASCII `A` through `Z` to `a` through `z`.
2. Treat space, tab, carriage return, and line feed as whitespace.
3. Trim leading and trailing whitespace.
4. Collapse each internal whitespace run to one ASCII space (`0x20`).
5. Pass every non-ASCII UTF-8 byte through unchanged. Do not apply Unicode case
   folding or normalization.

Example input before MD5:

```text
title  = "  The   Hobbit "  -> "the hobbit"
author = "J.R.R. Tolkien"   -> "j.r.r. tolkien"
bytes  = "the hobbit" + 0x1F + "j.r.r. tolkien"
```

`document` is an optional secondary identifier. Its exact value depends on the
CrossPoint KOReader matching setting (filename-derived MD5 or KOReader partial
file MD5), so clients must not require it to match a book. Prefer `titleHash`.

## Position representation

`PROGRESS.progress` is a KOReader/crengine XPath string. `percentage` is a
whole-book fraction from `0.0` through `1.0`. A client should preserve both:

- XPath is the preferred precise locator when the receiving renderer can map it.
- Percentage is the portable fallback if XPath mapping fails or pagination
  differs.

`updatedAt` is the Unix-seconds time when that book's position last changed. It
is the conflict key. Moving backward intentionally is still a new edit and must
receive a new timestamp.

## Messages

### `PAIR_HELLO` — optional logical introduction

Client → X4, written with response:

```json
{
  "protocolVersion": 1,
  "messageId": "75fc34f8-c407-47a7-b82b-bd97af9b07c2",
  "type": "PAIR_HELLO",
  "source": "koreader",
  "deviceId": "koreader-7f3a",
  "timestamp": "2026-07-16T18:40:09Z",
  "now": 1784227209,
  "displayName": "KOReader on Pixel"
}
```

CrossPoint records `deviceId` for the current BLE service lifetime. Pairing is
optional for synchronization; a client may proceed directly to `MANIFEST`.
The firmware also accepts a `PAIR_ACK` envelope for compatibility, but does not
currently send a pairing response or enforce an allowlist.

### `MANIFEST` — announce books and modification times

Either direction:

```json
{
  "protocolVersion": 1,
  "messageId": "384a52c9-d631-40b9-856b-fd610369e9f9",
  "type": "MANIFEST",
  "source": "koreader",
  "deviceId": "koreader-7f3a",
  "timestamp": "2026-07-16T18:40:10Z",
  "now": 1784227210,
  "more": true,
  "books": [
    {
      "titleHash": "809b960e1cd18f5f106cc0ab8e2b08cb",
      "document": "0baf4d1a608c2d9f3a8867efc5e1db42",
      "updatedAt": 1784227001
    },
    {
      "titleHash": "b1bd02f34e44d54f5ca77f168e42f72f",
      "updatedAt": 0
    }
  ]
}
```

- `books[].titleHash` is required for interoperable matching.
- `books[].document` is optional.
- `books[].updatedAt = 0` means the sender has the book but no clocked reading
  position. It is not newer than a positive timestamp.
- `more = true` means another `MANIFEST` page follows.
- `more = false` marks the final page, including an empty manifest.

Accumulate and de-duplicate all pages before comparing libraries. De-duplicate
by `titleHash`, using `document` only when `titleHash` is absent. Do not interpret
a book missing from one intermediate page as absent from the peer.

CrossPoint considers its 20 most-recent books and sends at most four rows per
page so each page fits one MTU-517 notification. A client with a larger library
may send any number of pages, subject to the negotiated payload limit.

### `PROGRESS` — send one complete reading position

Either direction:

```json
{
  "protocolVersion": 1,
  "messageId": "54ad69ff-34a1-42e4-a808-b311de9787b1",
  "type": "PROGRESS",
  "source": "x4",
  "deviceId": "x4-a1b2c3",
  "timestamp": "2026-07-16T18:40:11Z",
  "document": "0baf4d1a608c2d9f3a8867efc5e1db42",
  "titleHash": "809b960e1cd18f5f106cc0ab8e2b08cb",
  "progress": "/body/DocFragment[4]/body/p[18]/text().0",
  "percentage": 0.4275,
  "updatedAt": 1784227001
}
```

One `PROGRESS` carries one book. A receiver resolves `titleHash`, falls back to
`document`, and applies only when `updatedAt > localUpdatedAt` and
`updatedAt > 0`. Equal timestamps are already converged and do not overwrite.

When a client receives X4 `PROGRESS`, it must send an application `ACK` after it
has safely accepted or durably rejected the message. CrossPoint resends an
unacknowledged outgoing position every 700 ms, up to three attempts.

Client → X4 uses a GATT write with response, which supplies transport-level
delivery confirmation. Version 1 does not define X4 → client application ACKs,
so a client must not wait for one after sending `PROGRESS` to CrossPoint.

### `ACK` — acknowledge X4 `PROGRESS`

Client → X4:

```json
{
  "protocolVersion": 1,
  "messageId": "54e30821-f87f-49d7-b745-cfb0674fdc60",
  "type": "ACK",
  "source": "koreader",
  "deviceId": "koreader-7f3a",
  "timestamp": "2026-07-16T18:40:12Z",
  "now": 1784227212,
  "ackFor": "54ad69ff-34a1-42e4-a808-b311de9787b1"
}
```

`ackFor` echoes the received message's `messageId`. Send one ACK per received
X4 `PROGRESS`, including a duplicate resend. ACK is not used for manifests.

### `WANT` — request full positions

Either direction:

```json
{
  "protocolVersion": 1,
  "messageId": "a85f5903-a06e-4464-b7e2-01da1f49e36a",
  "type": "WANT",
  "source": "koreader",
  "deviceId": "koreader-7f3a",
  "timestamp": "2026-07-16T18:40:13Z",
  "now": 1784227213,
  "keys": [
    "809b960e1cd18f5f106cc0ab8e2b08cb",
    "b1bd02f34e44d54f5ca77f168e42f72f"
  ]
}
```

Each key is a `titleHash`. `WANT` is optional in the normal push-primary flow;
use it to repair a missed position or explicitly request a book before opening.

### Legacy diagnostic messages

`HAS_UPDATE`, `NEEDS_UPDATE`, and `DUMMY_POSITION_RESPONSE` remain accepted by
the implementation's diagnostic surface. They do not carry production reading
positions and are not required for a compatible client.

## Complete synchronization flow

The exchange is symmetric and push-primary: after both manifests are known,
the side with the newer timestamp sends the full position.

```text
Client                                                CrossPoint X4
  | scan for service UUID                                  |
  | connect; negotiate MTU; discover service               |
  | read capabilities                                      |
  | subscribe to X4-message notifications                  |
  |---- optional PAIR_HELLO ------------------------------>|
  |<--- MANIFEST page 1, more=true ------------------------|
  |<--- MANIFEST page N, more=false -----------------------|
  |                                                        |
  | accumulate all X4 pages; compare by titleHash          |
  |---- client MANIFEST page 1, more=true ---------------->|
  |---- client MANIFEST page N, more=false --------------->|
  |                                                        |
  | for each client-newer book:                            |
  |---- PROGRESS ------------------------------------------>|
  |                                                        |
  | for each X4-newer book:                                |
  |<--- PROGRESS -------------------------------------------|
  |---- ACK {ackFor: received messageId} ----------------->|
  |                                                        |
  | optional WANT / repair PROGRESS                        |
  | wait until traffic settles; disconnect                 |
```

CrossPoint declares success after it has sent its manifest, received a complete
client manifest (`more:false`), and observed 1.5 seconds without sync traffic.
The client may disconnect after it has sent all client-newer positions, ACKed
all X4 positions, and allowed the same quiet interval.

## Minimal client algorithm

1. Scan for service UUID `7ea41000-...` while a CrossPoint sync window is open.
2. Connect, request a large MTU, discover all four characteristics, read
   capabilities, and subscribe to notifications.
3. Optionally write `PAIR_HELLO`.
4. Start buffering X4 `MANIFEST` pages. Include current Unix time as `now` in
   every client message when possible.
5. After the final X4 page, build the local manifest. Send pages that fit one
   negotiated ATT payload; set `more:true` except on the last page.
6. For each X4 book:
   - local timestamp greater: write local `PROGRESS` to X4;
   - X4 timestamp greater: wait for X4 push, or send `WANT`;
   - equal timestamps: do nothing.
7. For client-only books, no X4 path exists, so CrossPoint cannot apply them
   until the book is present in its recent-book index.
8. For every X4 `PROGRESS`, apply strict newest-wins and write `ACK` with the
   received `messageId`. ACK duplicates too.
9. De-duplicate repeated manifests and progress messages. Applying the same or
   older `updatedAt` must be idempotent.
10. Disconnect after both sides converge and the line is quiet.

## Clock behavior

The ESP32-C3 loses wall time across deep sleep. CrossPoint stores the highest
Unix time it has seen in `/.crosspoint/ble-clock.bin` and seeds its clock from
that floor on boot. A client should include a valid `now` value (greater than
`1000000000`) so CrossPoint can advance to real wall time after sleeping.

When CrossPoint first learns a valid time, positions explicitly saved with an
unclocked `updatedAt = 0` are backfilled to that time. Missing timestamp files
are left untouched because they represent books with no BLE-tracked edit.

Known limitation: the persisted floor cannot measure time spent in deep sleep.
The next client connection corrects it forward. A logical or hybrid clock would
remove this limitation but is not part of version 1.

## CrossPoint synchronization windows

The BLE radio is enabled only during a window and is released afterward so
Wi-Fi can reclaim the radio and heap.

| Trigger | Window | User interaction |
|---|---:|---|
| Boot to library | 25 s | Background indicator |
| Open a book when the last sync is stale/absent | 12 s | Blocking, skippable |
| Exit a book | 20 s | Background indicator |
| Enable BLE sync or choose Pair | 25 s | Background indicator |
| Sleep after leaving a reader | up to 8 s; abort after 3 s without connection | Silent |

After a successful sync, book-open waits are skipped for 90 seconds. No peer in
range is a quiet no-op at the end of the window. BLE sync first disables Wi-Fi;
the two modes are not run concurrently on the X4.

## HTTP helpers

The existing CrossPoint web server also exposes read-only helpers. They are not
required for BLE interoperability.

### `GET /api/manifest`

Returns known local paths and matching keys:

```json
[
  {
    "path": "/Books/The Hobbit.epub",
    "titleHash": "809b960e1cd18f5f106cc0ab8e2b08cb"
  }
]
```

### `GET /api/progress?path=<url-encoded-epub-path>`

Returns HTTP 200 with:

```json
{
  "document": "0baf4d1a608c2d9f3a8867efc5e1db42",
  "progress": "/body/DocFragment[4]/body/p[18]/text().0",
  "percentage": 0.4275,
  "timestamp": 1784227001,
  "titleHash": "809b960e1cd18f5f106cc0ab8e2b08cb"
}
```

HTTP 204 means the book exists but has no saved position. The endpoint does not
construct a renderer or repaginate the book. There is intentionally no
`POST /api/progress`; XPath-to-page mapping is too expensive for the web task.

## Compatibility and error handling

- Ignore unknown JSON fields and unknown message types.
- Drop malformed JSON without disconnecting.
- Do not process a protocol version newer than the implementation supports.
- Clamp or reject `percentage` outside `0.0...1.0`.
- Reject a position with no usable `titleHash` and no usable `document`.
- Never overwrite on `updatedAt <= localUpdatedAt` or `updatedAt <= 0`.
- Bound receive queues and manifest sizes; CrossPoint's inbound queue holds 32
  parsed messages and drops the oldest on overflow.
- Reconnects start a new reconcile. CrossPoint clears its receive queue and ACK
  count when the BLE connection changes.

## Implementation checklist

A compatible client is ready when it can demonstrate all of the following:

- discovers by service UUID and subscribes before exchange;
- reads and gates on capabilities;
- sends a complete, paginated manifest with `now`;
- reproduces `titleHash` byte-for-byte for non-ASCII and whitespace fixtures;
- pushes a client-newer forward or backward position to CrossPoint;
- accepts an X4-newer position by XPath with percentage fallback;
- ACKs X4 progress and tolerates duplicate retries;
- does not treat an intermediate manifest page as the full peer library;
- converges equal timestamps without ping-pong;
- handles no-peer, disconnect, timeout, and reconnect paths;
- keeps all JSON objects within the negotiated ATT payload;
- does not claim authenticated pairing on the current unsecured transport.
