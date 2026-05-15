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
└── packages/
    └── <package-id>/
        └── manifest.json
```

The first firmware implementation only discovers and displays valid manifests in Settings. Runtime loading, install
transactions, app launching, and permission enforcement should build on top of this store instead of introducing a
second package location.

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
