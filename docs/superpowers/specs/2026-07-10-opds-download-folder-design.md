# OPDS Download Folder — Design

Date: 2026-07-10
Status: Approved (design), pending implementation plan
Branch: `feature/opds-download-folder`

## Context

The CrossPoint firmware already has a complete OPDS book browser
(`src/activities/browser/OpdsBookBrowserActivity`) that downloads EPUBs from an
OPDS server, plus OPDS server management UI (`OpdsServerListActivity`,
`OpdsSettingsActivity`) and a persisted server store (`OpdsServerStore`). This
is the mechanism used to browse and download books from a BookOrbit server,
whose NestJS backend exposes a standard OPDS feed at `/api/v1/opds` (HTTP Basic
auth, `application/epub+zip` acquisition links) that the firmware parser already
consumes.

Today every downloaded book is written to the **root of the SD card**:

```cpp
// src/activities/browser/OpdsBookBrowserActivity.cpp:272-273
std::string filename =
    "/" + StringUtils::sanitizeFilename((book.author.empty() ? "" : book.author + " - ") + book.title) + ".epub";
```

There is no way to direct downloads into a subfolder, so the SD root fills up
with loose files mixed into the whole file-browser tree.

## Goal

Let the user configure a single, global destination folder for OPDS downloads,
edited from the OPDS settings area, defaulting to current behaviour (SD root).

## Non-Goals

- Per-server download folders (the setting is global to all OPDS downloads).
- A generic folder-picker UI (the file browser has no directory-pick mode; out
  of scope).
- Changing the destination of any other download path (firmware/OTA, fonts).
- BookOrbit-native catalog client, progress/highlight sync, or any non-OPDS
  integration.

## Design

### 1. Storage — `CrossPointSettings`

Add one fixed-size field next to the existing `opds*` members in
`src/CrossPointSettings.h` (~line 237):

```cpp
char opdsDownloadFolder[64] = "";  // empty = SD root (current behaviour)
```

Cost: +64 bytes in the settings struct (DRAM), no runtime allocation.

### 2. Persistence — `SettingsList.h`

Register a `SettingInfo::String` entry **with no category** so it is persisted
but hidden from the on-device Settings UI:

```cpp
SettingInfo::String(StrId::STR_OPDS_DOWNLOAD_FOLDER, &SETTINGS.opdsDownloadFolder[0],
                    sizeof(SETTINGS.opdsDownloadFolder), "opdsDownloadFolder")
```

Why this works:
- `JsonSettingsIO` (`src/JsonSettingsIO.cpp:123`, `:182`) iterates
  `getSettingsList()` and saves/loads every entry that has a `key` and a
  `stringOffset`, regardless of category → the field round-trips through
  `/.crosspoint/settings.json` automatically, and is exposed on the web settings
  API for free.
- `SettingsActivity` (`src/activities/settings/SettingsActivity.cpp:43`) skips
  any entry whose `category == StrId::STR_NONE_OPT` → the field never appears in
  the generic device Settings screen.

Notes:
- This is the **first** `getSettingsList()` entry to use the `stringOffset`
  variant of `SettingInfo::String` (existing `DynamicString` entries use
  `stringGetter/Setter` with `stringOffset == 0`, which the JsonSettingsIO
  generic loop skips). The field is already read/written via the web API
  (`CrossPointWebServer.cpp:1170` GET, `:1256` SET), but the **`settings.json`
  file round-trip through JsonSettingsIO is untested** — implementation MUST
  verify it end to end (write a value, reload, confirm it persists).
- `JsonSettingsIO` skips any entry with `stringOffset == 0`
  (`JsonSettingsIO.cpp:126`, `:185`). `opdsDownloadFolder` therefore must **not**
  be the first member of `CrossPointSettings`; placing it mid-struct near the
  other `opds*` fields (~line 237) satisfies this.

### 3. Edit UX — `OpdsServerListActivity`

The download folder is edited from the OPDS server list screen (settings mode
only), keeping it in the OPDS area while remaining a single global value.

- Append one virtual item after the existing `[servers...] + "Add Server"`
  list, labelled `Download folder: <value or "SD root">`.
- `getItemCount()`: `+1` in settings mode only (not in `pickerMode`).
- `handleSelection()`: when the new item is selected, open
  `KeyboardEntryActivity` (`InputType::Text`, prefilled with the current value,
  maxLen 63).
- `KeyboardEntryActivity` has **no constructor callback**. It returns via
  `setResult(KeyboardResult{...})` + `finish()` and is consumed through
  `startActivityForResult(std::make_unique<KeyboardEntryActivity>(...), handler)`,
  where the handler reads `std::get<KeyboardResult>(result.data)` and checks
  `result.isCancelled` (established pattern at
  `src/activities/settings/OpdsSettingsActivity.cpp:116-145`). The confirm logic
  below MUST live inside that result handler.
- On keyboard confirm (inside the result handler, when not cancelled):
  1. Normalize the input: trim; empty → `""` (root); otherwise ensure a single
     leading `/` and strip any trailing `/`.
  2. Copy into `SETTINGS.opdsDownloadFolder` (bounded `strncpy`, NUL-terminate).
  3. `SETTINGS.saveToFile()`.
- Hidden entirely when `pickerMode == true` (the browse-from-home entry point).

### 4. Use — `OpdsBookBrowserActivity::downloadBook`

At `src/activities/browser/OpdsBookBrowserActivity.cpp:272`, prefix the folder
and ensure it exists:

```cpp
std::string folder = SETTINGS.opdsDownloadFolder;   // "" => SD root
if (!folder.empty()) {
  // Guard with exists() first: mkdir's return-on-existing is not confirmed, and
  // every existing caller (CrossPointWebServer.cpp:788, WebDAVHandler.cpp:472)
  // checks exists() before mkdir. Without the guard, the 2nd+ download into a
  // configured folder could log a false "mkdir failed" and redirect to root.
  if (!Storage.exists(folder.c_str()) && !Storage.mkdir(folder.c_str())) {
    LOG_ERR("OPDS", "mkdir failed for %s, falling back to SD root", folder.c_str());
    folder.clear();                                 // never lose the download
  }
}
std::string filename = folder + "/" +
    StringUtils::sanitizeFilename((book.author.empty() ? "" : book.author + " - ") + book.title) + ".epub";
```

`HalStorage::mkdir(path, pFlag=true)` is documented to create parent directories,
so nested folders (`/Books/SciFi`) should be supported. **Both of these are
unverified** because the `open-x4-sdk` submodule (where `SDCardManager::mkdir`
lives) is not checked out in this workspace. Implementation MUST confirm, before
relying on the above: (a) `mkdir` returns `true` (or is a no-op) for an existing
directory, and (b) `pFlag=true` truly creates parents. If mkdir-on-existing
returns `false`, the `exists()` guard above already covers it. A purpose-built
idempotent helper `HalStorage::ensureDirectoryExists()` exists (`HalStorage.h:31`,
currently zero callers) and may be used instead if it proves cleaner. On any
failure, fall back to SD root and still write the book (logged).

### 5. i18n

Add one key `STR_OPDS_DOWNLOAD_FOLDER` to `lib/I18n/translations/english.yaml`
(used for both the list label and the keyboard title), then regenerate:

```
python scripts/gen_i18n.py lib/I18n/translations lib/I18n/
```

Commit the YAML only; the three generated files are gitignored.

## Edge Cases & Decisions

- **Default = SD root** (`""`): zero behaviour change for existing users unless
  they set a folder. (Alternative `/Books` default was considered and rejected
  to preserve backward compatibility.)
- **Nested paths**: supported via `mkdir` recursive parent creation.
- **mkdir failure**: fall back to SD root, logged, download still succeeds.
- **Path sanitization**: normalize leading/trailing slashes on entry. Individual
  path segments are the user's responsibility; the existing
  `StringUtils::sanitizeFilename` still sanitizes the *filename*, not the folder.
- **RAM**: +64 bytes permanent DRAM in the settings singleton. Section 4 adds a
  transient `std::string folder` copy and one concatenation; negligible, and the
  current code already heap-allocates the filename string. No RISC-V alignment
  concern (plain `char[]`, byte access).
- **"Hidden" scope**: the setting is hidden from the **on-device** Settings
  screen only. `CrossPointWebServer::handleGetSettings()` does not filter by
  category, so it WILL appear in the web settings API/page under an empty
  ("None") category. This is acceptable (the value is legitimately a global
  setting) and is called out here so it is not a surprise.

## Files Touched

- `src/CrossPointSettings.h` — new field.
- `src/SettingsList.h` — `SettingInfo::String` registration (no category).
- `src/activities/settings/OpdsServerListActivity.{h,cpp}` — virtual item,
  count, selection handling, render label.
- `src/activities/browser/OpdsBookBrowserActivity.cpp` — folder prefix + mkdir.
- `lib/I18n/translations/english.yaml` — new string key (+ i18n regen).

## Verification

AI-verifiable:
- `pio run` clean (0 errors/warnings).
- `pio check` + clang-format.
- JSON round-trip of `opdsDownloadFolder` (inspect `settings.json` after a set).
- The setting does NOT appear in the device Settings screen (category filter).

Human tester (flag for user):
- On device: set a folder in OPDS settings, download a book, confirm it lands in
  that folder and appears in the file browser.
- Nested folder path is created.
- Empty folder → downloads to SD root as before.
- All 4 orientations render the new list item correctly.
