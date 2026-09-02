# Plugin events and the book metadata sidecar

Two firmware surfaces that let SD plugins react to what happens on the reader
and attach service data to books, while keeping all service knowledge out of
the firmware. They compose with the other plugin surfaces documented in
`sd-plugins.md`.

- **Events**: the firmware records a small whitelisted set of moments
  (a reading session ended, the device is going to sleep, ...) into a per-plugin
  queue on the SD card, and later replays them through HTTP request templates
  the plugin declared in its `device.json` — no browser, no plugin code
  running on the device.
- **The sidecar**: a small JSON file next to a book
  (`<book path>.meta.json`) where plugins record fields like a service book
  id at download time. Its fields ride along with KOSync progress uploads and
  are available to event handlers as substitution variables.

## Why delivery is deferred

Plugin event handlers need the network, and most events fire with WiFi down
(leaving a book, entering sleep). So `emit` only appends one JSON line to the
plugin's outbox (`<plugin dir>/events.jsonl`) — an SD append, nothing more —
and the queue is drained through the declared requests whenever the device is
already online: when the File Transfer web server comes up in Join Network
mode, or on the way into sleep (see the `connect` flag below). Delivery is
at-least-once and in order; dedupe on `ts` server-side if it matters.

## The event whitelist

Event names are a compatibility promise: they are semantic ("a reading session
ended"), never internal activity names, and manifests can only bind to names
on this list. Unknown names are ignored with a log line, so a manifest written
against newer firmware degrades gracefully on older firmware.

| Event             | Fires when                                             | Variables                            |
| ----------------- | ------------------------------------------------------ | ------------------------------------ |
| `reader.open`     | A book is opened (EPUB/TXT/XTC)                        | `book` (SD path)                     |
| `reader.exit`     | The reader is left (home, sleep, another activity)     | `book`, `percent` (0-100 whole-book) |
| `reader.session`  | An active-reading summary is flushed before sleep/exit | fields documented below              |
| `book.downloaded` | An on-device catalog download completed                | `path`, `title`, `plugin`            |
| `sleep.enter`     | The device begins entering deep sleep                  | `book`, `percent` when sleeping out of a reader, else none |

`sleep.enter` carries the reader snapshot because sleeping straight out of a
book is the common flow, and the reader's own `reader.exit` fires after the
sleep-time drain: binding a progress-sync handler to both events means the
current position is pushed on the sleep connection, not the next one.

### `reader.session` contract

All values are strings in the event's `vars` object:

| Variable | Meaning |
|---|---|
| `book` | SD path |
| `document` | binary KOSync partial-MD5, exactly 32 lowercase hex characters |
| `start_time` | trusted Unix epoch at the start of the first counted dwell |
| `end_time` | compact active end, exactly `start_time + duration_seconds` |
| `duration_seconds` | sum of counted active dwell seconds |
| `start_progress_bp` | first counted whole-book progress, 0..10000 basis points |
| `end_progress_bp` | monotonic-clamped final progress, 0..10000 basis points |
| `progress_scale` | literal `10000` |

Only successful ordinary forward turns count. A displayed page contributes
when it remains visible for at least 2 seconds; one dwell is capped at 1800
seconds. Reverse turns, chapter skips, failed turns, and shorter displays are
excluded and re-anchor the timer. At least 10 active seconds and a trustworthy
positive start time are required. EPUB retains basis-point precision; TXT/XTC
may report whole-percent steps. The document hash is calculated lazily only
when an emit-worthy summary and a subscriber both exist.

`end_time` is deliberately not the wall-clock time of the last interaction.
Idle gaps are removed, so it always equals `start_time + duration_seconds` and
is greater than `start_time` for every emitted session. The envelope
`event.ts` remains the actual emission time.

The pre-sleep activity hook queues and resets this event immediately before
`sleep.enter` and the sleep drain. Normal reader teardown calls the same
idempotent flush as a fallback, so direct-from-reader sleep queues it exactly
once and can deliver it on that same sleep.

A generic consumer can expand one summary into two distinct points without
firmware knowing the destination schema:

```json
[
  {"time": "{event.start_time}", "progress_bp": "{event.start_progress_bp}", "duration": "{event.duration_seconds}"},
  {"time": "{event.end_time}", "progress_bp": "{event.end_progress_bp}", "duration": "0"}
]
```

The distinct compact timestamps avoid a uniqueness collision even when the
two progress values are equal. Preserve at-least-once delivery and deduplicate
replayed points by the consumer's natural document/time key.

## Declaring handlers in `device.json`

```json
"events": {
  "reader.exit": {
    "request": {
      "method": "POST",
      "url": "{cfg.server}/api/progress",
      "headers": { "Authorization": "Bearer {token}" },
      "body": "{\"book\":\"{event.book}\",\"pct\":{event.percent},\"id\":\"{meta.bookfusion_id}\",\"ts\":{event.ts}}"
    },
    "toast": "Synced {event.book}"
  },
  "sleep.enter": {
    "download": { "url": "{cfg.server}/daily.bmp", "dest": "/sleep.bmp" },
    "connect": true
  }
}
```

One handler per event. A handler is either:

- **`request`** — `url`, `method` (default `POST`), `headers`, `body`. The
  response is an acknowledgement: any 2xx counts as delivered, the body is
  discarded (8KB cap).
- **`download`** — `url`, `method` (default `GET`), `headers`, `dest`. The
  response streams straight to `dest` on the SD card (1MB cap, never buffered
  in RAM). `dest` must be an absolute path without `..`. The example above
  fetches a fresh sleep image: the sleep-entry drain runs before the sleep
  screen renders, and the CUSTOM sleep mode reads `/sleep.bmp` first, so the
  fetched image appears on that very sleep.

Optional per handler:

- **`toast`** — a template shown as the standard popup after the request
  succeeds, when a screen is available (sleep entry, web-server session).
- **`connect`** — normally the drain only runs when the device is already
  online. `"connect": true` opts this handler into WiFi bring-up at **sleep
  entry only**: if any event is queued for any opted-in handler, battery is at least 20%, and a
  saved credential exists for the last-connected network, the device shows a
  popup, joins with a 10-second deadline, drains, and proceeds to sleep. A
  failed join just defers delivery; sleep is never blocked on the network.

Auth reuses the catalog vocabulary from the same `device.json`: `{token}` is
read from the declared token file, and on a 401/403 a `"password"`-type auth
section mints a fresh token and retries once — same behavior as catalog
browsing.

## Substitution variables

Templates in `url`, `headers`, `body`, `dest`, and `toast` substitute:

| Variable       | Source                                                          |
| -------------- | --------------------------------------------------------------- |
| `{token}`      | The plugin's token file (see `device.json` `token` section)      |
| `{cfg.KEY}`    | The plugin's flat config file (`config` section)                 |
| `{event.NAME}` | The event's variables (see the whitelist table)                  |
| `{event.ts}`   | Unix time when the event fired; 0 if the clock was never set     |
| `{meta.KEY}`   | The book's metadata sidecar, for events that carry a `book`      |

Unmatched variables substitute as nothing was replaced (the literal `{...}`
text is sent), so servers should tolerate or reject malformed values.

## The book metadata sidecar

Convention: a flat JSON file next to the book, `<book path>.meta.json`:

```json
{ "bookfusion_id": "36835", "source": "bookfusion" }
```

**Writers.** Anything that can already write small SD files:

- the on-device catalog's `sidecar` mechanism (`device.json` download section)
  at download time,
- browser-side plugin JS via `POST /api/plugin-fs`,
- a computer, by hand — it is just a file.

**Namespace your keys.** All plugins share one sidecar per book and the last
writer of a key wins. Use `bookfusion_id`, not `id`.

**Consumers.**

1. **KOSync progress uploads.** When *Send book metadata* is enabled in the
   KOSync settings, the sidecar's fields are merged verbatim into the
   `metadata` object of every progress upload (the reserved keys `filename`,
   `title`, `authors` cannot be overridden). Third-party KOSync servers ignore
   unknown fields; a custom sync server can route progress to a service using
   e.g. `metadata.bookfusion_id` — the firmware never learns what any key
   means.
2. **Event handlers**, via `{meta.*}` as above.

Fields are flat strings/numbers with a 2KB read cap. Structured service data
belongs on the service's server, keyed by the id in the sidecar.

## Delivery semantics and limits

- At-least-once, in order per plugin. The drain stops at the first failed
  delivery so a later event never overtakes an earlier one; delivered events
  are removed, the rest wait for the next drain.
- A corrupt queue line, or a line whose event no longer has a handler, counts
  as delivered — a manifest edit cannot wedge the queue.
- The subscription table is rebuilt at boot. A plugin installed mid-session
  starts receiving events after the next restart.

| Limit                          | Value                          |
| ------------------------------ | ------------------------------ |
| Outbox size                    | 4KB per plugin, then dropped wholesale (newest events describe current state; stale ones are worthless by then) |
| Event line                     | 512 bytes                      |
| Request response               | 8KB (discarded)                |
| Download                       | 1MB, streamed to SD            |
| Subscribed plugins             | 8                              |
| Handlers run per drain         | 4 (backlog continues next drain) |
| Sidecar / config / token files | 2KB each                       |

## Worked example: hands-free progress sync

A sync plugin writes a service id to the sidecar when it fetches a book, and
binds a request to `reader.session` with `"connect": true`. The user reads offline for a week and
presses sleep: the device queues the event with the current percent, joins the
saved network for a few seconds, POSTs
the compact active-session summary to the plugin's server, shows
"Synced", and goes to sleep. Nothing was tapped, no browser was involved, and
the firmware contains no mention of the service.
