# x4-orbit — a Witch(hunt) Reader build for the Xteink X4 and X3

> **This branch is a personal, unofficial build of [Witch(hunt) Reader](https://github.com/jpirnay/witchhunt-reader)**
> (itself based on CrossPoint). It is not supported by either project. The Witch(hunt) README follows further down.

**Download:** [`firmware-x4-orbit.bin` (latest build)](https://github.com/riverac11-lgtm/x4-orbit/releases/tag/x4-orbit-latest)

## What this build is

Witch(hunt) Reader — chosen for its font, embedded-style and image handling and its background pre-rendering —
trimmed to reading and pulling books from an OPDS server (built around BookOrbit), with:

- **Search in book** (reader menu > Search book): case- and accent-insensitive, shows chapter and snippet for each
  hit, jumps to the page and underlines the match.
- **Faster OPDS browsing.** One secure connection is kept open for the whole visit instead of a new TLS handshake per
  catalog page, search, cover or download.
- **"On device" marker** on catalog books that are already in your download folder.
- **Hold Back to leave the OPDS browser** from any depth, straight to Home.
- **Its own boot splash.**

**Removed** from the menus and build: screenshots, show page as QR, weather, KOReader sync, Calibre wireless, WebDAV,
dictionary and internet firmware updates. The web server, USB file transfer, SD-card firmware update and recovery mode
are kept. Settings and OPDS servers live in the same files as in CrossPoint, so they normally carry over; if an OPDS
login fails after switching, enter it again. Details and build steps: [docs/x4-orbit.md](docs/x4-orbit.md).

## Who should not install it

**Only flash this on an Xteink X4 or X3** (one binary covers both). Do not install it on:

| Device | Why |
| --- | --- |
| Xteink X4 Pro, X4 Classic | Different chip (ESP32-S3) and hardware. The SD updater refuses this file; don't force it through a USB flasher. |
| LilyGo T5S3, Seeed reTerminal Sticky, M5PaperMono, any other device | Not built for them; the SD updater refuses this file. |
| **A USB-locked X4** (some third-party-store units) | If anything goes wrong there is no USB recovery path. Check that the [web installer](https://crosspointreader.com/#flash-tools) can see your device first, or unlock it with the [unlock tool](https://crosspointreader.com/#unlock-tool). |

Also skip it if you rely on any of the removed features above — use Witch(hunt) or CrossPoint instead.

## Installing and going back

1. Put `firmware-x4-orbit.bin` **and a fallback `.bin`** (stock [CrossPoint](https://github.com/daveallie/crosspoint-reader/releases)
   or [Witch(hunt)](https://github.com/jpirnay/witchhunt-reader/releases)) on the SD card.
2. Flash with **Settings > System > SD Card Firmware Update**.
3. Open the reader's web page once while on Wi-Fi: it downloads its matching web interface from the same release.
4. To go back: flash the fallback file the same way. If menus misbehave, hold **Up + Power** while powering on to go
   straight to the SD firmware picker. If the reader won't boot at all, use the
   [web installer](https://crosspointreader.com/#flash-tools) over USB-C.

This build has not been through either project's testing. Use it at your own risk.

---

# Witch(hunt) Reader

This firmware is based on the [crosspoint-reader](https://github.com/crosspoint-reader/crosspoint-reader), a great piece of software by Dave Allie and others.

It supports the following devices:
- ESP32C3-based Xteink X4 and X3.
- ESP32S3-based Xteink X4Pro, LilyGo T5S3 Pro

# Installation

Flashing is done from the browser — no toolchain or driver install needed. Use a Chromium-based browser (Chrome, Edge, Opera) or a recent Firefox version (>151); older versions of Firefox and Safari do not support WebSerial.

1. Download `firmware.bin` for your device from the [latest release](../../releases/latest).
2. Open the [CrossPoint flash tools](https://crosspointreader.com/#flash-tools).
3. Pick your device.
4. Choose **Custom .bin** and upload the `firmware.bin` you downloaded in step 1.
5. Connect the device via USB and start the flash — pick the device's serial port when the browser asks.

# What this reader does differently
- Speed - rendering should be *fast*
- CSS layout - a lot of effort have gone into rendering 
- Memory - where others fail Witch Reader still works
- Proper KOReader Synchronisation - a synced position lands on the exact page, in both directions
- A Library with Books, Recent, New and Authors tabs, each shown as filenames, details (title, author, series, progress) or a grid of covers
- SD-card fonts in every size they were built at, plus enlarged large-print sizes up to 26 pt
- Additional sleep screens support (information overlay, transparent pictures over current reader screen)
- Clock-Support for all supported devices
- Weather information panel
- Multiple under-the-hood performance improvements
- Book information screen
- Markdown-support
- WiFi captive portal support
- Supporting ~~strikethrough~~, superscript / subscript and tables
- Support for used defined actions on double-click / long-click per button 
- Background preprocessing of sections, so hopefully you will see fewer of the infamous "Indexing" messages
- A lot of smaller quality of life improvements 

# What this reader doesn't
* Great UI design is not necessarily/obviously not a forte of mine, so if you look for a polished look and feel, I would recommend going e.g. to [CrossInk](https://github.com/uxjulia/crossink), a great piece of work by uxJulia
* Support for CJK (Chinese Japanese Korean) - look at https://github.com/aBER0724/crosspoint-reader-cjk
* Right-to-left rendering support (Hebrew, Arabic) - choose the original [CrossPoint](https://github.com/crosspoint-reader/crosspoint-reader) firmware
* The most memory efficient reader might still be [MicroReader](https://github.com/CidVonHighwind/microreader) by CidVonHighwind

All of them have their strengths and constraints (as has Witch reader), so they deserve a testrun before you decide which one is right for you


# Feature comparison with CrossPoint

A feature-by-feature comparison of **Witch Reader** against its ancestor,
**[CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader)**, focused on what
a user actually sees and does on the device.

> **Snapshot of 2026-06-19.** Both projects are under active and dynamic development, so individual
> rows may change quickly — treat this as a point-in-time picture rather than a guarantee. The
> entries were verified against the current source of both projects rather than commit history,
> since the two have effectively diverged.

Legend: ✅ supported · ⚠️ partial / basic · ❌ not supported.

## Rendering & Typography

| Feature | Witch Reader | CrossPoint |
| --- | :---: | :---: |
| Floating images / text wrap around figures | ✅ left/right float, text wraps beside figure | ❌ falls back to block placement |
| Tables | ✅ real grid (colspan, header cells, multi-column) | ⚠️ flattened to text ("Row X, Cell Y:") |
| Images inside tables | ✅ | ❌ |
| Small-caps (`font-variant`) | ✅ | ❌ |
| Strikethrough (`line-through`) | ✅ rendered | ❌ parsed but never drawn |
| Superscript / subscript | ✅ | ✅ |
| Underline | ✅ | ✅ |
| CSS `line-height` | ✅ | ❌ |
| CSS `font-size` scaling (em/rem/%) | ✅ | ❌ |
| CSS margin collapsing | ✅ proper collapsing | ✅ proper collapsing |
| GIF images | ✅ custom decoder | ❌ JPEG + PNG only |
| JPEG / PNG decode | ✅ TJpgDec (IRAM) + uzlib PNG | ✅ JPEGDEC + PNGdec |
| Large-image placeholders & tall-image cropping | ✅ | ❌ |
| Grayscale image caches | ✅ | ⚠️ minimal |
| Heading fonts (h1–h3) | ✅ crisp taller real fonts | ⚠️ single font size per block |
| Horizontal rules | ✅ | ✅ |
| Hyphenation (9 languages) | ✅ | ✅ |
| Bionic / focus reading | ✅ | ✅ |
| Anti-aliasing toggle | ✅ (+ fast AA, max-darkness mode) | ✅ |
| Markdown (`.md`) rendering | ✅ headings, tables, HR, code, lists, TOC | ❌ shown as raw plain text |
| **Right-to-left / BiDi (Hebrew, Arabic)** | ❌ **not supported** | ✅ full BiDi + Hebrew font + CSS `direction` |
| Drop caps | ✅ float-zone initials, top-aligned | ❌ |

## Reading Experience, Library & Navigation

| Feature | Witch Reader | CrossPoint |
| --- | :---: | :---: |
| Background prerendering | ✅  | ❌ |
| Background indexing | ✅ (up to 3 sections ahead during reading pauses, not obstructing the reading experience) | ⚠️ (next section during the last 5 pages, partially blocking the reader) |
| Custom fonts | ✅  | ✅ |
| Sizes per font family | 4 | 3 |
| SD font rendering | ✅ on-demand glyph from memory | ⚠️ on demand-glyph from SD-card |  
| File browser sorting (name / date / size / type, asc/desc) | ✅ | ❌ name-only, fixed |
| File context menu (open, mark-read, info, set sleep screen, flash `.bin`…) | ✅ | ❌ open / delete only |
| Large-folder handling (SD-backed index, bounded RAM) | ✅ | ❌ all in RAM |
| Cover carousel home view | ✅ | ❌ |
| Cover-grid Recent Books view | ✅ | ❌ plain text list |
| Book Info screen (metadata, cover, paged description) | ✅ | ❌ |
| Global (cross-book) bookmarks | ✅ jump into any book at a page | ❌ |
| Named starred pages | ✅ custom names | ⚠️ unnamed progress snapshots |
| Reading statistics (streaks, time, pages/min, per-book ETA, sparkline) | ✅ | ❌ |
| Interactive "finished book" flow (open next, series detection, move folder) | ✅ | ⚠️ passive screen |
| Jump to printed page | ✅ | ❌ |
| Jump to percent | ✅ | ✅ |
| Quick per-book overrides while reading (font, images, hyphenation, bionic…) | ✅ | ❌ |
| Footnotes | ✅ richer navigation, inline expansion | ✅ |
| TOC / chapter selection | ✅ | ✅ |
| Browser breadcrumb footer + continuous page-jump | ❌ dropped | ✅ |

## System, Settings, Network & Input

| Feature | Witch Reader | CrossPoint |
| --- | :---: | :---: |
| Clock on X4 | ✅ software clock (X3 **and** X4) | ❌ X3 only (DS3231 hardware RTC) |
| Weather panel (Open-Meteo, forecast, 48h graph) | ✅ from Home menu | ❌ |
| Timezone auto-detect (IP geolocation + DST) | ✅ | ⚠️ l offset picker |
| Time sync (NTP) | ✅ | ✅ (X3) |
| Sleep screens: transparent overlay over reader page | ✅ | ❌ |
| Sleep screens: PNG with alpha | ✅ | ❌ BMP only |
| Sleep screens: info overlay (title/chapter/page/percent) | ✅ | ❌ |
| Sleep screens: sequential image pick | ✅ | ⚠️ random only |
| KOReader sync | ✅ auto bidirectional, on-device settings, differential refresh | ⚠️ proper sync + automatic resolution, no on-device registration |
| Per-button custom actions (23 actions × short/double/long) | ✅ + overview screen | ❌ physical remap + a few toggles |
| Captive-portal login (client detect + QR to authorize) | ✅ | ❌ AP-side only |
| System information screen | ✅ | ❌ |
| OTA / SD firmware update | ✅ | ✅ |
| Categorized settings submenus | ✅ | ⚠️ flat list |
| OPDS / Calibre / web upload | ✅ (+ format badges, signal widget, streaming) | ✅ |
| USB file transfer (serial protocol, Calibre plugin, `serial_cmd.py`) | ✅ wire-compatible with MicroReader | ❌ |
| USB file manager plugin (Total/Double Commander WFX, [x4-filemanager-plugin](https://github.com/jpirnay/x4-filemanager-plugin)) | ✅ | ❌ |
| Memory management | ✅ lean libraries, more on demand memory | ⚠️ limited memory management |

## At a glance

**Choose Witch Reader for:** speed, richer CSS and typography (floats, real tables, small-caps,
strikethrough, line-height), GIF and better image handling, Markdown, a weather panel, a clock on
the X4, reading statistics, global bookmarks, a cover carousel, fully customizable per-button
gestures, automatic KOReader sync, and a deeper settings/system surface.

**Choose CrossPoint for:** right-to-left languages (Hebrew / Arabic) — the one user-facing
capability Witch Reader genuinely lacks — plus  a
couple of small browser-navigation conveniences. It is also the leaner, simpler codebase.

# Rendering comparisons
Rendering examples from [Alice in Wonderland](https://www.gutenberg.org/ebooks/28885)
| Item 	|Witch Reader |	Micro Reader 1) 2) | CrossPoint |	
| --- | --- | --- | --- | 
| Floating images 1 | <img src="docs/images/comparison/01_leftfig.png">  | <img src="docs/images/comparison/01_leftfig_mr.png"> | <img src="docs/images/comparison/01_leftfig_cpr.png">   |    
| Floating images 2 | <img src="docs/images/comparison/02_rightfig.png">  | <img src="docs/images/comparison/02_rightfig_mr.png">   | <img src="docs/images/comparison/02_rightfig_cpr.png">   |    
| CSS Rendering | <img src="docs/images/comparison/03_render.png"> <br> <img width="480" height="800" alt="screen" src="https://github.com/user-attachments/assets/ee11c062-b3cf-4543-8954-7e45f77ba772" /> | <img src="docs/images/comparison/03_render_mr.png">  | <img src="docs/images/comparison/03_render_cpr.png"> |    
| Graphics | <img src="docs/images/comparison/04_graphic.png">  | <img src="docs/images/comparison/04_graphic_mr.png">  | <img src="docs/images/comparison/04_graphic_cpr.png">  |    
| Images in tables | <img src="docs/images/comparison/05_tablegraphic.png">  | <img src="docs/images/comparison/05_tablegraphic_mr.png">  | <img src="docs/images/comparison/05_tablegraphic_cpr.png">  |    

1) Apologies for the poor image quality of the microreader screenshots, i needed to make photos with my mobile, as I couldn't figure out how to create screenshots from within the reader
   
2) The Rendering of the Mouse poem in MicroReader is even more refined, it manages to deal with different font sizes, too

# Attributions
If in doubt consider all the work being done here based on the work of others - especially crosspoint reader (as the ancestor of this version) and microreader have been a great source of inspiration.

## Project ancestry & inspiration
- **crosspoint-reader** by Dave Allie and others — the direct ancestor this firmware is forked from. https://github.com/crosspoint-reader/crosspoint-reader (MIT).
- **MicroReader** by CidVonHighwind — a source of inspiration, and still the most memory-efficient reader for the X4. https://github.com/CidVonHighwind/microreader
  - The **USB serial file transfer** feature ([`lib/SerialTransfer`](lib/SerialTransfer), `SerialTransferActivity`) is a clean-room, wire-compatible reimplementation of MicroReader's serial protocol — independently written from its `tools/serial_cmd.py`, with no firmware code copied — so MicroReader's host tools (its Calibre device plugin and `serial_cmd.py`) work against this firmware too. Full credit to CidVonHighwind for the original protocol and host tooling.
- **FreeInk SDK** - the shared X3/X4 hardware/display/utility libraries, included as a submodule. https://github.com/Free-Ink/freeink-sdk (to which we contributed our buffer memory management, the split display update cycle and some other goodies we previously had in our own sdk, see below)
- **CrossPoint XDK** (No longer used, but ancestry) — the shared X3/X4 hardware/display/utility libraries, included as a submodule. https://github.com/jpirnay/crosspoint-xdk (modified fork of https://github.com/crosspoint-reader/community-sdk ).

## Vendored third-party components (`lib/`)
These are bundled directly in the repository. Each retains its upstream copyright header in source.

- **TJpgDec — Tiny JPEG Decompressor** by ChaN (R0.03) — baseline-JPEG decode engine for the EPUB image path. http://elm-chan.org/fsw/tjpgd/ — Copyright (C) 2021 ChaN, BSD-1-Clause. Vendored under [`lib/TJpgDec`](lib/TJpgDec); modified from upstream in `tjpgdcnf.h` (config + the `JD_FASTPATH` IRAM macro) and `tjpgd.c` (the `JD_FASTPATH` annotations on the hot decode functions, plus a `BYTECLIP` clamp on the grayscale output path fixing an upstream wrap-around bug — black speckle in high-contrast covers when `JD_FASTDECODE≥1`); `tjpgd.h` is verbatim.
- **yxml** by Yoran Heling — the XML/HTML SAX parser backend (`SaxParser`), used by the EPUB and OPDS parsers. https://dev.yorhel.nl/yxml — Copyright (c) 2013-2014 Yoran Heling, MIT. Vendored under [`lib/SaxParser`](lib/SaxParser).
- **uzlib** by Joergen Ibsen and Paul Sokolovsky — tiny DEFLATE/inflate, used for ZIP/EPUB extraction and PNG inflate. https://github.com/pfalcon/uzlib — Copyright (c) 2003 Joergen Ibsen, (c) 2014-2018 Paul Sokolovsky, zlib license. Vendored under [`lib/uzlib`](lib/uzlib).
- **QR-Code-generator (qrcodegen)** by Project Nayuki — QR code generation. https://github.com/nayuki/QR-Code-generator — Copyright (c) Project Nayuki, MIT. Vendored under [`lib/QRCode`](lib/QRCode).
- **FreeType's anti-aliasing rasterizer (`ftgrays.c`, FreeType 2.13.3)** by David Turner, Robert Wilhelm and Werner Lemberg — draws the glyph outlines of TrueType (`.ttf`) fonts on the device. https://freetype.org — Portions of this software are copyright © 2024 The FreeType Project (www.freetype.org). All rights reserved. FreeType License (FTL, [`FTL.TXT`](lib/EpdFont/ttf/third_party/freetype/FTL.TXT)). Vendored under [`lib/EpdFont/ttf/third_party/freetype`](lib/EpdFont/ttf/third_party/freetype) with six patches listed in its `PATCHES.md`, regenerated by `scripts/vendor_ftgrays.py`. Only the rasterizer is used; the TrueType loader around it is this project's own.

## External libraries (PlatformIO `lib_deps`)
Pulled from the PlatformIO registry at build time.

- **ArduinoJson** by Benoît Blanchon — JSON parsing/serialization. https://github.com/bblanchon/ArduinoJson — MIT.
- **arduinoWebSockets** by Markus Sattler — WebSocket client (KOReader sync). https://github.com/Links2004/arduinoWebSockets — LGPL-2.1.

# Why this name
Originally this fork was called CrossPoint++ - it had a small userbase and then I made an honest mistake by reusing the crosspoint fork, providing ample reference in the PRs and the release notes of code origin and authorship but was losing the github commit author information in the progress when I copied code over instead of taking the tedious (and correct) way of cherrypicking the original commit and post-cleanup effort.

Another crosspoint developer approached me pointing this flaw out and I agreed to change the future integration work. What I did not care for was that persons attitude and way of communicating, and I told him so.

Then all hell broke lose, ending up in insults, harassment and plain lies in other forums without even caring for feedback (a good cause for any lawsuit for slander). So I took the repo down and just continued the development for my own benefit.

Still I wanted others to benefit from my progress, so here we are again:

Witch(hunt) Reader (name for obvious reasons) 

- So if you are one of those who felt poorly appreciated: please accept my apologies, that was never my intent, and I have taken a lot of effort to replace code / to properly attribute the origin of code or ideas
- If you are one of those who felt the need to raise a witchhunt, to lie, to libel: just go away - trolls aren't welcome here or more clearly: F*** OFF
