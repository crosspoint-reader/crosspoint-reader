# Build Configuration Guide

CrossPoint Reader supports customizable firmware builds, allowing you to include only the features you need and save precious flash memory space on your ESP32-C3 device.

## Table of Contents

- [Quick Start](#quick-start)
- [Feature Reference](#feature-reference)
- [Build Profiles](#build-profiles)
- [Local Build Instructions](#local-build-instructions)
- [GitHub Actions Builds](#github-actions-builds)
- [Flash Memory Considerations](#flash-memory-considerations)
- [Troubleshooting](#troubleshooting)

## Quick Start

### Using the Feature Picker (Easiest)

1. Visit [Feature Picker](https://unintendedsideeffects.github.io/crosspoint-reader/configurator/)
2. Select your desired features or choose a profile
3. Click "Build on GitHub Actions"
4. Wait ~5-10 minutes for the build to complete
5. Download the firmware artifact and flash to your device

### Using Command Line (Local Builds)

```bash
# Generate configuration for standard profile
uv run python scripts/generate_build_config.py --profile standard

# Build custom firmware
uv run pio run -e custom

# Flash to device
uv run pio run -e custom --target upload
```

## Feature Reference

### Extended Fonts

**Flag:** `ENABLE_EXTENDED_FONTS`
**Size Impact:** ~2.5MB
**Default:** Enabled

Includes additional Bookerly and Noto Sans font sizes.

**What's included when enabled:**
- Bookerly 12pt, 16pt, 18pt (Regular, Bold, Italic, Bold-Italic)
- Noto Sans 12pt, 14pt, 16pt, 18pt (Regular, Bold, Italic, Bold-Italic)

**What's always included:**
- Bookerly 14pt (default reading font)
- Ubuntu 10pt, 12pt (UI fonts)
- Noto Sans 8pt (small UI text)

**When disabled:**
- Only the default 14pt reading size and UI fonts are available
- Settings will not show unavailable font sizes

**Use case:** Disable if you only read at 14pt and want maximum space savings.

---

### OpenDyslexic Font Pack

**Flag:** `ENABLE_OPENDYSLEXIC_FONTS`
**Size Impact:** ~1.0MB
**Default:** Disabled
**Depends on:** `ENABLE_EXTENDED_FONTS`

Adds OpenDyslexic 8pt, 10pt, 12pt, and 14pt fonts.

**Use case:** Enable if you want the OpenDyslexic reading family.

---

### PNG/JPEG Sleep Images

**Flag:** `ENABLE_IMAGE_SLEEP`
**Size Impact:** ~33KB
**Default:** Enabled

Enables PNG and JPEG format support for custom sleep screen images.

**What's included when enabled:**
- PNG decoder (using PNGdec library)
- JPEG decoder (using picojpeg library)
- Support for `.png`, `.jpg`, `.jpeg` sleep images

**What's always included:**
- BMP image support (native to display library)
- Cover-based sleep screens (from EPUB covers)

**When disabled:**
- Sleep images folder only accepts `.bmp` files
- PNG/JPEG images in the sleep folder are ignored
- Cover sleep screens still work (covers are converted to display format)

**Use case:** Disable if you only use BMP sleep images or cover-based sleep screens.

---

### Book Images

**Flag:** `ENABLE_BOOK_IMAGES`
**Size Impact:** ~0KB
**Default:** Enabled

Controls inline image rendering inside EPUB and Markdown books.

**When enabled:**
- `<img>` content in EPUB chapters is rendered inline
- Markdown image syntax (`![alt](path)`) renders inline images when supported

**When disabled:**
- Inline images are replaced with fallback text labels
- Text layout, navigation, and chapter rendering remain available

**Use case:** Disable for text-only reading behavior or to troubleshoot problematic inline image content.

---

### Markdown/Obsidian

**Flag:** `ENABLE_MARKDOWN`
**Size Impact:** ~158KB
**Default:** Enabled

Full Markdown rendering with Obsidian vault compatibility.

**What's included when enabled:**
- Markdown parser (md4c library)
- HTML renderer for Markdown
- Obsidian-specific features:
  - Wikilinks (`[[Page]]`)
  - Callouts (note, warning, tip, etc.)
  - Embedded notes
  - Metadata frontmatter
- Markdown extensions:
  - Tables
  - Task lists
  - Strikethrough
  - Footnotes

**When disabled:**
- `.md` files show "Markdown support not available" message
- Todo feature defaults to `.txt` format instead of `.md`
- Obsidian vaults cannot be opened

**Use case:** Disable if you only read EPUB/TXT files and never use Markdown notes.

---

### KOReader Sync

**Flag:** `ENABLE_KOREADER_SYNC`
**Size Impact:** ~2KB
**Default:** Disabled
**Depends on:** `ENABLE_INTEGRATIONS`

Syncs reading progress with KOReader-compatible metadata.

**When disabled:**
- KOReader sync actions are unavailable
- Core EPUB/TXT/Markdown reading is unaffected

**Use case:** Enable only if you actively use KOReader sync flows.

---

### Calibre Sync

**Flag:** `ENABLE_CALIBRE_SYNC`
**Size Impact:** ~17KB
**Default:** Disabled
**Depends on:** `ENABLE_INTEGRATIONS`

Syncs metadata and reading progress with Calibre.

**When disabled:**
- Calibre sync actions are unavailable
- Core reader behavior is unchanged

**Use case:** Enable only if you use Calibre integration.

---

### Integrations Base

**Flag:** `ENABLE_INTEGRATIONS`
**Size Impact:** ~0KB
**Default:** Disabled

Shared runtime hooks required by remote sync integrations.

**When disabled:**
- KOReader and Calibre sync features are forced off
- Core reading behavior is unchanged

**Use case:** Enable only when you need KOReader sync and/or Calibre OPDS flows.

---

### Background Web Server

**Flag:** `ENABLE_BACKGROUND_SERVER`
**Size Impact:** ~4KB
**Default:** Enabled

Keeps the WiFi file management server running in the background while reading.

**What's included when enabled:**
- Background web server continues running during reading
- File uploads possible while reading a book
- WiFi stays connected in reading mode (if USB plugged in)

**When disabled:**
- Web server only runs in Home/Library views
- Reading automatically stops the web server
- Minimal power/memory impact

**Use case:** Disable for slightly lower memory usage if you never upload files while reading.

---

### Home Media Picker

**Flag:** `ENABLE_HOME_MEDIA_PICKER`
**Size Impact:** ~0KB
**Default:** Enabled

Replaces the classic Home list selector with a streamlined media-style layout:

- Left/Right controls the horizontal recent-book shelf
- Up/Down controls the vertical action menu
- Confirm opens the highlighted menu action

**When disabled:**
- Home screen uses the legacy "Continue Reading + vertical menu" layout
- Navigation falls back to single-axis menu selection

**Use case:** Disable if you prefer the legacy Home behavior.

---

### Web Pokedex Plugin

**Flag:** `ENABLE_WEB_POKEDEX_PLUGIN`
**Size Impact:** ~34KB
**Default:** Disabled

Adds an optional browser-side wallpaper generator page at `/plugins/pokedex`:

- Runs in the browser, not on the device CPU
- Generates grayscale X4 wallpapers from PokeAPI data
- Uploads directly to `/sleep/pokedex` using the existing web upload API
- Can optionally use a baked local cache for offline / low-latency firmware previews

Optional baked cache workflow:

1. Export `pokemon_cache.json` from the companion `pokedex.html` tool.
2. Run `python scripts/inject_pokemon_cache.py /path/to/pokemon_cache.json`
3. Build normally with `pio run`

The cache is stored in a local sidecar file and injected during `scripts/build_html.py`;
the source `PokedexPluginPage.html` stays unchanged.

**When disabled:**
- `/plugins/pokedex` route is not registered
- Files page does not show the Pokedex plugin launcher button

**Use case:** Enable when you want integrated Pokedex wallpaper creation in the device web UI.

---

### Pokemon Party

**Flag:** `ENABLE_POKEMON_PARTY`
**Size Impact:** ~4KB
**Default:** Disabled
**Depends on:** `ENABLE_WEB_POKEDEX_PLUGIN`

Builds on the web Pokedex plugin and recent-books cache to create a lightweight
Pokemon companion layer:

- Adds per-book `pokemon.json` sidecars in the existing book cache
- Exposes `GET`/`PUT`/`DELETE /api/book-pokemon`
- Extends `/api/recent` with saved Pokemon metadata plus real cached progress
- Turns recent books into a six-slot party surface in the Fork Drift home flow
- Bakes per-book cover+sprite visuals into `/sleep/pokedex/party` for both the recent-party slots and the sleep screen when a Pokemon is assigned from the web UI

**When disabled:**
- `/api/book-pokemon` routes are not registered
- The device Settings `Pokedex` shortcut is hidden
- `/plugins/pokedex` remains just the wallpaper/plugin surface when the web plugin is enabled

**Use case:** Enable when you want recent books to behave like a Pokemon party,
with reading progress driving level and evolution state.

---

## Build Profiles

### Lean Profile

**Size:** ~2.6MB (~3.8MB savings from full profile)

```bash
uv run python scripts/generate_build_config.py --profile lean
```

**Features:**
- ✗ Extended Fonts
- ✗ PNG/JPEG Sleep
- ✗ Markdown/Obsidian
- ✗ Background Server
- ✗ Home Media Picker
- ✗ Web Pokedex Plugin
- ✗ Pokemon Party

**Best for:**
- Devices with very limited flash space
- Users who only need basic EPUB reading at 14pt
- Maximum storage for books

---

### Standard Profile (Recommended)

**Size:** ~6.2MB

```bash
uv run python scripts/generate_build_config.py --profile standard
```

**Features:**
- ✓ Extended Fonts
- ✓ PNG/JPEG Sleep
- ✗ Markdown/Obsidian
- ✗ Integrations Base
- ✗ KOReader Sync
- ✗ Calibre Sync
- ✓ Background Server
- ✓ Home Media Picker
- ✗ Web Pokedex Plugin
- ✗ Pokemon Party

**Best for:**
- Most users
- Good balance of features and flash space
- Includes essential reading features plus background file access

---

### Full Profile

**Size:** ~6.4MB (feature-rich build, tight fit)

```bash
uv run python scripts/generate_build_config.py --profile full
```

**Features:**
- ✓ Extended Fonts
- ✓ PNG/JPEG Sleep
- ✓ Markdown/Obsidian
- ✓ Integrations Base
- ✓ KOReader Sync
- ✓ Calibre Sync
- ✓ Background Server
- ✓ Home Media Picker
- ✓ Web Pokedex Plugin
- ✗ Pokemon Party

**Note:** `Pokemon Party` is available for custom builds but is not enabled in
the stock `full` profile. Enable it explicitly if you want the recent-books
party flow.

**Best for:**
- Users who want most built-in features
- Devices with adequate flash space remaining
- Power users who use Markdown/Obsidian

---

## Local Build Instructions

### Prerequisites

- PlatformIO Core or VS Code with PlatformIO IDE
- Python 3.11+
- USB-C cable for flashing

### Generate Configuration

The `generate_build_config.py` script creates a `platformio-custom.ini` file with your selected features.

**Using profiles:**
```bash
# Lean build
uv run python scripts/generate_build_config.py --profile lean

# Standard build
uv run python scripts/generate_build_config.py --profile standard

# Full build
uv run python scripts/generate_build_config.py --profile full
```

**Custom feature selection:**
```bash
# Start from lean, add specific features
uv run python scripts/generate_build_config.py --profile lean --enable extended_fonts --enable image_sleep

# Start from full, remove specific features
uv run python scripts/generate_build_config.py --profile full --disable markdown

# Enable only markdown
uv run python scripts/generate_build_config.py --enable markdown

# Enable KOReader sync (auto-enables integrations base)
uv run python scripts/generate_build_config.py --enable koreader_sync
```

**List available features:**
```bash
uv run python scripts/generate_build_config.py --list-features
```

### Build and Flash

```bash
# Build the custom firmware
uv run pio run -e custom

# Build and flash in one command
uv run pio run -e custom --target upload

# Build and monitor serial output
uv run pio run -e custom --target upload --target monitor
```

### Verify Build Size

After building, check the firmware size:

```bash
# On Linux/macOS
ls -lh .pio/build/custom/firmware.bin

# Or use PlatformIO
uv run pio run -e custom -t size
```

---

## GitHub Actions Builds

GitHub Actions provides cloud-based builds without requiring local build tools.

### Using the Feature Picker

1. Go to [Feature Picker](https://unintendedsideeffects.github.io/crosspoint-reader/configurator/)
2. Configure your features
3. Click "Build on GitHub Actions"
4. Sign in to GitHub if prompted
5. Run the workflow
6. Wait for the build (typically 5-10 minutes)
7. Download the artifact from the Actions page

> **Note on the Fork:** The web configurator and automated builds are primarily supported on the `fork-drift` branch of this fork. For more details on our branch management and relationship with upstream, see [docs/fork-strategy.md](fork-strategy.md).

### Manual Workflow Trigger

1. Go to your fork's Actions tab
2. Select "Build Custom Firmware" workflow
3. Click "Run workflow"
4. Select your branch
5. Choose profile or toggle individual features
6. Click "Run workflow"
7. Wait for completion
8. Download the `custom-firmware` artifact

### Artifact Contents

The downloaded artifact contains:
- `firmware.bin` - Flash this to your device
- `partitions.bin` - Partition table (usually not needed for OTA)
- `platformio-custom.ini` - Configuration used for this build

---

## Flash Memory Considerations

### ESP32-C3 Flash Layout

The ESP32-C3 in the Xteink X4 has:
- **Total flash:** 16MB
- **Available for firmware:** ~6.4MB
- **Firmware partition:** 6MB (0x600000 bytes)
- **OTA partition:** 6MB (second firmware slot)

### Size Guidelines

| Build Type | Size | Flash Usage | Books Space |
|------------|------|-------------|-------------|
| Lean | ~2.6MB | 41% | Maximum |
| Standard | ~6.2MB | 97% | Good |
| Full | ~6.4MB | 99-100% | Tight |

*Note: Full build currently fits, but leaves very little headroom. Test before deploying.

### Tips for Managing Flash Space

1. **Start with Standard profile** - best balance for most users
2. **Disable unused features** - save space for more books
3. **Use BMP sleep images** - if you don't need PNG/JPEG
4. **Skip Extended Fonts** - largest single feature at ~2.5MB
5. **Monitor OTA updates** - custom builds may be larger than default

---

## Troubleshooting

### Build Fails with "firmware.bin is too large"

**Problem:** The configured features result in firmware larger than the partition.

**Solutions:**
1. Disable one or more features
2. Use a smaller profile (Standard instead of Full)
3. Specifically disable large features like Extended Fonts (~2.5MB)

Example:
```bash
uv run python scripts/generate_build_config.py --profile standard
```

### Feature Not Working After Flash

**Problem:** A feature you expected to be enabled isn't working.

**Check:**
1. Verify the feature was enabled in `platformio-custom.ini`
2. Rebuild and flash again
3. Clear device settings (may have cached old behavior)

### Markdown Files Show Error

**Problem:** "Markdown support not available" message appears.

**Cause:** `ENABLE_MARKDOWN=0` in your build.

**Solution:** Rebuild with Markdown enabled:
```bash
uv run python scripts/generate_build_config.py --enable markdown
uv run pio run -e custom --target upload
```

### Sleep Images Not Loading

**Problem:** PNG/JPEG sleep images not displaying.

**Cause:** `ENABLE_IMAGE_SLEEP=0` in your build.

**Solutions:**
1. Rebuild with image sleep enabled:
   ```bash
   uv run python scripts/generate_build_config.py --enable image_sleep
   uv run pio run -e custom --target upload
   ```
2. Or convert your images to BMP format (works in all builds)

### GitHub Actions Build Fails

**Problem:** Workflow fails during build.

**Common causes:**
1. Invalid feature combination (rare)
2. Branch has compilation errors
3. Repository not properly forked

**Solutions:**
1. Check the Actions log for specific errors
2. Try a known-good profile (standard)
3. Ensure your fork is up to date with upstream

### How to Check Current Build Configuration

Unfortunately, there's no runtime feature detection yet. To know what features are in your current firmware:

1. Check the GitHub Actions build summary (if built on Actions)
2. Check your local `platformio-custom.ini` file
3. Try using a feature - disabled features show error messages

*Future enhancement: Settings → About screen will show enabled features*

---

## Advanced Usage

### Modifying Build Flags Directly

If you need fine-grained control, edit `platformio-custom.ini` directly:

```ini
[env:custom]
extends = base
build_flags =
  ${base.build_flags}
  -DCROSSPOINT_VERSION="${crosspoint.version}-custom"
  -DENABLE_EXTENDED_FONTS=1
  -DENABLE_IMAGE_SLEEP=1
  -DENABLE_BOOK_IMAGES=1
  -DENABLE_MARKDOWN=0
  -DENABLE_INTEGRATIONS=0
  -DENABLE_KOREADER_SYNC=0
  -DENABLE_CALIBRE_SYNC=0
  -DENABLE_BACKGROUND_SERVER=0
  -DENABLE_HOME_MEDIA_PICKER=1
  -DENABLE_WEB_POKEDEX_PLUGIN=0
```

### Adding Your Own Feature Flags

To add a new optional feature:

1. Add the flag definition in your code:
   ```cpp
   #ifndef ENABLE_MY_FEATURE
   #define ENABLE_MY_FEATURE 1
   #endif
   ```

2. Wrap the feature code:
   ```cpp
   #if ENABLE_MY_FEATURE
   // Feature implementation
   #endif
   ```

3. Add to `generate_build_config.py`:
   ```python
   'my_feature': Feature(
       name='My Feature',
       flag='ENABLE_MY_FEATURE',
       size_kb=100,
       description='My custom feature'
   )
   ```

4. Update profiles as needed

---

## Related Documentation

- [README.md](../README.md) - Main project documentation
- [USER_GUIDE.md](../USER_GUIDE.md) - User guide for operating CrossPoint
- [Feature Picker](https://unintendedsideeffects.github.io/crosspoint-reader/configurator/) - Web-based configuration tool

---

**Questions or Issues?**

- Check [Troubleshooting](#troubleshooting) section above
- Open an issue on GitHub
- Consult the main [README.md](../README.md)

---

## Feature Store OTA Catalog

The Feature Store provides over-the-air firmware bundles with different feature configurations. The catalog is a JSON file hosted alongside release artifacts.

### Catalog Format

Each bundle entry contains:
- `id` — Unique bundle identifier (e.g., `stable-default`)
- `displayName` — Human-readable name shown in the OTA picker UI
- `version` — Version tag or `latest`/`nightly`
- `board` — Target board (must match device, e.g., `esp32s3`)
- `featureFlags` — Comma-separated compile-time feature flags included in the bundle
- `downloadUrl` — Direct URL to the firmware binary
- `checksum` — SHA-256 of the binary (empty string if not yet computed)
- `binarySize` — Size in bytes (0 if not yet known)

### Compatibility Checks

Before offering a bundle for installation, the device checks:
1. **Board match** — `bundle.board` must equal the device's configured board
2. **Partition size** — `bundle.binarySize` (if non-zero) must fit in the OTA partition

Incompatible bundles are shown in the picker with a warning and cannot be selected.

### Catalog Location

The catalog JSON is stored at `docs/ota/feature-store-catalog.json` in the repository and served from the GitHub releases page at runtime.
