# CrossPoint Architecture

## Feature Flag Boundary Rules

### What `ENABLE_*` flags are and where they live

`ENABLE_*` macros are **compile-time feature toggles**. They are defined in
`include/FeatureFlags.h`, which is the single source of truth. Build profiles
set overrides via `-DENABLE_FOO=0` in `platformio.ini`; `FeatureFlags.h` applies
inter-feature dependency constraints (e.g. `ENABLE_KOREADER_SYNC` is forced to 0
when `ENABLE_INTEGRATIONS=0`).

### Approved locations for `ENABLE_*` usage

| Location | Allowed | Reason |
|---|---|---|
| `include/FeatureFlags.h` | ✅ | Source of truth and dependency enforcement |
| `src/core/*` | ✅ | Bootstrap, registry dispatch, FeatureCatalog validation |
| `src/features/<feature>/Registration.cpp` | ✅ | Each feature wraps its own registration unit in `#if ENABLE_<FEATURE>` |
| `platformio.ini` / `platformio.local.ini` | ✅ | Build profile overrides |

### Prohibited locations

Direct `#if ENABLE_*` checks must **not** appear in:

- `src/main.cpp` — call bootstrap and lifecycle stages only; no concrete feature names
- `src/activities/ActivityManager.cpp` — query registries; no per-format or per-feature branches
- `src/network/CrossPointWebServer.cpp` — mount routes from `WebRouteRegistry`; no per-route `#if` blocks
- `src/activities/**` (activity implementations) — activities must not gate behaviour on feature flags
- `src/network/**` (web handlers, other than the web server mount point)
- `src/CrossPointSettings.cpp` — settings persistence must not branch per feature
- `src/components/UITheme.cpp` — theme rendering must not branch per feature
- `lib/**` — internal libraries are feature-agnostic

Any `ENABLE_*` reference found outside the approved set is a boundary violation.
`scripts/check_feature_boundaries.sh` enforces this. The script carries two distinct
categories of exclusion — **permanent compile-time exceptions** and **temporary cleanup
debt** — which must not be conflated.

#### Permanent compile-time exceptions

These files contain `ENABLE_*` guards that cannot be moved to a `Registration.cpp`
because the guarded symbols (EpdFont objects, decoder headers, peripheral driver
classes) are absent from the binary when the feature is disabled. No runtime registry
check can substitute for a compile-time-absent symbol.

| File | Representative flag(s) | Why it cannot move to Registration.cpp |
|---|---|---|
| `src/CrossPointSettings.cpp` | `ENABLE_*_FONTS`, `ENABLE_LYRA_THEME` | Selects among EpdFont *objects* and theme *instances*; the objects are not linked when disabled — a runtime fallback would reference a non-existent symbol |
| `src/activities/home/SleepActivity.cpp` | `ENABLE_IMAGE_SLEEP` | PNG/JPEG decoder symbols absent when disabled |
| `src/activities/reader/OpdsBookBrowserActivity.cpp` | `ENABLE_EPUB_SUPPORT` | `<Epub.h>` header absent when disabled |
| `src/network/CrossPointWebServer.cpp` | `ENABLE_IMAGE_SLEEP` | Same decoder-absence constraint as SleepActivity |
| `src/network/OtaWebCheck.cpp` | `ENABLE_OTA_UPDATES` | OtaUpdater symbols absent when disabled |
| `src/util/UsbSerialProtocol.*` | `ENABLE_USB_SERIAL` | Entire peripheral driver; compile-time-only option |
| `src/util/UserFontManager.*` | `ENABLE_USER_FONTS` | Entire peripheral driver; compile-time-only option |
| `src/util/BleWifiProvisioner.*` | `ENABLE_BLE_WIFI_PROV` | Entire peripheral driver; compile-time-only option |

#### Temporary cleanup debt

These files contain `ENABLE_*` guards that **can** be migrated to
`src/features/<feature>/Registration.cpp` but have not been yet. Each exclusion in
`check_feature_boundaries.sh` must be removed once the corresponding migration PR
lands. The script emits a non-blocking warning while they remain.

| File | Flag(s) still present | Migration target |
|---|---|---|
| `src/components/UITheme.cpp` | `ENABLE_LYRA_THEME` | Extract theme class selection to a feature registration unit |
| `src/network/BackgroundWebServer.cpp` | `ENABLE_BACKGROUND_SERVER` | Move whole-subsystem guard to Registration.cpp |
| `src/activities/reader/TxtReaderActivity.cpp` | `ENABLE_MARKDOWN` | Split markdown helper functions into a separate compilation unit guarded in Registration.cpp |

### Why this boundary exists

Before the modularity refactor, `FeatureModules.cpp` accumulated every `#if ENABLE_*`
branch for every optional feature — a ~1000-line God Class that required modification
on every feature add or remove. The registry pattern moves feature knowledge into
`src/features/<feature>/Registration.cpp`, leaving app shells as generic coordinators.

The boundary rule preserves **compile-time dead-code elimination**: a disabled feature
contributes zero binary size only if its `#if ENABLE_*` guard lives exclusively in its
registration unit and nowhere in the app shell.

---

## Registry Architecture

### Registry types

| Registry | Header | Purpose |
|---|---|---|
| `ReaderRegistry` | `src/core/registries/ReaderRegistry.h` | Maps file extensions to reader activity factories (EPUB, XTC, Markdown, TXT) |
| `SettingsActionRegistry` | `src/core/registries/SettingsActionRegistry.h` | Maps `SettingAction` enum values to settings sub-activity factories |
| `WebRouteRegistry` | `src/core/registries/WebRouteRegistry.h` | Mounts optional HTTP routes (Anki, OTA, and simple plugin routes) |
| `HomeActionRegistry` | `src/core/registries/HomeActionRegistry.h` | Exposes optional home screen actions (Anki, OPDS, Todo) |
| `LifecycleRegistry` | `src/core/registries/LifecycleRegistry.h` | Dispatches lifecycle hooks (onStorageReady, onFontChanged, etc.) |
| `SyncServiceRegistry` | `src/core/registries/SyncServiceRegistry.h` | Typed service access for features with data/settings APIs |

### Registry constraints (ESP32-C3 memory rules)

- **No `std::function`** — adds ~2–4 KB per unique signature and heap-allocates closures.
  Use raw function pointers and POD entry structs instead.
- **No per-entry heap allocation** — use fixed-capacity static arrays.
- **No dynamic growth** — do not use `std::vector` for registry storage.
- **No static constructor self-registration** — all registration must be triggered
  explicitly from `CoreBootstrap::init()` to avoid static-initialization-order issues
  on ESP32-C3 / newlib.

Illustrative entry shape:

```cpp
struct ReaderEntry {
    const char* extension;                    // e.g. ".epub"
    bool (*isSupported)(const char* path);    // runtime capability check
    Activity* (*create)(GfxRenderer&, MappedInputManager&, const std::string& path);
};
```

### Registration model

Each feature provides a registration unit:

```
src/features/<feature>/Registration.h
src/features/<feature>/Registration.cpp
```

The `.cpp` file is wrapped in `#if ENABLE_<FEATURE>` so that a disabled feature
compiles to nothing and links to nothing. The registration unit calls
`SomeRegistry::add(entry)` for each capability it contributes.

The `CoreBootstrap` initialization path calls each feature's `registerFeature()`
in dependency order before `FeatureCatalog` validation runs.

---

## App Shell Contracts

### `src/main.cpp`
- Calls `CoreBootstrap` feature-system initialization (triggers feature registration)
- Calls lifecycle stages via `LifecycleRegistry`
- Still contains some capability queries through `FeatureModules`; the target state is to keep those deliberate and minimal

### `src/activities/ActivityManager.cpp`
- Uses `HomeActionRegistry` for optional home actions such as Anki, OPDS, and Todo
- Still opens readers through the `FeatureModules` compatibility wrapper rather than directly through `ReaderRegistry`
- Must not add new concrete optional-feature construction paths

### `src/network/CrossPointWebServer.cpp`
- Calls `WebRouteRegistry::mountAll(server)` to register routes
- Mounts simple plugin routes, Anki routes, and OTA routes via the registry
- Still mounts some complex feature routes manually (`pokemon_party_api`, `user_fonts_api`, `web_wifi_setup_api`)
- Must not reintroduce per-feature `#if` blocks in the app shell

---

## Legacy Component Status

### FeatureManifest.h (DELETED)

`src/FeatureManifest.h` has been removed. All feature capability checks now use
the registry pattern or direct `ENABLE_*` guards in approved registration units.

### FeatureModules.cpp (Compatibility Facade)

`FeatureModules.cpp` remains as a temporary compatibility facade for legacy
components not yet fully migrated to the registry pattern. Do not add new
logic here.
