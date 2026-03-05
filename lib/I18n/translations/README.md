# CrossPoint Reader — Translations

## Structure

| File | Purpose |
|---|---|
| `EN.yaml`, `DE.yaml`, … | Per-language string files |
| `manifest.json` | List of non-core languages available for on-device download |

---

## Core vs Non-Core Languages

**Core languages** (EN, DE, FR, ES, PT) are compiled directly into firmware flash.
They are always available on the device without an SD card.

**Non-core languages** (all other YAML files) are *not* compiled in.
They are loaded at runtime from the SD card path `/.crosspoint/languages/XX.yaml`.
The device can download them over WiFi using the manifest.

---

## Adding a New Language

### Non-core language (most translations)

1. Copy an existing YAML file and rename it `XX.yaml` (ISO 639-1 code, uppercase).
2. Fill in all string values. The `_language_code` and `_language_name` metadata fields
   at the top of the file are required — the device reads them at runtime to identify
   the language without loading the full file.
3. Add an entry to `manifest.json`:
   ```json
   { "code": "XX", "name": "Native Language Name" }
   ```
4. Commit both files. The `manifest.json` is served directly from GitHub and downloaded
   by devices on demand — there is no build step needed for non-core languages.

### Core language (only for very common languages)

Promoting a language to core bakes it into firmware flash, making it available without
an SD card. Only do this if there is a strong reason.

1. Add the language YAML file as above.
2. Add the language code to `CORE_LANGUAGES` in `scripts/gen_i18n.py`.
3. Run `gen_i18n.py` (or trigger a PlatformIO build) to regenerate `I18nKeys.h`,
   `I18nStrings.h`, and `I18nStrings.cpp`.
4. Remove the language from `manifest.json` (core languages are always present,
   so listing them in the manifest is unnecessary).

---

## manifest.json

`manifest.json` is a **manually maintained** file. It lists all non-core languages
that users can download to their device over WiFi.

The build script (`scripts/gen_i18n.py`) validates `manifest.json` at compile time
and prints a warning if it is out of sync with the YAML files (missing or extra codes).
It does **not** auto-update the file — you must edit it by hand when adding or removing
non-core languages.

The file is served via GitHub's raw content URL:

```text
https://raw.githubusercontent.com/crosspoint-reader/crosspoint-reader/master/lib/I18n/translations/manifest.json
```

---

## YAML File Format

```yaml
_language_code: "XX"           # ISO 639-1 code, uppercase (required)
_language_name: "Native Name"  # Display name in the native script (required)

# --- Strings (must match the keys in EN.yaml) ---
STR_OK: "OK"
STR_CANCEL: "Cancel"
# …
```

- Missing keys fall back to English at runtime (for non-core languages).
- Escape sequences: `\\` → `\`, `\"` → `"`, `\n` → newline.
- All string values must be quoted.

---

## Code Generation

The pre-build script `scripts/gen_i18n.py` reads all YAML files and regenerates:

| Generated file | Contents |
|---|---|
| `lib/I18n/I18nKeys.h` | `Language` enum, `StrId` enum, `LanguageMeta` table, `I18N_MAX_DECOMPRESSED_SIZE` |
| `lib/I18n/I18nStrings.h` | `I18nLangData` struct, `LANG_DATA[]` extern declaration |
| `lib/I18n/I18nStrings.cpp` | Compressed string blobs + `LANG_DATA[]` table, `ALL_LANGUAGES[]` |

**Do not edit these generated files by hand** — changes will be overwritten on the next build.

The script runs automatically as a PlatformIO pre-build step. You can also run it manually:

```bash
python scripts/gen_i18n.py lib/I18n/translations lib/I18n/
```
