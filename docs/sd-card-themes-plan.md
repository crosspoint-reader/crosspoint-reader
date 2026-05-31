# SD Card Downloadable Themes Plan

## Goal

Move every UI theme except the firmware fallback theme, Lyra, out of compiled
C++ and onto the SD card. Themes should behave like SD-card fonts:

- bundled Lyra remains available without an SD card
- installed themes are discovered from SD card folders
- downloadable themes can be installed over WiFi from a manifest
- users can create themes visually in `crosspoint-tools`
- the settings UI lists built-in Lyra plus installed SD themes
- missing or invalid selected themes fall back to Lyra safely

The first implementation should keep Lyra as the built-in base, migrate the old
Classic selection to Lyra, and move Lyra 3 Covers and RoundedRaff to SD-card
themes.

## Current State

Themes currently live as C++ classes:

- `BaseTheme` / `BaseMetrics` currently contains the old Classic shared drawing
  implementation, but Classic should not remain a user-visible theme
- `LyraTheme` overrides drawing behavior and metrics
- `Lyra3CoversTheme` inherits from Lyra and adjusts home-cover metrics
- `RoundedRaffTheme` overrides more drawing behavior and metrics
- `UITheme::setTheme()` hard-codes the enum to class mapping
- `SettingsList.h` hard-codes the UI theme enum labels

SD fonts already provide the model to copy:

- `SdCardFontRegistry` scans `/.fonts` and `/fonts`
- settings keep built-ins as enums and SD selections as a saved name
- `FontDownloadActivity` downloads a JSON manifest, installs files, refreshes
  the registry, and marks updates
- manual copy and web upload are both supported

## Recommended Direction

Use a declarative JSON theme format plus an optional `icons/` folder. Do not use
runtime HTML or runtime SVG as the main rendering format.

Reasons:

- The UI already renders through `GfxRenderer`, not a browser/layout engine.
- HTML would require a layout engine, CSS cascade, text measurement, hit testing,
  and a DOM-like retained tree. That is too much RAM and code for this device.
- Full SVG parsing has similar risk: paths, transforms, fills, strokes, viewBox,
  clipping, and scaling add a lot of parser and rasterizer complexity.
- JSON can map directly to the existing theme concepts: metrics, styles, row
  shapes, selection treatment, keyboard treatment, popup style, and icon names.

SVG can still be supported, but as an offline authoring input. The theme builder
should convert SVG icons to the same bitmap/header/raw monochrome format the
firmware can draw cheaply.

The user-facing authoring experience should live outside the firmware in
`crosspoint-tools`. Firmware should consume validated theme packages; it should
not try to be a visual editor, SVG renderer, CSS engine, or theme compiler.

## Proposed SD Card Layout

Scan hidden first, visible second, matching fonts:

```text
SD Card Root/
+-- .themes/
|   +-- LyraCarousel/
|       +-- theme.json
|       +-- preview.bmp
|       +-- icons/
|           +-- book.cpi
|           +-- folder.cpi
|           +-- settings.cpi
+-- themes/
    +-- RoundedRaff/
        +-- theme.json
        +-- icons/
```

Use hidden `/.themes` as the preferred install target and `/themes` as the
user-visible alternative.

## Theme JSON Shape

Start with a constrained schema, not general drawing commands:

```json
{
  "schema": 1,
  "id": "lyra-carousel",
  "name": "Lyra Carousel",
  "inherits": "lyra",
  "author": "CrossPoint",
  "version": "1.0.0",
  "metrics": {
    "topPadding": 5,
    "headerHeight": 45,
    "contentSidePadding": 20,
    "listRowHeight": 30,
    "listWithSubtitleRowHeight": 50,
    "buttonHintsHeight": 40,
    "homeRecentBooksCount": 1
  },
  "styles": {
    "header": {
      "battery": "right",
      "titleFont": "ui12",
      "subtitleFont": "ui10"
    },
    "list": {
      "selection": {
        "shape": "roundedRect",
        "radius": 8,
        "fill": "lightGray"
      },
      "icons": true
    },
    "keyboard": {
      "cornerRadius": 5,
      "fillUnselected": false,
      "outlineAllUnselected": true
    },
    "popup": {
      "cornerRadius": 8,
      "frameThickness": 2,
      "textBold": true,
      "textInverted": true
    }
  },
  "icons": {
    "book": "icons/book.cpi",
    "folder": "icons/folder.cpi",
    "settings": "icons/settings.cpi"
  },
  "devices": {
    "x3": {
      "constraints": {
        "screenWidth": 528,
        "screenHeight": 800,
        "frontButtons": 4,
        "sideButtons": "split"
      },
      "metrics": {
        "contentSidePadding": 24
      }
    },
    "x4": {
      "constraints": {
        "screenWidth": 480,
        "screenHeight": 800,
        "frontButtons": 4,
        "sideButtons": "rightStack"
      }
    }
  }
}
```

Keep the schema narrow at first. If a field is missing, inherit from Lyra.
If a field is unknown, ignore it so newer themes can remain forward-compatible.

Theme packages may include a top-level `devices` object for device-specific
overrides. The active device id should be stable and lowercase, currently `x3`
or `x4`. Common top-level fields are loaded first, then `devices.<deviceId>`
overrides are applied. This lets one theme package adapt cover geometry, menu
spacing, button-hint placement, and other constraints across devices with
different screen widths or button layouts.

Reserve extension namespaces so downstream firmware like CrossInk can share the
same theme packages while adding fields CrossPoint does not understand:

- top-level `extensions` object for vendor-specific data
- vendor keys such as `extensions.crossink`
- optional `requires`/`supports` metadata for feature negotiation
- unknown extension namespaces must be ignored by CrossPoint

Example:

```json
{
  "schema": 1,
  "id": "carousel",
  "name": "Carousel",
  "inherits": "lyra",
  "requires": {
    "crosspoint": {
      "schema": 1,
      "modules": ["carousel", "iconDock"]
    }
  },
  "extensions": {
    "crossink": {
      "readingStatsFooter": true,
      "bookmarkMenuIcon": "bookmarkRibbon"
    }
  }
}
```

CrossPoint should load the common fields and ignore `extensions.crossink`.
CrossInk can read the shared CrossPoint fields, then apply its own extension
fields when present.

## Rendering Architecture

Add a data-backed theme implementation:

- `ThemeDefinition`: parsed `theme.json`, normalized with Lyra defaults
- `SdCardThemeRegistry`: scans `/.themes` and `/themes`, validates folders
- `SdCardTheme`: inherits `BaseTheme` and renders from `ThemeDefinition`
- `SdCardThemeSystem`: owns registry, active definition, dirty/reload behavior
- `ThemeIconStore`: resolves icon names to built-in icons or SD icon files

Then change `UITheme`:

- keep Lyra compiled in as the firmware base/fallback theme
- refactor the current `BaseTheme` naming so "base" means shared renderer, not a
  user-visible Classic theme
- when selected theme is an SD name, load `SdCardTheme`
- if loading fails, clear the SD theme setting and fall back
- expose `registry()` for settings screens

The first `SdCardTheme` should not try to express arbitrary custom layouts. It
should parameterize the shared Lyra-compatible drawing functions and add a small
set of style switches needed to recreate RoundedRaff and carousel-style layouts.

## Settings Model

Mirror the SD font pattern:

- keep `uiTheme` for built-in firmware themes
- add `char sdThemeName[32] = ""`
- Lyra is always the first built-in option
- installed SD themes are appended after Lyra
- selecting Lyra clears `sdThemeName`
- selecting an SD theme writes its folder/name into `sdThemeName`
- old enum values migrate like this:
  - `LYRA` -> built-in Lyra
  - `CLASSIC` -> built-in Lyra
  - `LYRA_3_COVERS` -> `sdThemeName = "Lyra 3 Covers"` if installed
  - `ROUNDEDRAFF` -> `sdThemeName = "RoundedRaff"` if installed

Do not ship Classic as a separate first-party theme package. The old Classic
name is a compatibility alias for Lyra only.

## Downloadable Theme Flow

Create a theme equivalent of the font download flow:

- `ThemeDownloadActivity`
- `ThemeInstaller`
- remote `themes.json` manifest
- per-theme install/update/delete
- `Download all` and `Update all`
- CRC32 validation per file
- temp files during download, then atomic-ish rename
- refresh registry after install/delete

Manifest example:

```json
{
  "version": 1,
  "baseUrl": "https://crosspointreader.com/themes/",
  "themes": [
    {
      "id": "lyra-carousel",
      "name": "Lyra Carousel",
      "description": "Carousel home layout for Lyra",
      "version": "1.0.0",
      "files": [
        { "name": "theme.json", "size": 4096, "crc32": 1234567890 },
        { "name": "icons/book.cpi", "size": 256, "crc32": 2345678901 }
      ]
    }
  ]
}
```

Use the same website/repo pattern as `crosspoint-fonts`, likely a
`crosspoint-themes` repo that publishes theme folders and `themes.json`.

## Crosspoint Tools Theme Builder

Add a browser-based theme builder to the `crosspoint-tools` repo. This should be
the primary way for users to create custom SD-card themes without hand-editing
JSON.

Core builder features:

- visual controls for theme metadata: name, id, author, version, description
- form controls for supported metrics and style fields
- live previews for the main firmware surfaces:
  - home screen
  - file browser/list screen
  - settings list
  - popup/progress dialog
  - keyboard/text field
  - reader status bar/progress bar
- import an existing theme package or `theme.json`
- validate against the firmware-supported schema version
- show warnings for unsupported fields, missing icons, oversized assets, invalid
  names, and values that would not render well on X3/X4 screens
- export either a folder layout or a single `.zip` package

The exported package should match the SD-card runtime layout:

```text
LyraCarousel/
+-- theme.json
+-- preview.bmp
+-- icons/
    +-- book.bmp
    +-- folder.bmp
    +-- settings.bmp
```

The builder should produce a ZIP whose root contains exactly one theme folder,
so users can either:

- unzip/copy the folder manually to `/.themes/` or `/themes/`
- upload the ZIP through the CrossPoint web server
- download the ZIP from the builder and drag it onto the SD card

Builder output should include a small generated `manifest.json` inside the theme
folder only if the firmware needs per-file sizes/checksums for upload-time
validation. Otherwise the remote catalog `themes.json` remains the source of
download/update checksums.

### Builder Icon Library

The builder should include an icon library so users do not need to draw every
firmware icon from scratch.

Requirements:

- ship a curated set of monochrome icons for every current `UIIcon` role:
  `folder`, `text`, `image`, `book`, `file`, `recent`, `settings`, `transfer`,
  `library`, `wifi`, `hotspot`, and `bookmark`
- let users pick icons by role and preview them at firmware scale
- support importing user SVG, PNG, or BMP icons
- convert imported/vector icons in the browser or build pipeline to the exact
  firmware runtime bitmap format
- generate all required sizes/variants automatically
- enforce 1-bit monochrome output, transparent/background handling, padding, and
  safe dimensions
- report conversion problems before export

Prefer BMP or a very small custom `.cpi` format for the firmware runtime. If BMP
is chosen, standardize the exact constraints the firmware accepts: 1-bit or
8-bit grayscale, maximum dimensions, no compression, and top-down/bottom-up
handling. If `.cpi` is chosen, the builder should hide that detail from users
and simply export valid icon files.

The builder can parse SVG because it runs on desktop/mobile browsers with much
more RAM and CPU than the reader. The firmware should receive only pre-rendered
bitmap icon files.

### Builder Package Validation

Before export, `crosspoint-tools` should run the same validation rules that the
firmware will enforce:

- folder/id/name characters are safe: alphanumeric, hyphen, underscore, and
  spaces only if we explicitly support spaces
- no absolute paths or `..` entries
- `theme.json` schema version is supported
- all referenced icon files exist
- bitmap dimensions and format are firmware-compatible
- package size is reasonable for SD-card upload
- preview renders successfully for X3 and X4 dimensions

Keep a shared JSON schema file in the tools repo, and copy or vendor it into
the firmware repo tests if needed. The browser builder and firmware parser
should reject the same malformed packages.

### Web Server Upload Flow

Add theme ZIP upload support to the CrossPoint web server alongside font
management.

Device-side behavior:

- add a Themes page or extend the Fonts page pattern into a generic
  customization manager
- accept a `.zip` theme package upload
- write the upload to a temporary file on SD card first
- validate ZIP structure before installing:
  - one top-level theme folder
  - required `theme.json`
  - allowed file extensions only
  - no path traversal entries
  - size limits per file and per package
- extract/install into `/.themes/<ThemeName>/`, reusing `/themes` if that theme
  already exists there
- validate the installed `theme.json` and icon files
- refresh the SD theme registry
- if the uploaded theme replaces the active theme, reload it
- expose delete support for installed SD themes

The firmware already has `ZipFile`; confirm it supports the extraction pattern
needed here. If not, either add a constrained ZIP extractor for stored/deflated
small files or have the web page unzip client-side with JSZip and upload the
individual files to theme-specific endpoints.

Client-side unzip is attractive because `FilesPage.html` already ships JSZip,
but device-side validation is still required. Browser-side checks should improve
UX; firmware checks are the security boundary.

## Icon Format

Preferred firmware runtime format:

- simple monochrome BMP or custom `.cpi`
- fixed, documented constraints either way
- 1-bit pixels preferred, optionally RLE-compressed if `.cpi` is used
- direct draw path in `GfxRenderer`

Authoring formats:

- SVG accepted by the host-side theme builder
- PNG/BMP accepted by the host-side theme builder
- builder converts to firmware-ready BMP or `.cpi`

Do not make firmware parse arbitrary SVG in the first version. A tiny SVG subset
could be added later for simple icons, but it should not block the core theme
system.

## JSON vs Drawing Commands

There are two possible levels of JSON support:

1. Style JSON, recommended first.
   This stores metrics and component styles. Existing C++ still owns layout and
   drawing logic.

2. Layout module JSON, needed for advanced themes.
   This selects firmware-provided component renderers, such as a recent-books
   carousel, icon-only bottom dock, or alternate list selection style, and passes
   bounded numeric/style parameters into those renderers.

3. Scene JSON, possible later.
   This stores primitive drawing commands such as rect, line, text, icon, and
   conditional groups.

Start with style JSON plus a small set of named layout modules. Scene JSON is
more flexible, but it becomes a mini UI language and will need validation,
layout variables, text measurement rules, conditional logic, and compatibility
guarantees.

## Advanced Theme Compatibility

The plan should support themes shaped like CrossInk's Lyra Carousel without
requiring firmware to load arbitrary C++ from the SD card. That kind of theme is
not just a set of metrics; it changes component behavior:

- the home screen recent-books area becomes a carousel with center and side
  covers
- side covers use perspective/skewed rendering
- the selected book gets dots and title wrapping
- the home menu becomes a bottom icon dock with one selected label
- list rows use a black selected highlight with inverted text/icons
- selection overlay drawing may be separate from full home redraws

CrossInk-specific features that are not present in CrossPoint, such as reading
stats, should be ignored. The compatibility target is the layout: carousel
covers, title placement, indicator dots, bottom icon dock, and selection styling.

Support this with built-in layout modules selected by JSON:

```json
{
  "inherits": "lyra",
  "metrics": {
    "homeRecentBooksCount": 3,
    "homeCoverHeight": 600,
    "homeCoverTileHeight": 660,
    "menuRowHeight": 64,
    "keyboardKeyHeight": 50,
    "keyboardCenteredText": true
  },
  "components": {
    "homeRecents": {
      "module": "carousel",
      "centerCover": {
        "maxWidth": 340,
        "maxHeight": 540,
        "visualInset": 10,
        "cornerRadius": 6,
        "selectionLineWidth": 3
      },
      "sideCovers": {
        "enabled": true,
        "nearWidthPercent": 26,
        "farWidthPercent": 21,
        "perspective": true
      },
      "title": {
        "font": "ui12",
        "style": "bold",
        "maxLines": 2
      },
      "indicators": {
        "type": "dots",
        "size": 8,
        "gap": 6
      },
      "footer": {
        "progressBar": false
      }
    },
    "homeMenu": {
      "module": "iconDock",
      "iconSize": 32,
      "selectedShape": "roundedRect",
      "selectedFill": "black",
      "selectedIcon": "inverted",
      "labelMode": "selectedOnly"
    },
    "list": {
      "selection": {
        "fill": "black",
        "text": "inverted",
        "icons": "inverted"
      }
    }
  }
}
```

Any component or metric under `devices.x3` / `devices.x4` should override the
common value for that device. For example, a carousel can use wider side-cover
spacing on X3 while keeping tighter X4 geometry in the same package.

Firmware still owns the `carousel`, `iconDock`, and `list` renderers. JSON only
chooses supported modules and parameters. If a theme asks for an unknown module,
fall back to the inherited Lyra component and log the reason.

Add a typed context object for richer renderers instead of growing method
signatures one theme at a time. For example:

- `ThemeHomeContext`: recent books, selected index, active row, cached-cover
  flags, optional progress percent, and callbacks
- `ThemeMenuContext`: labels, icons, selected index, available rect
- `ThemeListContext`: titles, subtitles, values, icons, dim/header flags

Existing `BaseTheme`/`LyraTheme` virtual methods can be adapted internally, but
the SD theme renderer should consume these context structs. That gives advanced
modules the data they need while keeping the public theme schema declarative.

The `crosspoint-tools` builder should expose these modules as presets. For
example, "Home recents: Lyra Carousel" should reveal only the parameters that
the firmware supports and should preview the same fallback behavior used on the
device. Features unavailable in CrossPoint should be absent from the builder or
shown as disabled, not exported into the package.

For CrossInk compatibility, the builder should preserve unknown `extensions.*`
objects when importing and re-exporting a package. It may display them as
read-only advanced metadata, but it should not strip them unless the user
explicitly chooses to export a CrossPoint-only package.

## Migration Phases

### Phase 1: Inventory and Normalize

- Compare `BaseTheme`, `LyraTheme`, `Lyra3CoversTheme`, and `RoundedRaffTheme`.
- Include CrossInk-style advanced themes, especially carousel home/menu/list
  behavior, in the inventory so the schema does not stop at simple metrics.
- List every metric override and every method override.
- Classify differences as:
  - pure metrics
  - simple style switches
  - icon substitutions
  - genuinely custom drawing behavior
- Refactor shared drawing helpers so Lyra can be the built-in base and Classic
  disappears as a user-visible theme.

### Phase 2: SD Theme Registry

- Add `SdCardThemeRegistry` using the SD font registry structure.
- Scan `/.themes` and `/themes`.
- Validate `theme.json` exists and has supported schema/name/id.
- De-duplicate by theme id or folder name, hidden root wins.
- Sort themes alphabetically and cap count.

### Phase 3: Parser and Data Model

- Add a small parser using ArduinoJson.
- Parse into `ThemeDefinition`.
- Apply Lyra defaults before loading overrides.
- Add a `components` section for named firmware layout modules.
- Add a `devices` section for per-device metrics/components/constraints.
- Add forward-compatible `extensions` and `requires` handling; unknown extension
  namespaces must be preserved by tools and ignored by firmware.
- Reject dangerous values: negative sizes, huge dimensions, impossible row
  heights, oversized names, missing required fields.
- Add unit tests for valid, partial, unknown-field, and invalid JSON.

### Phase 4: Data-Backed Theme Renderer

- Add `SdCardTheme : BaseTheme`.
- Add typed theme context structs for home recents, menus, and lists.
- Move parameterizable drawing decisions out of hard-coded Lyra/RoundedRaff code
  and into style fields.
- Keep unsupported custom behavior falling back to Lyra.
- Recreate Lyra 3 Covers as JSON inheriting Lyra.
- Recreate RoundedRaff as JSON after the Lyra path is stable.
- Add a built-in `carousel` home-recents module and `iconDock` menu module before
  claiming compatibility with CrossInk-style carousel themes.

### Phase 5: Settings Integration

- Add `sdThemeName` to settings and JSON settings IO.
- Build the UI theme setting dynamically, like `buildFontFamilySetting()`.
- Update web settings API if it assumes fixed enum values.
- On selection change, reload the active theme.
- On missing selected SD theme, clear the setting and choose fallback.

### Phase 6: Download/Install UI

- Add theme manifest URL and manifest parser.
- Add `ThemeInstaller`.
- Add `ThemeDownloadActivity`.
- Add a settings action near display/theme settings, similar to Manage Fonts.
- Add web upload/delete support for installed theme packages.

### Phase 7: Packaging Tools

- Add the visual theme builder to `crosspoint-tools`.
- Add a shared JSON schema for theme validation.
- Preserve unknown `extensions.*` fields on import/export for downstream
  firmware compatibility.
- Add preview rendering for X3/X4 surfaces.
- Add builder presets for built-in layout modules, including carousel home
  recents and icon dock menu.
- Add SVG/PNG/BMP to firmware-ready BMP or `.cpi` icon conversion.
- Add an icon library that covers every current `UIIcon` role.
- Add theme ZIP export and import.
- Add manifest generation with sizes and CRC32.
- Package first-party themes into a repo or release artifact:
  - Lyra 3 Covers
  - RoundedRaff

### Phase 8: Remove Compiled Themes

- Keep Lyra compiled.
- Remove or disable compiled Lyra 3 Covers/RoundedRaff after SD themes
  are shipped and migration is proven.
- Update docs and README theme instructions.
- Record firmware size before and after each major step:
  - baseline before SD theme support
  - after registry/parser/settings support
  - after web upload/download support
  - after removing compiled Lyra 3 Covers/RoundedRaff
- Include total flash usage, RAM usage if available from the build output, and
  the byte delta in PR notes.

## Compatibility and Fallback Rules

- Lyra must work with no SD card.
- Boot must not fail because a theme is malformed.
- Theme parsing should happen from a file, not by loading large strings into RAM.
- Bad installed theme: ignore it in registry and log why.
- Bad selected theme: clear `sdThemeName`, save settings, fall back to Lyra.
- Unsupported schema: show installed theme as unavailable or ignore it.
- Newer unknown fields: ignore them.

## Open Decisions

- Whether any third-party theme packages still refer to `classic`; if so, treat
  that id as an alias for Lyra.
- Whether theme ids are folder names, JSON ids, or both.
- Whether icon files should be raw 1-bit, RLE, or existing bitmap headers.
- Whether downloadable themes live in `crosspoint-fonts`, a new
  `crosspoint-themes`, or the main site repo.
- Whether theme ZIP extraction happens device-side or browser-side with JSZip
  plus validated per-file upload endpoints.
- Whether the builder lives as a route on the public CrossPoint site, a static
  app in `crosspoint-tools`, or both.

## Suggested First Milestone

Implement enough of the JSON style schema to express Lyra 3 Covers as a
data-backed theme while keeping compiled Lyra as the base. That proves
discovery, parsing, settings, and active theme reload without risking the
default UI. In parallel, scaffold the `crosspoint-tools` theme builder with
schema validation and static preview data, even if icon conversion starts with
the built-in icon library only.
