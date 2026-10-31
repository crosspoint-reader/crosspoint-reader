# SD wallpaper collections

CrossPoint can install and select independent collections of custom sleep-screen BMPs. Collections are **not plugins** and cannot contain executable code.

## Creating and publishing a wallpaper pack

**Want to make your own pack?** Start with the [full wallpaper pack contract](https://github.com/jadehawk/starwars.crosspoint-wallpaper/blob/main/docs/WALLPAPER_PACK_CONTRACT.md). It is the publisher-facing specification for valid manifests (including the `collection` ID), image formats, ZIP contents, and validation. This document instead explains how CrossPoint installs, selects, and manages those packs.

- [Reference wallpaper pack repository](https://github.com/jadehawk/starwars.crosspoint-wallpaper) — a working example to follow.
- [Wallpaper pack AI agent guide](https://github.com/jadehawk/starwars.crosspoint-wallpaper/blob/main/docs/AI_AGENT_GUIDE.md) — instructions for building a compliant pack with a coding assistant.
- [Plugin Hub build and publishing guide](https://github.com/crosspoint-reader/crosspoint-plugin-hub/blob/main/docs/BUILD_AND_PUBLISH_EXTENSIONS.md#3-standalone-wallpaper-pack) — validation, packaging, GitHub release, and catalog discovery instructions.
- [Extended catalog schema](https://github.com/crosspoint-reader/crosspoint-plugin-hub/blob/main/docs/EXTENSION_CATALOG_SCHEMA.md#wallpaper-catalog) — catalog fields and resolution-specific variants.

Follow the reference project's validation and packaging steps before publishing a stable GitHub release with the `crosspoint-wallpaper` topic. Plugin Hub discovers compliant releases; firmware that advertises `wallpaper.packs.v1` selects the variant matching the actual screen resolution. No particular firmware brand or model name is required.

## On-SD layout

An installed package occupies exactly one directory:

```text
/.crosspoint/wallpapers/<variant-id>.crosspoint-wallpaper/
  manifest.json
  README.md
  DETAILS.md
  wallpapers/
    <image>.bmp
```

The manifest uses `kind: "wallpaper-pack"`, a lowercase/hyphenated `name` matching the variant directory, `title`, `version`, `target_resolution: {width,height}`, and two explicit arrays: `images` (paths under `wallpapers/`) and `files` (the three metadata paths plus all images). The installer accepts 1–32 BMPs and validates dimensions against the actual screen. Collections are discovered only under the wallpaper root; installed plugins, themes, and ordinary sleep images are not treated as collections. Installed collections also appear in the device's installed-items picker with their own removal action; they remain data-only assets and are never executed as plugins.

Up to eight installed collections are listed in Display → Sleep Screen when Custom or Cover + Custom is selected. The **Wallpaper Collection** picker offers the existing SD custom image pool by default, followed by each installed collection. The selected ID is stored in the settings file. Long-press a collection in this picker to remove it; removing the active collection restores the existing default pool. Unmounting the SD card alone does not erase a saved selection.

The existing `/sleep.bmp`, `/.sleep`, and `/sleep` sources are unchanged when no collection is selected, and provide fallback if a selected collection has no readable image.

Plugin Hub's WebUI may show **Installed Wallpaper Packs** below its plugin list, including the active selection and a Remove action. `GET /api/wallpaper-packs` lists recognized packs, and `POST /api/wallpaper-packs/delete` with `{"id":"<pack-id>"}` deletes only a validated installed wallpaper pack. The WebUI plugin loader (`/api/plugins`) remains limited to browser-side JavaScript plugins.

## Installing from a catalog

A firmware supporting `wallpaper.packs.v1` may consume a capability-filtered catalog list whose `kind` is `wallpaper-pack` and whose `items` field identifies the JSON array of collections. Each collection has a stable `name`, `version`, and a `variants` array. Each variant declares its own `name`, `target_resolution: {width,height}`, `asset_url`, `sha256`, and `bytes`.

The firmware matches the actual display's pixel dimensions, not device names. It downloads **only one** matching ZIP, verifies its SHA-256, requires an exact set of safely named files and an embedded manifest matching the variant ID/version/resolution, and validates every extracted BMP before swapping the staged directory into place. Failed verification preserves a previously installed version. A matching collection without a compatible variant cannot be installed.

The ZIP contains the single `<variant-id>.crosspoint-wallpaper/` directory prefix. The required files are `manifest.json`, `README.md`, `DETAILS.md`, and the declared images. No undeclared files, traversal, absolute paths, executable scripts, or extra directories are accepted. The firmware limits file counts, individual file sizes, compressed archive sizes, and expanded total bytes.

Catalogs are capability-gated, independent of publisher or firmware branding. Firmware without `wallpaper.packs.v1` should not display wallpaper lists. Existing plugin catalog behavior is preserved.
