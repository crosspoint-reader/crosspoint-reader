# Marginalia Architecture

Marginalia is a reader-first fork of CrossPoint with a package ecosystem. The firmware stays the base product, while
community extensions live in a separate package model and hub.

## Goals

- Keep the Xteink reader experience fast and stable.
- Make side-loaded packages a first-class part of the platform.
- Avoid a monorepo for the ecosystem pieces that move at different speeds.
- Keep the package contract stable enough to survive future boards.

## Repo Map

### `marginalia-firmware`

The firmware product itself.

- device boot and recovery
- reader, file browser, settings, sleep, sync
- package install/update/remove
- package runtime for modules
- standalone app launch path
- xteink X3/X4 board support

### `marginalia-sdk`

Developer-facing package SDK.

- manifest schema
- permission names
- compatibility constants
- package scaffolds and examples
- validation helpers

### `marginalia-registry`

Metadata-only package index.

- package ids and versions
- checksums and signatures
- target compatibility
- release channels
- deprecation and replacement links

### `marginalia-hub`

Web catalog and package publishing service.

- browse and search packages
- submit and verify releases
- publish signed catalog snapshots
- expose docs, screenshots, and changelogs

### `marginalia-examples`

Reference packages.

- themes
- sleep-screen packages
- reader modules
- integrations
- sample apps

## RT-Thread Reference Model

Marginalia uses RT-Thread as the reference model for firmware extension architecture when it fits the project. The
important split is between core firmware, system components, extension packages, and optional runtime modules. Packages
are the distribution unit, not the user-facing mental model.

In firmware UI, this means extension-related features should be grouped under **Extensions** instead of exposing storage
format names such as packages or archive files. Normal device settings stay in Settings. Extension-specific configuration
belongs on the extension detail/configuration screens, with optional bridge rows from normal settings when that improves
discovery.

This follows RT-Thread's broad shape:

- firmware/BSP/core capabilities are compiled into the device
- system components provide reusable host services
- packages add optional capabilities selected by category
- runtime modules are introduced only when the firmware has an explicit loader/host for them

Marginalia maps that to:

- core firmware: reader, file browser, settings, Wi-Fi transfer, rendering, input, storage
- system components: package store, hub catalog client, theme host, future reader hook host, future app/module runtime
- extensions: themes, reader modules, sleep screens, integrations, and apps once app runtime support exists
- hub: catalog, archive download, metadata, compatibility, and documentation

## Package Model

Marginalia uses one package system with two execution classes:

- `module`: runs in the firmware host for themes, sleep screens, reader hooks, widgets, and integrations
- `app`: standalone experiences with their own navigation lifecycle

Package kinds:

- `theme`
- `sleep_screen`
- `reader_module`
- `integration`
- `app`

## Local Package Store

The firmware scans packages from the SD card:

```text
/.marginalia/
├── inbox/
│   └── <upload-folder>/
│       └── manifest.json
├── package-state/
│   └── <package-id>.json
└── packages/
    └── <package-id>/
        └── manifest.json
```

Wi-Fi side loading writes package folders into `inbox/`. The user then installs an inbox package, which validates the
manifest and moves it into `packages/` through a staging transaction. User-controlled lifecycle state, such as whether a
package is enabled, lives in `package-state/` so package upgrades can replace files without resetting user intent.
Runtime loading, app launching, and permission enforcement should build on top of this store instead of introducing a
second package location.

## Package Settings

Package settings are declared by the package manifest and stored by firmware as user state. This keeps extension
configuration in the same place as the rest of the extension lifecycle: the user opens **Extensions**, selects a package,
and configures package-owned settings from the package detail screen.

Manifest-declared settings are small typed values:

```json
{
  "settings": [
    {
      "id": "invertScreen",
      "label": "Invert screen",
      "type": "boolean",
      "default": true
    }
  ]
}
```

Firmware persists values in `/.marginalia/package-state/<package-id>.json`:

```json
{
  "schemaVersion": 1,
  "id": "org.marginalia.examples.dark-mode",
  "enabled": true,
  "settings": {
    "invertScreen": true
  }
}
```

The state file is not part of the package archive. Package upgrades must preserve existing state, including unknown
future settings, while new installs fall back to manifest defaults. Runtime hosts should read settings through
`PackageStore` helpers rather than parsing state files directly.

## Theme Host Contract

Theme packages can include `src/theme.json` to request firmware-hosted OS rendering behavior:

```json
{
  "schemaVersion": 1,
  "scope": "os",
  "mode": "invert-screen",
  "refreshMode": "half",
  "textAntialiasing": "package-setting"
}
```

The firmware host only applies this contract for enabled, compatible `theme` packages. `refreshMode: "half"` promotes
normal fast display updates to half refresh while the theme is active. `textAntialiasing: "off"` suppresses reader text
grayscale antialiasing without overwriting the user's saved setting. `textAntialiasing: "package-setting"` reads the
package boolean setting named `textAntialiasing`; this lets the theme expose an antialiasing toggle in its package
settings while still keeping the normal reader setting intact.

Theme host state is refreshed during firmware startup and package lifecycle changes. Rendering code reads only cached
theme flags; it must not scan package directories or parse package JSON from `displayBuffer()`.

## Compatibility Gate

Manifest `target` metadata is optional for early local packages. When present, firmware evaluates it before install and
marks active packages as compatible or incompatible during scans.

The current firmware accepts:

- `devices`: `xteink-x3` or `xteink-x4`
- `chipFamilies`: `esp32-c3`
- `apiLevel`: `1` or lower
- `requiresPSRAM`: `false`
- `minFirmware`: less than or equal to the running firmware version

Incompatible inbox packages are visible but cannot be installed. Incompatible active packages stay visible so the user
can disable or uninstall them instead of losing track of what is on the SD card.

## Manifest v1

```json
{
  "schemaVersion": 1,
  "id": "org.example.gameoflife",
  "name": "Game of Life",
  "version": "1.0.0",
  "kind": "sleep_screen",
  "execution": "module",
  "summary": "Animated Conway's Game of Life for sleep mode",
  "author": "Example",
  "license": "MIT",
  "target": {
    "devices": ["xteink-x3", "xteink-x4"],
    "chipFamilies": ["esp32-c3"],
    "minFirmware": "0.1.0",
    "apiLevel": 1,
    "ramClass": "low",
    "requiresPSRAM": false
  },
  "permissions": ["display", "sleep_state"],
  "dependencies": [],
  "entrypoints": {
    "onLoad": "init",
    "onUnload": "shutdown",
    "onSleepEnter": "renderFrame",
    "onSleepTick": "advance"
  },
  "assets": {
    "icon": "icon.png",
    "preview": "preview.png"
  },
  "integrity": {
    "sha256": "sha256-of-package",
    "signature": "signature-over-package"
  }
}
```

## Hub Contract

The hub is the catalog and distribution layer, not the runtime.

Firmware should:

1. fetch a signed catalog snapshot
2. filter packages by target and permissions
3. download a package from the hub or read one from SD
4. verify checksum and signature
5. install into local storage
6. register the package only after verification succeeds
7. disable a broken package instead of blocking boot

The hub should:

1. store package metadata
2. sign catalog snapshots
3. publish version/channel information
4. serve package archives
5. show compatibility and documentation

## v1 Boundary

The first release stays on Xteink X3/X4 and ESP32-C3. The architecture should still model apps from day one, but the
first packages can be lightweight modules and themes while the runtime hardens.
