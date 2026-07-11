# OPDS Download Folder Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let the user set one global destination folder for OPDS book downloads, edited from the OPDS server list screen, defaulting to SD root (current behaviour).

**Architecture:** A fixed `char[64]` field on the `CrossPointSettings` singleton stores the folder. A category-less `SettingInfo::String` entry in `getSettingsList()` makes it persist through `settings.json` while staying hidden from the on-device Settings screen. A virtual list item in `OpdsServerListActivity` (settings mode only) edits it via `KeyboardEntryActivity`. `OpdsBookBrowserActivity::downloadBook` prefixes the folder (creating it if needed) before writing the `.epub`.

**Tech Stack:** C++20 (no exceptions/RTTI), ESP32-C3 / PlatformIO, ArduinoJson, custom I18n YAML→codegen, HAL (`Storage`), Activity lifecycle.

**Spec:** `docs/superpowers/specs/2026-07-10-opds-download-folder-design.md`

## Testing note

There is **no unit-test harness** for these UI/device paths. The per-task
verifiable gate is a clean **compile** (`pio run`) plus **clang-format**. Runtime
behaviour (download lands in folder, persistence round-trip) is verified on
hardware by the human tester — see Task 6.

## File Structure

- `lib/I18n/translations/english.yaml` — new UI string `STR_OPDS_DOWNLOAD_FOLDER` (source; generated `I18nKeys.h`/`I18nStrings.*` are gitignored).
- `src/CrossPointSettings.h` — new `opdsDownloadFolder[64]` field on the settings struct.
- `src/SettingsList.h` — `SettingInfo::String` registration (no category → persisted, device-UI-hidden).
- `src/activities/browser/OpdsBookBrowserActivity.cpp` — consume the folder in `downloadBook`.
- `src/activities/settings/OpdsServerListActivity.cpp` — virtual "Download folder" item + keyboard editor (no header change).

---

### Task 1: Add the i18n string

**Files:**
- Modify: `lib/I18n/translations/english.yaml:340` (after `STR_OPDS_SERVERS`)

- [ ] **Step 1: Add the key**

Insert after the `STR_OPDS_SERVERS` line:

```yaml
STR_OPDS_DOWNLOAD_FOLDER: "Download folder"
```

- [ ] **Step 2: Regenerate the i18n tables**

Run: `python scripts/gen_i18n.py lib/I18n/translations lib/I18n/`
Expected: exits 0; `lib/I18n/I18nKeys.h` now contains `STR_OPDS_DOWNLOAD_FOLDER`.

Verify: `grep STR_OPDS_DOWNLOAD_FOLDER lib/I18n/I18nKeys.h`
Expected: one match (the enumerator).

- [ ] **Step 3: Commit (source YAML only — generated files are gitignored)**

```bash
git add lib/I18n/translations/english.yaml
git commit -m "feat(i18n): add STR_OPDS_DOWNLOAD_FOLDER"
```

---

### Task 2: Add the settings field and persistence entry

**Files:**
- Modify: `src/CrossPointSettings.h:237` (after `opdsPassword`)
- Modify: `src/SettingsList.h:196` (after the `moveFinishedToReadFolder` entry)

- [ ] **Step 1: Add the struct field**

In `src/CrossPointSettings.h`, immediately after `char opdsPassword[64] = "";`:

```cpp
  // OPDS download destination folder ("" = SD root). Global; edited from the
  // OPDS server list. Persisted via a category-less SettingInfo::String in
  // SettingsList.h, so it stays out of the on-device Settings screen.
  char opdsDownloadFolder[64] = "";
```

(Placement note: must NOT be the first member of the struct — `JsonSettingsIO`
skips `stringOffset == 0`. Here it sits mid-struct, which is correct.)

- [ ] **Step 2: Register the persistence entry**

In `src/SettingsList.h`, immediately after the `SettingInfo::Toggle(... "moveFinishedToReadFolder" ...)` entry, add:

```cpp
        // OPDS download folder: persisted + web-exposed, but category-less so it
        // is hidden from the on-device Settings screen (edited via OPDS UI).
        SettingInfo::String(StrId::STR_OPDS_DOWNLOAD_FOLDER, &SETTINGS.opdsDownloadFolder[0],
                            sizeof(SETTINGS.opdsDownloadFolder), "opdsDownloadFolder"),
```

- [ ] **Step 3: Compile**

Run: `pio run`
Expected: build succeeds, 0 errors/warnings.

- [ ] **Step 4: Format**

Run: `find src -name "*.cpp" -o -name "*.h" | xargs clang-format -i`
Expected: no diff in the two edited files (already formatted), or auto-fixed.

- [ ] **Step 5: Commit**

```bash
git add src/CrossPointSettings.h src/SettingsList.h
git commit -m "feat: add opdsDownloadFolder setting (persisted, device-UI-hidden)"
```

---

### Task 3: Consume the folder in downloadBook

**Files:**
- Modify: `src/activities/browser/OpdsBookBrowserActivity.cpp` (includes + `downloadBook`, ~line 272)

- [ ] **Step 1: Add the two includes**

In `src/activities/browser/OpdsBookBrowserActivity.cpp`, add to the include block (after `#include "MappedInputManager.h"`):

```cpp
#include "CrossPointSettings.h"
```

and, with the angle-bracket includes near the top:

```cpp
#include <HalStorage.h>
```

(`Storage` may already be visible transitively via `HttpDownloader.h`; the
explicit include documents the dependency and is harmless if redundant.)

- [ ] **Step 2: Replace the filename construction**

Replace these two lines (currently ~272-273):

```cpp
  std::string filename =
      "/" + StringUtils::sanitizeFilename((book.author.empty() ? "" : book.author + " - ") + book.title) + ".epub";
```

with:

```cpp
  // opdsDownloadFolder is already a null-terminated char[64]; use it directly —
  // no std::string copy. exists()/mkdir() take const char*.
  const char* folder = SETTINGS.opdsDownloadFolder;  // "" => SD root
  bool haveFolder = folder[0] != '\0';
  if (haveFolder && !Storage.exists(folder) && !Storage.mkdir(folder)) {
    // exists()-guard first: mkdir's return-on-existing is unconfirmed, and every
    // existing caller checks exists() before mkdir. On real failure, fall back
    // to SD root so the download is never lost.
    LOG_ERR("OPDS", "mkdir failed for %s, using SD root", folder);
    haveFolder = false;
  }

  // downloadToFile() needs a std::string, and titles are unbounded (a fixed
  // char[] would truncate). Cold path (a multi-second download follows), so one
  // reserve'd, in-place-appended owning string is the right call.
  std::string filename;
  filename.reserve(96);
  if (haveFolder) filename += folder;
  filename += '/';
  filename += StringUtils::sanitizeFilename((book.author.empty() ? "" : book.author + " - ") + book.title);
  filename += ".epub";
```

- [ ] **Step 3: Compile**

Run: `pio run`
Expected: build succeeds, 0 errors/warnings.

- [ ] **Step 4: Format**

Run: `find src -name "*.cpp" -o -name "*.h" | xargs clang-format -i`
Expected: no diff / auto-fixed only in the edited file.

- [ ] **Step 5: Commit**

```bash
git add src/activities/browser/OpdsBookBrowserActivity.cpp
git commit -m "feat: download OPDS books into configured folder"
```

---

### Task 4: Add the "Download folder" list item + editor

**Files:**
- Modify: `src/activities/settings/OpdsServerListActivity.cpp` (includes, `getItemCount`, `handleSelection`, `render`)

No header change: the folder editor is inlined in `handleSelection`.

- [ ] **Step 1: Add includes**

After `#include "OpdsServerStore.h"`:

```cpp
#include "CrossPointSettings.h"
```

and with the standard headers at the top of the file:

```cpp
#include <cstring>
```

- [ ] **Step 2: Add a file-local folder normalizer**

Inside the existing top of the file, add an anonymous namespace above `getItemCount` (create one if absent):

```cpp
namespace {
// Normalizes a user-typed folder: trims spaces, "" => SD root, otherwise a
// single leading '/' and no trailing '/'. Cold path (runs once per edit).
std::string normalizeFolder(std::string v) {
  while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(v.begin());
  while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.pop_back();
  if (v.empty()) return "";
  if (v.front() != '/') v.insert(v.begin(), '/');
  while (v.size() > 1 && v.back() == '/') v.pop_back();
  return v;
}
}  // namespace
```

- [ ] **Step 3: Grow the item count (settings mode adds 2 items)**

Replace `getItemCount`:

```cpp
int OpdsServerListActivity::getItemCount() const {
  int count = static_cast<int>(OPDS_STORE.getCount());
  // Settings mode appends two virtual items: "Add Server" and "Download folder".
  if (!pickerMode) {
    count += 2;
  }
  return count;
}
```

- [ ] **Step 4: Handle selection of the folder item**

In `handleSelection`, after the `if (pickerMode) { ... return; }` block and before the `resultHandler`/editor branch, insert:

```cpp
  // Settings mode. Index layout: [servers 0..serverCount-1], [Add Server], [Download folder].
  if (selectedIndex == serverCount + 1) {
    auto folderHandler = [this](const ActivityResult& result) {
      if (!result.isCancelled) {
        const auto& kb = std::get<KeyboardResult>(result.data);
        const std::string norm = normalizeFolder(kb.text);
        strncpy(SETTINGS.opdsDownloadFolder, norm.c_str(), sizeof(SETTINGS.opdsDownloadFolder) - 1);
        SETTINGS.opdsDownloadFolder[sizeof(SETTINGS.opdsDownloadFolder) - 1] = '\0';
        SETTINGS.saveToFile();
        requestUpdate();
      }
    };
    startActivityForResult(
        std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_OPDS_DOWNLOAD_FOLDER),
                                                std::string(SETTINGS.opdsDownloadFolder), 63, InputType::Text),
        folderHandler);
    return;
  }
```

Add the include for the keyboard activity near the other project includes:

```cpp
#include "activities/util/KeyboardEntryActivity.h"
```

Note: the existing editor branch uses `if (selectedIndex < serverCount) editExisting; else newServer;`. With the folder handled above, `selectedIndex == serverCount` ("Add Server") still correctly falls to the `else` (new server) branch. No change needed there.

- [ ] **Step 5: Render the two virtual items**

Replace the `GUI.drawList(...)` call's two label lambdas (primary + secondary) so `serverCount` and `serverCount + 1` are labelled. The full replacement for the `else` branch body in `render`:

```cpp
    const auto& servers = OPDS_STORE.getServers();
    const auto serverCount = static_cast<int>(servers.size());

    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, itemCount, selectedIndex,
        [&servers, serverCount](int index) -> std::string {
          if (index < serverCount) {
            const auto& server = servers[index];
            return server.name.empty() ? server.url : server.name;
          }
          if (index == serverCount) {
            return std::string(I18n::getInstance().get(StrId::STR_ADD_SERVER));
          }
          return std::string(I18n::getInstance().get(StrId::STR_OPDS_DOWNLOAD_FOLDER));
        },
        [&servers, serverCount](int index) -> std::string {
          if (index < serverCount && !servers[index].name.empty()) {
            return servers[index].url;
          }
          if (index == serverCount + 1) {
            const char* f = SETTINGS.opdsDownloadFolder;
            return f[0] ? std::string(f) : std::string(I18n::getInstance().get(StrId::STR_SD_CARD));
          }
          return std::string("");
        });
```

- [ ] **Step 6: Compile**

Run: `pio run`
Expected: build succeeds, 0 errors/warnings.

- [ ] **Step 7: Format**

Run: `find src -name "*.cpp" -o -name "*.h" | xargs clang-format -i`
Expected: no diff / auto-fixed only in the edited file.

- [ ] **Step 8: Commit**

```bash
git add src/activities/settings/OpdsServerListActivity.cpp
git commit -m "feat: edit OPDS download folder from server list"
```

---

### Task 5: Static analysis + full clean build

**Files:** none (verification only)

- [ ] **Step 1: Clean build**

Run: `pio run -t clean && pio run`
Expected: 0 errors, 0 warnings.

- [ ] **Step 2: Static analysis**

Run: `pio check`
Expected: no new defects introduced by the changed files.

- [ ] **Step 3: Confirm no gitignored/generated files are staged**

Run: `git status --short`
Expected: clean tree (all work committed); no `*.generated.h`, `I18nKeys.h`, `I18nStrings.*`, `.pio/` staged.

---

### Task 6: Device verification (human tester)

**Not AI-verifiable — flag to the user.** After flashing:

- [ ] Set a folder (e.g. `/Books`) via **OPDS servers → Download folder**; confirm it displays under the item as the subtitle.
- [ ] Reboot; reopen the screen; confirm the folder value persisted (JSON round-trip through `settings.json`).
- [ ] Confirm it does **not** appear in the on-device Settings screen (category filter).
- [ ] Download a book; confirm the `.epub` lands in `/Books/` and shows in the file browser.
- [ ] Set a nested path (e.g. `/Books/SciFi`); confirm the folder tree is created and the book lands there.
- [ ] Clear the folder (empty); confirm downloads go to SD root as before.
- [ ] Verify the list item renders correctly in all 4 orientations.
- [ ] `ESP.getFreeHeap()` stable across repeated downloads (no leak).
