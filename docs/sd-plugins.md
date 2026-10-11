# SD-card web plugins

A plugin is a folder on the SD card holding a `plugin.js`. The web server lists
it, the Settings or File Manager page loads it, and the script renders into a
card on that page. Adding or changing a plugin never needs a firmware build.

> **Provenance.** This system is ported from
> [crosspoint-reader#2734](https://github.com/crosspoint-reader/crosspoint-reader/pull/2734)
> ("feat: Add browser-side plugin system with SD card support") by Justin
> Mitchell ([@itsthisjustin](https://github.com/itsthisjustin)). The folder
> layout, the `/api/plugins` and `/plugin` contract, the `registerPlugin`
> handshake and the `mount` convention are his design and are kept compatible: a
> plugin that only uses the page and these two endpoints runs on both firmwares.
> The device-side `api` calls are not: upstream has since changed them
> (`relay(method, url, headers, body)` returning `{status, headers, body}`, a
> base64 `writeFile`), so a plugin that relays or writes files is written for one
> firmware or the other. This port takes a subset of that branch: the relay,
> fetch-to-SD and small-file endpoints are here (with the host allowlist upstream
> had removed), while its crypto endpoint, job queue, on-device catalog screens
> and content-protection work are not — see [Trust](#trust) and `allowedHosts`
> below.

Plugins extend the **web interface**, not the reader. Nothing runs on the
device: the firmware only enumerates the folders and serves their bytes, and the
script executes in the browser of whoever opened the page. Cost on the device is
one directory scan per page load and no resident RAM.

## Layout

```
<root>/<name>/
    plugin.js       the plugin (required - a folder without one is not listed)
    manifest.json   optional: { "title": "...", "mount": "settings" | "files" }
    ...assets       optional, served flat via api.pluginFile()
```

`<root>` is any of `/.crosspoint/plugins`, `/plugins`, or `/.plugins`. All three
are scanned; on a name collision the earliest in that order wins. The two
dot-free roots exist because a dot-prefixed folder is awkward to create in some
desktop file managers.

**Clear Cache does not remove plugins**, including from `/.crosspoint/plugins`.
That action is an allowlist: it deletes only the `epub_`/`xtc_`/`txt_` per-book
cache directories and leaves everything else in `/.crosspoint` alone. Note
though that `/plugins` is a normal visible folder, so unlike the two dot-prefixed
roots it can be deleted through the web file manager like any other.

Folders are flat. The server rejects any `name` or `file` containing `/`, `\`,
or `..`, so a plugin cannot ship subdirectories or read outside its own folder.

## Writing one

`plugin.js` calls `registerPlugin` at top level with a render function:

```js
CrossPoint.registerPlugin((container, api) => {
  container.innerHTML = '<h2>' + api.title + '</h2><button id="go">Scan</button>';
  container.querySelector('#go').onclick = async () => {
    const files = await (await fetch('/api/files?path=/Books')).json();
    // ...inspect, then POST to /move, /mkdir, /delete as needed
  };
});
```

The plugin is handed an empty `<div class="card">` and owns all of it, heading
included — the firmware renders no chrome of its own, so a plugin that draws no
heading shows an untitled card.

`api` carries:

| Field | Description |
|---|---|
| `name` | the folder name |
| `title` | `manifest.json`'s title, falling back to `name` — use it if you want the heading to track the manifest rather than hardcoding it |
| `pluginFile(filename)` | URL for another file in this plugin's folder |
| `relay(url)` | GET a URL the browser cannot reach itself; resolves to a `Response`. Allowlisted — see below |
| `fetchToSd(url, dest)` | download straight to the card, one transfer instead of two |
| `writeFile(path, data)` | write one small file (≤64KB); `data` may be a string, Blob or ArrayBuffer |
| `currentPath` | the folder the File Manager is showing. Default to it rather than asking for a path the user has already navigated to; always `/` on Settings |

**`api.crypto` is not provided.** The browser's own `crypto.subtle` covers
hashing, HMAC, AES and RSA, so a device-side implementation would only duplicate
it. A plugin written against the upstream API that calls `api.crypto` gets a
readable error rather than `is not a function`.

A plugin may declare what it needs, and one asking for something this firmware
does not implement is refused with a visible message instead of failing somewhere
inside its own code:

```jsonc
{ "requires": ["relay", "fetchToSd"] }
```

Supported: `relay`, `fetchToSd`, `writeFile`, `pluginFile`.

Everything else is the web server's own
same-origin API — a plugin uses `fetch()` against the endpoints in
[webserver-endpoints.md](webserver-endpoints.md) exactly as the pages do:
`/api/files`, `/mkdir`, `/move`, `/rename`, `/delete`, `/upload`, `/download`.
The File Manager page also has JSZip already loaded, so a plugin mounted there
can open and rewrite an EPUB in the browser.

Prefer these same-origin endpoints wherever they suffice — they cost the device
nothing beyond the request itself. A plugin reaches the SD card through the same
doors the web UI already opens, so it gains no privilege the page does not have.

Reaching *outward* is different, and is fenced separately: see `allowedHosts`
below. Where a remote site sends CORS headers, call it directly with `fetch()`
and leave the device out of it entirely.

### Reaching the internet: `allowedHosts`

A plugin runs in a browser page served by the device, so it can only read a
cross-origin response when the remote site sends CORS headers. Most do not —
Goodreads and `books.google.com` among them — and an image without CORS taints a
canvas, so its bytes can be displayed but never saved.

`GET /api/relay?plugin=<name>&url=<url>` fetches on the page's behalf and answers
same-origin. It is opt-in per plugin: the host must be listed in that plugin's
own `manifest.json`, so a plugin's reach is readable before you install it.

```jsonc
{
  "title": "Cover Finder",
  "mount": "files",
  "allowedHosts": [
    "openlibrary.org",        // exact match
    ".openlibrary.org"        // any subdomain, e.g. covers.openlibrary.org
  ]
}
```

No list means no access. `GET` only — a plugin cannot make the device POST
anywhere. A redirect is followed only to a host the list allows too; one to any
other host fails the request, so add the hosts a service redirects to (a CDN, say)
to the list.
The body streams back rather than being buffered, so a large image does not have
to fit in the device's largest free block.

Prefer a direct `fetch()` when the site sends CORS (Open Library does) and keep
the relay for sites that do not — it costs the device a network round trip.

### Mount points

| `mount` | Page | Suits |
|---|---|---|
| `settings` (default) | Settings | configuration, one-off maintenance actions |
| `files` | File Manager | anything operating on the library; JSZip available |

### Conventions

**Declare nothing at top level.** Plugins are `<script>` tags in the shared page,
so a top-level `function escapeHtml()` collides with the page and with every
other plugin, last one wins. Keep all state and helpers inside the
`registerPlugin` closure, as the bundled plugins do.

**Request one thing at a time.** The device serves a single connection, so
concurrent `fetch()` calls only queue - and a burst can stall a response long
enough to trip the firmware watchdog. `await` in a loop.

**Move a book's sidecars with it.** The firmware resolves a cover as `book.jpg`
(or `.jpeg`/`.png`/`.bmp`) beside `book.epub` and prefers it over the embedded
cover, so a plugin that relocates a book and leaves those behind silently breaks
its cover. `book.opf` travels with it for the same reason - see below.

### The `book.opf` metadata sidecar

Calibre writes an OPF beside each exported book. Where `book.opf` sits next to
`book.epub`, treat it as authoritative for that book's metadata: it is a few KB
against a multi-megabyte download, so reading it instead of the book is the
difference between a library organising instantly and one grinding through
every file. `organize-by-author` prefers it and falls back to the book's own
OPF, reporting which it used per book.

Prefer `opf:file-as` over the element text when deriving a folder name - that is
the sortable form, and what Calibre's own author folders use.

This mirrors the existing cover-sidecar rule, and the same priority applies:
sidecar wins over what is embedded in the book. **The firmware reads it too** -
the title, author and series shown on the device come from `book.opf` when one
is present, so a plugin writing one changes what the reader displays. See
[sidecar-files.md](sidecar-files.md) for the device-side rules.

## Installing and managing plugins: the Plugins page

`/plugins` in the web interface (linked from every page) lists every installed
plugin with its description, the page it adds itself to and the hosts it may
reach (`allowedHosts`), and offers **Switch off** / **Switch on** and **Remove**.
Plugin management lives here and nowhere on the reader: it is only ever useful
while a browser is connected.

It installs in two ways, both done by the browser; the reader only stores what
it is sent:

- **From the catalog**: `plugins/catalog.json` in this repository, read with the
  plugins' files straight from `raw.githubusercontent.com` (which allows
  cross-origin reads). It needs a browser with internet, so not hotspot mode.
  `?ref=<branch>` on the page reads another branch's catalog, to try plugins
  before they are merged. Regenerate the catalog with
  `scripts/build_plugin_catalog.py` after changing a plugin; CI fails when it is
  out of date. A manifest with `"catalog": false` stays out of it.

  Entries have the shape of crosspoint-reader's plugin catalog: `name`, `title`,
  `description`, `author`, `version`, `base` (the folder the files are read
  from) and `files` (their names), plus this firmware's `mount`, `requires` and
  `allowedHosts`. `base` is resolved against the catalog's own URL. The script
  gives this repository's plugins a relative one (`find-duplicates/`), so a
  catalog read from another branch installs that branch's files. A plugin hosted
  elsewhere is added to `catalog.json` by hand with an absolute `base`; the
  script keeps such entries, and the page shows where they come from. The
  crosspoint-reader catalog itself is not offered: most of its plugins need
  on-device screens (`device.json`) or a relay this firmware does not have.
- **From a `.zip`**: the plugin's files side by side, or inside one folder. The
  page shows the manifest's title, description and `allowedHosts` and asks
  before installing.

Installs go to `/.crosspoint/plugins/<name>`, through endpoints of their own
(`/api/plugins/begin`, `/file`, `/commit`; see
[webserver-endpoints.md](webserver-endpoints.md)): files are staged in
`<name>.new` and switched in with one rename, so a plugin cut off half-way never
runs. A folder copied by hand into `/plugins` or `/.plugins` still works and is
listed and managed the same way.

**Switch off** puts a `disabled` file in the plugin's folder: the pages skip the
plugin, and the setting holds for every browser. It only stops the pages loading
it: `/plugin`, `/api/relay` and `/api/fetch` still answer for it. That is no
loss of protection, since any script in a page already has the same reach.

Manifest fields the page shows: `title`, `version`, `description`, `mount`,
`allowedHosts` (see [Layout](#layout) for the rest).

## Bundled plugins

`plugins/` in this repository, offered by the Plugins page's catalog. They are
never compiled, so they cost no flash.

| Plugin | Mount | What it does |
|---|---|---|
| `hello-plugin` | settings | Minimal reference and smoke test - renders a card and lists `/` |
| `find-duplicates` | files | Reports files sharing an exact size. Report only: never writes, and never downloads a book |
| `organize-by-author` | files | Sorts loose EPUBs into `<Author>/` folders. Preview first, moves only on Apply, carries sidecars along |

The metadata editor that used to be a plugin here is now part of the web
interface: the File Manager's **Details** button on a book
(`src/network/html/js/metadata-editor.js`). A copy installed as a plugin still
loads; remove it on the Plugins page.

### Cover sources, and why they differ

The Details dialog is the worked example of the CORS rules above. It is not a
plugin, so its relay requests use the scope `:details`, whose host list is built
into the firmware (`www.goodreads.com`, `.gr-assets.com`, `books.google.com`); a
`:` cannot appear in a folder name, so no plugin can claim it.

| Source | Search | Cover bytes |
|---|---|---|
| Open Library | direct — it sends CORS | direct |
| Google Books | direct — the API sends CORS | **relay** — images are on `books.google.com`, which does not |
| Goodreads | **relay** — no API since 2020, so the search page is scraped | **relay** — images are on `i.gr-assets.com` |

Result thumbnails are plain `<img>` tags pointing at the remote host, because
*displaying* a cross-origin image was never restricted — only reading its pixels
is. The relay is used for one thing: reading the bytes of the cover you actually
pick. The device does work for one image, not for the whole grid.

Two caveats. The Goodreads path scrapes HTML and will break if they change their
markup — it looks for `img.bookCover` and strips the `._SX50_` size segment to
get the full-size image. And Google's covers are small (~128px), so Open Library
or Goodreads give a better result on the device.

## Loading order and failures

Plugins load **last** on a page, strictly one at a time. The device serves one
HTTP connection at a time, so a plugin fetching in parallel with the page's own
loaders can stall a response long enough to trip the firmware watchdog — which
is also why a plugin should not fire concurrent requests of its own.

A plugin that fails to load, or that never calls `registerPlugin`, renders an
error in its own card and does not affect the rest of the page or the other
plugins.

## Trust

A plugin is unsandboxed JavaScript running with the page's full authority: it
can read, rewrite, and delete anything on the SD card the web UI can reach.
Installing one is equivalent to running a script on your library — only use
plugins you would trust with the card. This is inherent to the design, not a
gap to be closed: the plugin is code you chose to copy onto your own card.

Note also that the web server has no authentication of its own, so it should
only be enabled on networks you trust — that is true with or without plugins.

## Limits

- `manifest.json` is read with a 4KB cap; a larger file is ignored (the plugin
  still loads, falling back to its folder name and the `settings` mount).
- An unparseable manifest is logged (`[WEB]`) and ignored the same way.
- A plugin whose listing entry would exceed 1536 bytes of JSON is skipped —
  in practice this means keeping `description` to a sentence or two.
- At most 64 directories are examined per root. The scan runs inside one
  request and each candidate costs two SD stats, so an unbounded walk over a
  folder holding hundreds of subdirectories could outlast the task watchdog.
  Hitting the bound logs to serial (`[PLG]`) and ignores the remainder — which
  is why a plugins root should be a dedicated folder, not a general-purpose one.
- `/api/plugins` and `/plugin` return 503 with `Retry-After` when the heap is
  too fragmented to serve them, like the other allocating endpoints.
- Install, update and removal go through the Plugins page (above); copying a
  folder onto the card works too.

## Debugging

1. Copy the folder to `/.crosspoint/plugins/<name>/` on the card.
2. Open Settings (or the File Manager) and check the card appears.
3. `GET /api/plugins` shows exactly what the firmware discovered — if a plugin
   is missing from that list, the folder has no `plugin.js` or a same-named
   folder in an earlier root shadowed it.
4. Script errors surface in the browser console, not on the device. Watch
   serial (`[WEB]`) only for manifest and file-serving problems.
