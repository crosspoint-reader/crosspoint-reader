# Plan: Load WiFi credentials at boot so Reminders sync connects without visiting WiFi settings

> Self-contained handoff doc. Start a new branch off the latest `taskpoint/main`
> (e.g. `claude/reminders-wifi-boot-load`) and implement the changes below.

## Problem

Waking the device and double-tapping power to launch **Reminders** sync fails to
connect to WiFi **unless the user first opens the WiFi settings screen**. After
visiting settings once, sync works.

### Why visiting settings "fixes" it (the misleading part)
`WifiSelectionActivity::onExit` deliberately does **not** disconnect WiFi
(`src/activities/network/WifiSelectionActivity.cpp`, see the "We do NOT disconnect
WiFi here" comment). So after you connect in settings, WiFi stays up, and the
later sync short-circuits here:

```cpp
// src/network/GoogleClient.cpp  (connectWifi)
if (WiFi.status() == WL_CONNECTED) return true;   // already connected → succeeds
```

That masks the real bug: on a cold wake the sync's own scan/associate never has
credentials to work with.

### Root cause
`WIFI_STORE` (the saved-WiFi credential store) is the **only** persistent store
not loaded at boot. `src/main.cpp` `setup()` loads every other store:

```cpp
// src/main.cpp ~lines 418-423
SETTINGS.loadFromFile();
APP_STATE.loadFromFile();
RECENT_BOOKS.loadFromFile();
I18N.setLanguage(...);
KOREADER_STORE.loadFromFile();
OPDS_STORE.loadFromFile();
// <-- WIFI_STORE.loadFromFile() is MISSING
```

The credential store is populated **only** by `WifiSelectionActivity::onEnter`
(`WifiSelectionActivity.cpp:25`, `WIFI_STORE.loadFromFile()` under a `RenderLock`
on the main task). A deep-sleep wake is a full chip reset, so on the
wake→Reminders path the in-memory credential vector is empty, and
`connectWifi()` (`GoogleClient.cpp`) bails with `WifiFailed`.

A prior fix added a lazy `WIFI_STORE.loadFromFile()` inside `connectWifi()`, but
that runs **bare on the background `RmndSync` FreeRTOS task**. Loading at boot on
the main task — exactly like every other store and like the known-good settings
path — is the reliable fix.

## Changes (small, single-concern)

### 1. Load WiFi credentials at boot — the actual fix
File: `src/main.cpp`
- Add the include near the other store includes (around lines 24-27, next to
  `OpdsServerStore.h` / `RecentBooksStore.h`):
  ```cpp
  #include "WifiCredentialStore.h"
  ```
- Add the load right after `OPDS_STORE.loadFromFile();` (line 423):
  ```cpp
  WIFI_STORE.loadFromFile();
  ```
This runs on the main task during boot, so credentials are in memory before any
Reminders sync, regardless of whether the user ever opens WiFi settings.

> The existing lazy load inside `connectWifi()` can stay as a harmless fallback
> (covers a store cleared at runtime). The boot load is the primary path.

### 2. Split the failure diagnostic — so a continued failure is unambiguous
Today both "no saved networks" and "couldn't associate" collapse into one
`WifiFailed` → "WiFi could not connect" screen. Separate them.

- `src/network/GoogleClient.h`: add `NoWifiCreds` to the `Result` enum, right
  after `WifiFailed`:
  ```cpp
  enum class Result {
    OK,
    NoCredentials,   // google_creds.json missing/malformed
    WifiFailed,      // saved network(s) present but could not associate
    NoWifiCreds,     // no saved WiFi networks on SD at all
    ClockUnset,
    AuthFailed,
    FetchFailed,
    Cancelled,
  };
  ```
- `src/network/GoogleClient.cpp`:
  - `resultName()` (~line 802): add `case Result::NoWifiCreds: return "NoWifiCreds";`.
  - In `syncAll` (the `if (!connectWifi())` at ~line 704) **and** `markTaskComplete`
    (~line 775), choose the result by whether any credentials exist:
    ```cpp
    if (!connectWifi()) {
      if (cancelled()) return Result::Cancelled;
      return WIFI_STORE.getCredentials().empty() ? Result::NoWifiCreds : Result::WifiFailed;
    }
    ```
    (`connectWifi` already attempts a load, so "empty here" means
    `/.crosspoint/wifi.json` is genuinely missing/empty.)
- `src/activities/reminders/RemindersActivity.cpp`: in the `render()`
  `State::Failed` switch (~lines 330-346), add:
  ```cpp
  case GoogleClient::Result::NoWifiCreds:
    hint = StrId::STR_REMINDERS_FAIL_NOWIFI;
    break;
  ```
- `lib/I18n/translations/english.yaml`: add near the other `STR_REMINDERS_FAIL_*`
  keys:
  ```yaml
  STR_REMINDERS_FAIL_NOWIFI: "No saved WiFi — open WiFi settings"
  ```
  Commit **only** the YAML — generated i18n headers (`I18nKeys.h`,
  `I18nStrings.{h,cpp}`) are gitignored and regenerated at build (or via
  `python scripts/gen_i18n.py lib/I18n/translations lib/I18n/`).

No cache-format/struct changes, no new heap allocation. Reuses existing
`WIFI_STORE.loadFromFile()`, `WIFI_STORE.getCredentials()`, and `resultName()`.

## Verification

1. **Build from the new branch via CI.** The ESP32 toolchain can't be downloaded
   in the sandbox (SSL cert failure on the arduino-esp32 package), so rely on the
   PR's GitHub Actions build. Run `clang-format` on edited `.cpp`/`.h`; regenerate
   i18n locally to sanity-check the YAML.
2. **Cold path test on device** — fully power off / deep sleep, do **not** open
   WiFi settings, then wake and double-tap power:
   - Expected: sync connects without visiting WiFi settings (advances past WiFi to
     the next stage / completes).
   - Still **"WiFi could not connect"** → creds load but radio can't associate
     (weak signal, wrong password, 5 GHz-only AP); chase `tryConnectSsid`/scan.
   - New **"No saved WiFi — open WiFi settings"** → `wifi.json` is missing/empty;
     re-save the network once in settings.
3. Confirm the normal already-connected (post-settings) path still works.

## Commit guidance
Two small commits:
- `fix: load saved WiFi credentials at boot so wake-into-Reminders can connect`
- `feat: distinguish "no saved WiFi" from "WiFi could not connect" on sync-fail screen`

## Notes / context for the next session
- Mainline `taskpoint/main` already contains earlier related commits:
  `cb6a837` (scan settle+retry), `d5ee902` (per-stage fail screen),
  `950a91e` (lazy credential load inside `connectWifi`). This plan layers on top.
- The PR-number tangle (#24 based on `master`) is a red herring — the substantive
  issue is the missing boot-time `WIFI_STORE.loadFromFile()` above.
- Relevant files: `src/main.cpp`, `src/network/GoogleClient.{h,cpp}`,
  `src/activities/reminders/RemindersActivity.cpp`,
  `src/activities/network/WifiSelectionActivity.cpp` (reference only),
  `src/WifiCredentialStore.{h,cpp}`, `lib/I18n/translations/english.yaml`.
