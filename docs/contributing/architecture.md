# Architecture Overview

CrossPoint is firmware for the Xteink X4 (unaffiliated with Xteink), built with PlatformIO targeting the ESP32-C3 microcontroller.

At a high level, it is firmware that uses an activity-driven application architecture loop with persistent settings/state, SD-card-first caching, and a rendering pipeline optimized for e-ink constraints.

## System at a glance

```mermaid
graph TD
    A[Hardware: ESP32-C3 + SD + E-ink + Buttons] --> B[freeink-sdk]
    B --> C[lib/hal wrappers]
    C --> D[src/main.cpp runtime loop]
    D --> E[Activities layer]
    D --> F[State and settings]
    E --> G[Reader flows]
    E --> H[Home/Library/Settings flows]
    E --> I[Network/Web server flows]
    G --> J[lib/Epub parsing + layout + hyphenation]
    J --> K[SD cache in .crosspoint]
    E --> L[GfxRenderer]
    L --> M[E-ink display buffer]
```

## Runtime lifecycle

Primary entry point is `src/main.cpp`.

```mermaid
flowchart TD
    A[Boot] --> B[Init GPIO and optional serial]
    B --> C[Init SD storage]
    C --> D[Load settings and app state]
    D --> E[Init display and fonts]
    E --> F{Resume reader?}
    F -->|No| G[Enter Home activity]
    F -->|Yes| H[Enter Reader activity]
    G --> I[Main loop]
    H --> I
    I --> J[Poll input and run current activity]
    J --> K{Sleep condition met?}
    K -->|No| I
    K -->|Yes| L[Persist state and enter deep sleep]
```

In each loop iteration, the firmware updates input, runs the active activity, handles auto-sleep/power behavior, and applies a short delay policy to balance responsiveness and power.

## Activity model

Activities are screen-level controllers deriving from `src/activities/Activity.h`.
Some flows use `src/activities/ActivityWithSubactivity.h` to host nested activities.

- `onEnter()` and `onExit()` manage setup/teardown
- `loop()` handles per-frame behavior
- `skipLoopDelay()` and `preventAutoSleep()` are used by long-running flows (for example web server mode)

Top-level activity groups:

- `src/activities/home/`: home and library navigation
- `src/activities/reader/`: EPUB/XTC/TXT reading flows
- `src/activities/settings/`: settings menus and configuration
- `src/activities/network/`: Wi-Fi selection, AP/STA mode, file transfer server
- `src/activities/boot_sleep/`: boot and sleep transitions

## Reader and content pipeline

Reader orchestration starts in `src/activities/reader/ReaderActivity.h` and dispatches to format-specific readers.
EPUB processing is implemented in `lib/Epub/`.

```mermaid
flowchart LR
    A[Select book] --> B[ReaderActivity]
    B --> C{Format}
    C -->|EPUB| D[lib/Epub/Epub]
    C -->|XTC| E[lib/Xtc reader]
    C -->|TXT| F[lib/Txt reader]
    D --> G[Parse OPF/TOC and collect CSS refs]
    G --> H[Build/load book.bin and css_rules.cache]
    H --> I[Layout pages/sections]
    I --> J[Write section cache]
    J --> K[Render current page via GfxRenderer]
```

Why caching matters:

- RAM is limited on ESP32-C3, so expensive parsed/layout data is persisted to SD
- repeat opens/page navigation can reuse cached data instead of full reparsing

## Reader internals call graph

This diagram zooms into the EPUB path to show the main control and data flow from activity entry to on-screen draw.

```mermaid
flowchart TD
    A[ReaderActivity onEnter] --> B{File type}
    B -->|EPUB| C[Create Epub object]
    B -->|XTC/TXT| Z[Use format-specific reader]

    C --> D[Epub load]
    D --> E[Locate container and OPF]
    E --> F[Build or load BookMetadataCache]
    F --> G[Load TOC and spine]
    G --> H[Load CSS cache or parse manifest/base-dir CSS]

    H --> I[EpubReaderActivity]
    I --> J{Section cache exists for current settings?}
    J -->|Yes| K[Read section bin from SD cache]
    J -->|No| L[Parse chapter HTML and layout text]
    L --> M[Apply typography settings and hyphenation]
    M --> N[Write section cache bin]

    K --> O[Build page model]
    N --> O
    O --> P[GfxRenderer draw calls]
    P --> Q[HAL display framebuffer update]
    Q --> R[E-ink refresh policy]

    S[SETTINGS singleton] -. influences .-> J
    S -. influences .-> M
    T[APP_STATE singleton] -. persists .-> U[Reading progress and resume context]
    U -. used by .-> I
```

Notes:

- CSS files are collected from the OPF manifest and, when needed, discovered by
  streaming ZIP paths under the OPF content base directory; the firmware avoids
  preloading the full ZIP central directory for large books.
- "section cache exists" depends on cache-busting parameters such as font,
  viewport size, paragraph alignment, hyphenation, embedded CSS, image rendering,
  and Focus Reading settings
- rendering favors reusing precomputed layout data to keep page turns responsive on constrained hardware
- progress/session state is persisted so the reader can reopen at the last position after reboot/sleep

## State and persistence

Two singletons are central:

- `src/CrossPointSettings.h` (`SETTINGS`): user preferences and behavior flags
- `src/CrossPointState.h` (`APP_STATE`): runtime/session state such as current book and sleep context

Typical persisted areas on SD:

```text
/.crosspoint/
  epub_<hash>/
    book.bin
    css_rules.cache
    progress.bin
    cover.bmp
    sections/*.bin
    img_* cache files
  settings.json
  state.json
```

`sections/*.bin` contains rendered pages plus anchor, paragraph, and list-item
lookup tables used for TOC/footnote jumps and KOReader sync refinement. For
binary cache formats, see `docs/file-formats.md`.

## Networking architecture

Network file transfer is controlled by `src/activities/network/CrossPointWebServerActivity.h` and served by `src/network/CrossPointWebServer.h`.

Modes:

- STA: join existing Wi-Fi network
- AP: create hotspot
- Calibre Wireless: STA flow specialized for Calibre plugin uploads

Server behavior:

- HTTP server on port 80
- WebSocket upload server on port 81
- WebDAV handler on the HTTP server
- UDP discovery listener for upload clients
- file operations backed by SD storage
- browser APIs for file management, settings, fonts, OPDS servers, and saved Wi-Fi networks
- activity requests faster loop responsiveness while server is running

Endpoint reference: `docs/webserver-endpoints.md`.

## Build-time generated assets

Some sources are generated and should not be edited manually.

- `scripts/build_html.py` generates `src/network/html/*.generated.h` from HTML files
- `scripts/gen_i18n.py` generates `lib/I18n/I18nKeys.h`, `I18nStrings.h`, and `I18nStrings.cpp`
- `scripts/generate_hyphenation_trie.py` generates hyphenation headers under `lib/Epub/Epub/hyphenation/generated/`

When editing related source assets, regenerate via normal build steps/scripts.

## Key directories

- `src/`: app orchestration, settings/state, and activity implementations
- `src/network/`: web server and OTA/update networking
- `src/components/`: theming and shared UI components
- `lib/hal/`: hardware abstraction wrappers around freeink-sdk
- `lib/Epub/`: EPUB parser, layout, CSS handling, and hyphenation
- `lib/`: supporting libraries (fonts, text, filesystem helpers, etc.)
- `freeink-sdk/`: hardware SDK submodule (display, input, storage, battery). Docs: https://freeink.org/docs
- `docs/`: user and technical documentation

## Embedded constraints that shape design

- constrained RAM drives SD-first caching and careful allocations
- e-ink refresh cost drives render/update batching choices
- main loop responsiveness matters for input, power handling, and watchdog safety
- background/network flows must cooperate with sleep and loop timing logic

## Scope guardrails

Before implementing larger ideas, check:

- [SCOPE.md](../../SCOPE.md)
- [GOVERNANCE.md](../../GOVERNANCE.md)
