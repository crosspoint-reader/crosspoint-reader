# OTA Rollback Debugging Notes

**Branch**: `feature/rss-feed-sync`  
**Date**: 2026-02-28  
**Status**: UNRESOLVED — needs serial monitor

## Symptom

Firmware flashes successfully (progress bar 100%, `firmware.bin` deleted, no OTA error log), but device always boots back to `1.0.0-dev`.

Evidence:
- `GET /api/status` returns `"version": "1.0.0-dev"` after every flash
- No `build` or `resetReason` fields (only in our firmware)
- `GET /download?path=/boot.log` → "Item not found" — our firmware never wrote the boot log

## Root Cause Theory

`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=1` is in the pre-compiled ESP-IDF bootloader (confirmed in `sdkconfig.h`). New firmware must call `esp_ota_mark_app_valid_cancel_rollback()` before any watchdog fires. Our firmware crashes **before SD card init** (boot.log never written), triggering a watchdog reset, causing rollback.

## What Was Tried

### Attempt 1: Call in `setup()` (commit `05d4bf9`)
Added at top of `setup()` before `gpio.begin()`. Still rolled back — crash is before `setup()`.

### Attempt 2: `__attribute__((constructor(101)))` (commit `08cb07d`)
```cpp
__attribute__((constructor(101))) static void earlyMarkOtaValid() {
  esp_ota_mark_app_valid_cancel_rollback();
}
```
Confirmed in `.init_array.00101` in linker map. Still rolling back. Possible causes:
- ESP-IDF partition layer not initialized when constructor(101) runs on RISC-V
- Crash before even priority-101 constructors (very early crash)

## Crash Location

Boot log written after `Storage.begin()` → `SETTINGS.loadFromFile()`. Since never written, crash is in:

```
[global constructors]   ← before setup()
setup() {
  gpio.begin()          ← ???
  powerManager.begin()  ← ???
  Storage.begin()       ← ???
  SETTINGS.loadFromFile()
  // === BOOT LOG WRITTEN HERE === ← NEVER REACHED
```

## Suspected: Global Constructor Crash

```cpp
// src/main.cpp global scope — run BEFORE setup():
ActivityManager activityManager(renderer, mappedInputManager);
// Constructor calls: xSemaphoreCreateMutex() + assert(result != nullptr)
```

If FreeRTOS isn't ready when this runs, the mutex creation fails and `assert` fires.
(FreeRTOS heap IS available during constructors on ESP32 — but worth verifying with serial.)

## History

- **Pre-merge (≤ f146fed)**: Firmware ran successfully — PULSR theme confirmed in screencaps.
  Version was still `1.0.0-dev` (not bumped yet), so we couldn't detect rollback.
- **After upstream merge (f697e43)**: All subsequent builds roll back.
- **open-x4-sdk submodule**: SAME SHA (`91e7e2b`) before/after merge — NOT the culprit.
- **src/ changes in merge**: `main.cpp`, `PulsrTheme.cpp`, `CrossPointWebServer.cpp/.h`, 
  `RssFeedSync.cpp`, `HomePage.html`

## To Diagnose

**Serial monitor output during boot** is the missing piece:

```bash
pio device monitor --baud 115200
# Then trigger a flash: upload firmware.bin, sleep/wake reader
```

Serial will show exact crash location (panic dump, assert, or watchdog reason).

## Relevant Files

- `src/main.cpp:16-19` — `earlyMarkOtaValid` constructor
- `src/activities/ActivityManager.h:63-65` — `xSemaphoreCreateMutex()` in constructor
- `CLAUDE.md` — OTA rollback section
- `.skills/SKILL.md` — firmware dev loop

## Related Issues

- #1248 — Feed error log in web UI
- #1249 — Show version on install screen  
- #1250 — Firmware-ready notification in web UI
