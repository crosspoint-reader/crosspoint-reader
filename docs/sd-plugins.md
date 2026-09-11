# SD-card plugins

CrossPoint keeps service integrations off the firmware. A plugin is a folder on
the SD card; the firmware provides three generic, scheme-neutral surfaces that
plugins compose. Adding or updating a plugin never requires a firmware build,
and the firmware carries no vendor names, URLs, or file-format knowledge.

```
<root>/<name>/
    manifest.json     web UI card metadata (optional): title, description, mount
    plugin.js         browser-side plugin (optional)
    device.json       on-device catalog screen (optional): title, description, ...
    README.md         usage instructions, shown on-device (optional)
    ...assets
```

**Discovery and the on-device list.** Every plugin folder (anything holding a
`manifest.json`, `plugin.js`, or `device.json`) appears under **Settings →
System → Plugins** on the reader, showing its `title` and one-line
`description`. Selecting a plugin opens an info screen with the description and
the plugin's `README.md` as scrollable usage instructions — so even a
browser-only plugin (no `device.json`) is listed and can explain how to use it
from the web UI. A plugin that ships a `device.json` also gets an **Open**
action there to launch its on-device catalog. `title`/`description` are read
from `manifest.json`, with `device.json` overriding when present.

`<root>` is any of `/.crosspoint/plugins`, `/plugins`, or `/.plugins` — the
first two-dot-free options exist so plugins are easy to copy onto the card
from a computer. All three roots are scanned; on a name collision the earlier
root in that order wins.

A plugin can ship any combination: `plugin.js` alone (web-only),
`device.json` alone (on-device only), or both (e.g. sign in from either side,
browse on the reader).

The plugin sources and examples live in the separate `sd-plugins` repository,
which also documents the browser-side `plugin.js` API. This document covers the
firmware surfaces: the job queue and the `device.json` on-device screens.

## Surface 1: browser plugins (`plugin.js`)

JS loaded into the File Manager or Settings web page, backed by generic device
endpoints (`/api/relay`, `/api/crypto`, `/api/fetch`, `/api/plugin-fs`). See
`docs/webserver-endpoints.md` for the endpoints and the sd-plugins repository
for the JS contract.

## Surface 2: the plugin job queue (external automation)

External systems (a companion app, a script) can trigger plugin actions
without a human clicking the web UI. The firmware stores small opaque
`{plugin, action, args}` JSON blobs in a fixed 6-slot pool; it never interprets
them. Any open page hosting the plugin — including the headless runner at
`GET /plugins-run` — claims jobs, executes the plugin's registered handler in
the browser context, and posts the result back.

| Endpoint | Purpose |
|---|---|
| `POST /api/plugin-jobs` | enqueue `{plugin, action, args?}` → `{id}` (503 when the pool is full) |
| `GET /api/plugin-jobs/claim?plugin=` | executor claims the next pending job |
| `POST /api/plugin-jobs/complete` | executor posts `{id, ok, result?}` |
| `GET /api/plugin-jobs/status?id=` | caller polls: `{id, state, result}`; states: `pending`, `running`, `done`, `error`, `unknown` (recycled) |
| `GET /plugins-run` | headless page that loads every plugin (UI hidden) and executes jobs while open |

Flow for an external system:

```sh
# 1. upload whatever the action needs (e.g. a file, via /upload)
# 2. enqueue
curl -X POST http://crosspoint.local/api/plugin-jobs \
  -d '{"plugin":"<name>","action":"<action>","args":{"path":"/folder/file"}}'
# -> {"id":7}
# 3. open http://crosspoint.local/plugins-run in a browser tab or hidden
#    webview (jobs only execute while a hosting page is open)
# 4. poll
curl "http://crosspoint.local/api/plugin-jobs/status?id=7"
# -> {"id":7,"state":"done","result":{...}}
```

Limits: plugin/action names < 24 chars, args and result < 192 bytes of JSON
each. A job claimed by a runner that dies is reclaimable after 10 minutes.
Results persist only until their slot is recycled — poll promptly.

Plugins register handlers in `plugin.js`:

```js
api.registerAction('myaction', async (args) => {
  // runs in the hosting page; throw -> state "error" with {error: message}
  return { anything: 'small' };  // -> state "done" with this result
});
```

## Surface 3: on-device catalog screens (`device.json`)

A declarative manifest the firmware's generic `PluginCatalogActivity` renders
under **Settings → System → Plugins**. It expresses "authenticated JSON
catalog: sign in, browse, download, sidecar" — enough for most book services —
without any code running on the device. Anything beyond this vocabulary
belongs in `plugin.js`.

The firmware piece (service-agnostic, in `src/activities/plugins/`):
`PluginCatalogActivity`, one activity that opens as a picker over the
installed `device.json` plugins (title + description rows) and morphs into
the manifest-driven browse / download / sign-in flow for the picked plugin.
`discoverPlugins()` rescans the plugin folders each time the picker opens;
nothing stays resident. Browser-only plugins (`plugin.js` without a
`device.json`) are listed as inert "Web-only plugin" rows — an install is
visibly installed — while their UI lives in the web interface.

### Schema

Every string field is a template. Available substitutions:

| Variable | Meaning | Available in |
|---|---|---|
| `{token}` | contents of the token file at `token.path` | everywhere |
| `{cfg.KEY}` | value of `KEY` in the flat JSON config file at `config.file` | everywhere |
| `{page}` | current 1-based page | browse |
| `{limit}` | `page_size + 1` (the extra row detects "more pages") | browse |
| `{id}` `{title}` `{author}` `{url}` | fields of the selected item | download, sidecar |
| `{md5}` | MD5 hex of the destination file path | sidecar |
| `{dest}` | sanitized on-SD path of the downloaded file | sidecar |
| `{device_code}` | from the auth request response | auth poll |

Two browse formats:
- **`"json"`** (default) — a paged JSON list, using `items`/`fields` below.
- **`"xml"`** — walks a repeating XML element with optional folder navigation
  (Confirm opens a folder, Back goes up). No paging; the JSON `items`/
  `page_size` are ignored. The firmware knows no specific protocol: a WebDAV
  `PROPFIND` multistatus, an OPDS/Atom feed, or any XML list is all manifest
  config. XML field selectors are `"elem"` (child element text), `"elem@attr"`
  (child element attribute), or `"@attr"` (attribute on the item element).

```jsonc
{
  "title": "Service Name",                  // menu label; defaults to folder name

  "token": {                                // omit for token-less catalogs
    "file": "/.crosspoint/<name>.json",     // written by auth (either side)
    "path": "token"                         // dotted JSON path inside the file
  },

  "config": {                               // optional: flat JSON of {cfg.KEY} values,
    "file": "/.crosspoint/<name>-cfg.json"  // e.g. a user-entered server URL + credentials
  },

  "browse": {                               // required
    "format": "json",                       // or "xml"
    "url": "https://.../search",
    "method": "POST",                       // default GET
    "headers": { "Authorization": "Bearer {token}" },
    "body": "{\"page\":{page},\"per_page\":{limit}}",

    // --- json format ---
    "items": "",                            // dotted path to the item array; "" = response root
    "fields": {                             // json: dotted paths (numeric = array index)
      "title": "title",                     // required (items without one are dropped)
      "author": "authors.0.name",
      "id": "id",
      "url": "download_url",                // when the item carries a direct file URL
      "version": "version"                  // catalog-of-plugins only: badges each row
                                            // Installed / Update by comparing this to the
                                            // installed plugin's manifest.json (found by id)
    },
    "page_size": 8,                         // 1..16; response should honor {limit}

    "lists": [                              // optional named sub-catalogs (json only):
      { "title": "All Books" },             // a picker screen precedes browsing;
      { "title": "Favorites",               // each entry may override url and/or
        "body": "{\"page\":{page},\"per_page\":{limit},\"list\":\"favorites\"}" }
    ],                                      // omitted keys fall back to browse's

    // --- xml format (ignore the json fields above; fields become selectors) ---
    "item": "response",                     // local-name of the repeating element
    "container_element": "collection",      // presence marks a navigable folder (omit = flat)
    "skip_self": true,                      // drop the entry equal to the request URL
    "resolve_urls": true,                   // resolve url field against the request origin
    "extensions": [".epub", ".pdf"]         // allowed file extensions (omit = all)
  },

  "download": {
    "url": "https://.../books/{id}/download",
    "method": "POST",
    "headers": { "Authorization": "Bearer {token}" },  // file GET too, unless url_path is set
    "body": "{}",
    "url_path": "url",                      // response field with the file URL;
                                            // omit to treat "url" itself as the file URL
    "username": "{cfg.user}",              // optional HTTP Basic creds for the file GET
    "password": "{cfg.pass}",             // omit for token/header auth
    "dest_dir": "/ServiceName",             // created if missing; falls back to SD root
    "filename": "{title}.epub",             // rendered filename is sanitized to 100 bytes;
                                            // a conventional extension is preserved
    "sidecar": {                            // optional per-book metadata file
      "path": "{dest}.meta.json",             // the book metadata sidecar convention (see plugin-events.md)
      "body": "{\"book_id\":{id}}"
    }
  },

  "auth": {                                 // optional on-device sign-in
    "type": "device_code",                  // or "password"

    // device_code (interactive: shows a code + QR, polls until authorized):
    "request": { "url": "...", "method": "POST", "headers": { ... }, "body": "..." },
    "poll":    { "url": "...", "method": "POST", "headers": { ... },
                 "body": "{...\"device_code\":\"{device_code}\"}" },
    // device_code response field paths, with their defaults:
    "code_path": "user_code", "verify_url_path": "verification_uri",
    "device_code_path": "device_code", "interval_path": "interval",
    "expires_path": "expires_in", "token_path": "access_token",
    "error_path": "error"

    // password (silent: mints a token from stored config credentials — for
    // OAuth2 password grants and login-returns-token APIs). Only "request" and
    // "token_path" apply; no user interaction:
    //   "type": "password",
    //   "request": { "url": "{cfg.url}/oauth/v2/token", "method": "POST",
    //                "headers": {...}, "body": "...{cfg.username}...{cfg.password}..." },
    //   "token_path": "access_token"
  }
}
```

### Behavior

- **Sign-in (`device_code`)**: the not-signed-in screen offers Sign in on the
  device: it shows the verification URL (text + QR) and user code, then polls
  until authorized, honoring `authorization_pending`, `slow_down`,
  `expired_token`, and `access_denied`. The token is written to `token.file`
  in the shape `token.path` expects, so the web plugin and the device share
  one sign-in.
- **Sign-in (`password`)**: no on-device UI — the device silently POSTs the
  stored config credentials to the token endpoint before browsing, and re-mints
  a fresh token automatically after any 401 (these tokens are short-lived).
  Credentials come from a browser `plugin.js` writing the `config.file`. Set it
  up once on the web page; the reader is then standalone.
- Without an `auth` block the not-signed-in screen directs the user to the web
  plugin.
- **Stale tokens**: a 401/403 from browse returns to the sign-in screen rather
  than an error.
- **Pagination** (json): the browse request should return up to `{limit}`
  items; the firmware displays `page_size` and uses the extra row to know
  another page exists. Navigation matches the OPDS browser: a "Previous
  page" row precedes the items past page 1 and a "Next page" row follows
  them while more pages exist — tappable and button-reachable like any row.
- **Lists** (json): with `browse.lists`, opening the catalog shows a picker
  of the named entries first; each entry browses with its own url/body
  overrides (server-side categories, shelves, sort orders). Back from the
  book list returns to the picker.
- **XML navigation**: Confirm on a folder (an item carrying `container_element`)
  descends into it, Back climbs out (Back at the root leaves the screen).
  With `resolve_urls`, server-relative URLs resolve against the browse URL's
  origin. A `config.file` lets values like a server URL and credentials be
  user-entered (via a browser `plugin.js`) rather than baked into the manifest.
- **After download**: the book's layout cache is invalidated and the optional
  sidecar is written. Use the `{dest}.meta.json` convention so the fields ride
  KOSync progress uploads and event handlers (see `plugin-events.md`).

### Heap budget (ESP32-C3, ~380KB total)

- Manifest ≤ 8KB, token file ≤ 2KB, small API responses (auth, download-url
  hop) ≤ 48KB (hard caps).
- Browse responses (json and xml) stream to an SD temp file and are parsed
  from it, so the raw body never occupies DRAM — services that inline heavy
  per-item metadata (a page of BookFusion JSON runs 60+ KB) work unmodified.
  A 1MB cap bounds a misbehaving server.
- Catalog responses are parsed with a dynamically built ArduinoJson filter
  containing only the declared field paths, so a page parses into a few KB
  no matter the body size.
- One TLS session is kept alive across browse requests: repeated handshakes
  permanently fragment the heap.
- The book download streams to SD via `HttpDownloader`; file size is
  unconstrained.
- An XML list is parsed from the temp file in 2KB chunks; the engine stops
  after 200 entries per folder/feed. Split very large libraries into
  subfolders.

### Testing a new manifest

1. Copy the plugin folder to `/.crosspoint/plugins/<name>/` on the SD card.
2. Settings → System → Plugins → your title. With no token and no `auth`
   block you should see the not-signed-in screen; with `auth`, the code/QR
   screen.
3. Watch serial (`[PCAT]` tag) for request/parse failures — the log includes
   HTTP status and truncation flags.
4. Token-less services (public JSON APIs) work by omitting the `token` block
   entirely; that is the quickest way to validate `items`/`fields` paths.

## Surface 4: firmware events and the book metadata sidecar

The firmware can record whitelisted moments (a reading session ended, the
device is entering sleep, a catalog download finished) into a per-plugin queue
and later replay them through HTTP request templates declared in
`device.json` — including bounded WiFi bring-up at sleep entry for handlers
that opt in. A companion convention, the `<book path>.meta.json` sidecar,
lets plugins attach service fields (e.g. a service book id) to a book; those
fields ride along with KOSync progress uploads and are available to event
handlers as `{meta.*}` variables. See `plugin-events.md` for the whole
surface: the event whitelist, handler schema, delivery semantics, and limits.

## Ideas to build

The two planes decide *how* you build something, so pick by what the idea needs:

- **On-device (`device.json`)** — standalone on the reader, but only the
  declarative vocabulary above: an authenticated JSON or XML/OPDS-style catalog,
  browse, download to SD, optional device-code or password sign-in and per-book
  sidecar. No arbitrary logic.
- **Browser (`plugin.js`)** — anything at all (any protocol, auth, format), but
  it runs in a connected browser/app. Use it when the idea needs request
  signing, HTML→EPUB conversion, multi-file writes, or a real UI. The job queue
  (`/api/plugin-jobs` + `/plugins-run`) lets an external app trigger it.

Rule of thumb: if the service is "log in, list an authenticated JSON/XML
catalog, download files," it's on-device. If it needs computed request bodies
(OAuth 1.0a signing, canonicalized signatures) or format conversion, it's
browser-side.

### On-device (`device.json`) ideas

- **Self-hosted library presets** — Kavita, Komga, Audiobookshelf, Calibre-Web,
  COPS. Most expose OPDS (native OPDS browsing already covers those — a plugin
  adds value mainly for their JSON APIs or token auth). Build: `browse.format`
  `json` against the REST API, or `xml` against the OPDS feed; `download.url`
  from the entry; bearer/Basic/`{cfg.*}` auth. See `bookfusion` (JSON + token)
  and `webdav` (XML + config creds).
- **Dictionary store** — browse a JSON index of dictionary files, download to
  the SD dictionary dir. No native equivalent. Build: token-less `json` catalog,
  `download.dest_dir` = the dictionary folder, no sidecar. ~15 lines.
- **Theme store / font store** — same shape as the dictionary store, pointed at
  the theme or font directory. Asset catalogs OPDS can't express.
- **More read-later that exports EPUB** — anything with a token API and an
  EPUB export URL fits like `wallabag`: `auth.type` `password` (or a stored
  bearer), `browse` the entries JSON, `download` the export endpoint with a
  Bearer header.
- **A curated free-books catalog** — Standard Ebooks / Gutenberg via their OPDS
  feed (`xml` format, token-less). Overlaps native OPDS, but ships as a
  zero-config preset.

### Browser (`plugin.js`) ideas

- **Plugin store** — install other plugins from a hosted catalog. Build: fetch
  a catalog with `api.relay`, `/mkdir` the target folder, `api.writeFile` each
  file. See `plugin-store/`.
- **AO3 / fanfiction downloader** — the site serves per-work EPUB downloads.
  Build: take a work URL, `api.fetchToSd` (or `api.relay` + `api.writeFile`) the
  EPUB to SD. Browsing/search is HTML, so parse it in the browser.
- **RSS → EPUB digest** — build a "morning edition." Fetch feeds with
  `api.relay`, assemble an EPUB in the browser with the JSZip the File Manager
  page already loads, then `api.writeFile` it to SD.
- **Read-any-article** — paste a URL, run it through a readability service via
  `api.relay`, wrap the result as an EPUB, write to SD.
- **Metadata / cover editor** — a File Manager plugin: open an EPUB with JSZip,
  edit the OPF (title/author/cover), rewrite it in place. See
  `organize-by-author/` for the File-Manager-endpoints pattern.
- **Library tidy tools** — dedupe, bulk rename, move-by-metadata. Same
  same-origin endpoints (`/api/files`, `/mkdir`, `/move`, `/delete`), no device
  API needed.
- **Signed-API services** (Instapaper OAuth 1.0a, etc.) — must be browser-side:
  do the per-request signing in JS with `crypto.subtle`, relay the calls, and
  convert the article HTML to EPUB before writing to SD.

### Building blocks reference

| Need | Use |
|---|---|
| Outbound HTTP(S), any method (CORS-free) | `api.relay(method, url, headers, body)` |
| Download a URL straight to SD | `api.fetchToSd(url, dest, headers)` |
| Write a small file to SD | `api.writeFile(path, base64)` |
| Crypto (hash, HMAC via SHA, AES, RSA) | `api.crypto(op, fields)` or browser `crypto.subtle` |
| Create / delete / move SD files | same-origin `/mkdir`, `/delete`, `/move` |
| On-device catalog/browse/download | `device.json` (this document) |
| External app triggers an action | `api.registerAction(name, fn)` + `/api/plugin-jobs` |

## Protected content and loan expiry

Books whose entries are content-protected open through the read path in
`lib/Epub/ContentProtection.cpp`: the access credential lives at
`/.crosspoint/content.key`, and an out-of-band rights document may sit next to
the book as `<book>.epub.rights` (falling back to a rights file inside the
zip). Entries decrypt on demand, streamed in small chunks; nothing decrypted
is ever written to SD.

When the rights carry a due date, the reader enforces it against
`lib/TrustedTime`: a monotonic clock floor persisted in NVS (on-flash, not on
the removable card), restored into the system clock at boot, advanced at
every sleep entry and snapped to real time by SNTP on every Wi-Fi join. The
floor can lag real time while the device sits powered off, but it can never
be rolled back — staying offline delays a due date at most by the powered-off
gap, it does not suspend it. A book with a due date and no trustworthy clock
at all refuses to open and asks for one Wi-Fi connection.
