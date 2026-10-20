# x4-orbit: a Witch(hunt) Reader build for the Xteink X4 / X3

A personal build of [Witch(hunt) Reader](https://github.com/jpirnay/witchhunt-reader) for reading
EPUBs and pulling books from an OPDS server (BookOrbit). Branch: `x4-orbit`. Build profile:
`env:x4-orbit`.

Witch(hunt) was chosen as the base for its rendering: CSS and embedded-style layout, SD-card
fonts at every size, a 4-level grayscale image pipeline with pixel caches, background
pre-rendering of the next pages and sections, and large images shown as a placeholder that loads
at full quality on Confirm.

## What is different from Witch(hunt)

**Added**
- **Search in book** (reader menu > Navigation > Search book): case- and accent-insensitive,
  searches from the current chapter on and wraps, lists hits with chapter and snippet as they are
  found, jumps to the page and underlines the match. Any button press pauses it; Back stops it and
  keeps the results.
- **One TLS connection per OPDS visit.** Catalog pages, searches, covers and the book download
  reuse one kept-alive connection instead of a handshake each (seconds per request through a
  Cloudflare tunnel).
- **"On device" marker** on catalog books already in the download folder.
- **Long Back leaves the OPDS browser** from any screen in it (lists, loading, errors, book
  details) and goes Home.
- **Own boot splash.**

**Hidden / compiled out** (`CP_FEATURE_*` switches in `src/BuildFeatures.h`; the upstream envs
are unchanged): screenshots (menu entry and Power+Down), page-as-QR, weather, KOReader sync,
Calibre wireless, WebDAV, dictionary, internet firmware update, the font scaling test screen.
Witch(hunt) has no night-light mode, so there was nothing to remove there.

Kept: the Wi-Fi web server, USB file transfer, SD-card firmware update and the Up+Power recovery
picker, X3 and X4 support in one binary.

## Build

GitHub: push the `x4-orbit` branch. The **x4-orbit firmware** workflow runs the host tests, builds
`env:x4-orbit` and publishes `firmware-x4-orbit.bin` plus its web bundle (`web-<hash>.bin`) to the
rolling pre-release `x4-orbit-latest`.

Locally:

```sh
git submodule update --init --recursive
npm ci
pio run -e x4-orbit
# firmware: .pio/build/x4-orbit/firmware.bin, web bundle: .pio/build/x4-orbit/web-*.bin
```

## Flash

Copy `firmware-x4-orbit.bin` to the SD card and pick it in **Settings > System > SD Card Firmware
Update**. The first time the reader's web page is opened on Wi-Fi, it fetches its web bundle from
the `x4-orbit-latest` release by itself. In hotspot mode, copy the `web-*.bin` from that release
to the top of the SD card instead.

Stock CrossPoint or Witch(hunt) can be flashed back the same way.
