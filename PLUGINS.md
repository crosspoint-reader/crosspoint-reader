# CrossPoint Reader — Plugin Developer Guide

CrossPoint Reader supports a **compile-time plugin system** that lets fork
authors ship features as self-contained modules without forking the entire
firmware. Plugins are statically linked into the binary — there is no dynamic
loading on ESP32-C3.

---

## Quick start

1. **Copy the example plugin** `src/plugins/HelloPlugin.cpp` to a new file,
   e.g. `src/plugins/MyFeature.cpp`.
2. **Fill in the `CprPlugin` struct** with your metadata and hook
   implementations.
3. **Register your descriptor** by adding an `extern const CprPlugin` forward
   declaration and a table entry in `src/plugin/PluginRegistry.cpp`.
4. **Build and flash.** Your plugin will appear in *Settings → Plugins*.

---

## Plugin descriptor

Every plugin defines a single `CprPlugin` struct (see
`src/plugin/CprPlugin.h`):

```cpp
#include "plugin/CprPlugin.h"

extern const CprPlugin myFeature = {
    .id          = "my_feature",      // unique snake_case identifier
    .name        = "My Feature",      // shown in Settings UI
    .version     = "0.1.0",
    .author      = "Your Name",
    .minCpr      = "1.2.0",           // minimum CrossPoint version
    .description = "A short description of what this plugin does",

    .onBoot           = nullptr,      // called once at startup
    .onSettingsRender = nullptr,      // inject custom rows in detail screen
    .onBookOpen       = nullptr,      // receives epub path
    .onBookClose      = nullptr,
    .onPageTurn       = nullptr,      // receives (chapter, page)
    .onSleep          = nullptr,
    .onWake           = nullptr,
};
```

Set any unused hooks to `nullptr`.

---

## Registering a plugin

Open `src/plugin/PluginRegistry.cpp` and:

1. Add an `extern const CprPlugin myFeature;` forward declaration alongside the
   existing ones.
2. Add `&myFeature` to the `pluginTable[]` array.

```cpp
extern const CprPlugin myFeature;

static const CprPlugin* const pluginTable[] = {
    &helloPlugin,
    &myFeature,
};
```

That's it — no other files need to change.

---

## Hook reference

| Hook              | Signature                              | When it fires                        |
|-------------------|----------------------------------------|--------------------------------------|
| `onBoot`          | `void ()`                              | Once after system init completes     |
| `onSettingsRender`| `void ()`                              | Plugin detail screen is rendered     |
| `onBookOpen`      | `void (const char* epubPath)`          | A book is opened in the reader       |
| `onBookClose`     | `void ()`                              | The reader activity exits            |
| `onPageTurn`      | `void (int chapter, int page)`         | Every page change in the reader      |
| `onSleep`         | `void ()`                              | Device is entering deep sleep        |
| `onWake`          | `void ()`                              | Device has woken from deep sleep     |

**Important:** hooks run on the main loop thread. They must return quickly
(< 10 ms) to avoid watchdog timeouts or UI stalls.

---

## Persisting plugin-specific state

Each plugin may store its own data under `/.crosspoint/plugin_<id>/` on the
SD card.  Use the `Storage` singleton (from `HalStorage.h`) for all file I/O:

```cpp
#include <HalStorage.h>
#include <ArduinoJson.h>

static void saveMyState() {
    Storage.mkdir("/.crosspoint/plugin_my_feature");

    JsonDocument doc;
    doc["counter"] = counter;

    String json;
    serializeJson(doc, json);
    Storage.writeFile("/.crosspoint/plugin_my_feature/state.json", json);
}
```

Follow the same JSON + `Storage.readFile()` / `Storage.writeFile()` pattern
used by the base firmware (see `src/JsonSettingsIO.cpp` for examples).

---

## Compatibility versioning

Each plugin declares `minCpr` — the oldest firmware version it supports.  At
boot, the registry compares this against `CROSSPOINT_VERSION` using semantic
versioning (major.minor.patch).

- If the firmware is **older** than `minCpr`, the plugin is automatically
  disabled and the Settings UI shows an "Incompatible" badge.
- Users cannot re-enable an incompatible plugin until they update their
  firmware.

**Tip:** set `minCpr` to the version you developed against.  Only bump it when
you start using new firmware APIs.

---

## Excluding a plugin at compile time

If flash space is tight, you can exclude a plugin entirely by wrapping its
source file with a build flag:

```cpp
// src/plugins/MyFeature.cpp
#ifndef EXCLUDE_MY_FEATURE_PLUGIN

#include "plugin/CprPlugin.h"
// ... plugin code ...

#endif  // EXCLUDE_MY_FEATURE_PLUGIN
```

Then add `-DEXCLUDE_MY_FEATURE_PLUGIN` to `build_flags` in
`platformio.ini` or `platformio.local.ini`.  Remember to also guard the
corresponding entry in `pluginTable[]` in `PluginRegistry.cpp`.

---

## Coding conventions

All plugin code **must** follow the conventions in `.skills/SKILL.md`:

- Use `LOG_DBG` / `LOG_INF` / `LOG_ERR` for output — never raw `Serial`.
- Keep stack locals under 256 bytes.
- Avoid `std::string` in hot paths; prefer `const char*` or `snprintf`.
- Check every `malloc` return for `nullptr` and free promptly.
- Mark constant data `static constexpr` to keep it in Flash.
- Use `tr()` macro for any user-facing text (i18n support).
- Follow the project naming conventions: PascalCase for classes, camelCase for
  methods/variables, UPPER_SNAKE_CASE for constants.

See `src/plugins/HelloPlugin.cpp` for a fully annotated reference
implementation.
