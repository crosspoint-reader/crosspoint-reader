# AUDIT REPORT -- biscuit. firmware
**Date:** 2026-04-06
**Target:** ESP32-C3 custom firmware (Xteink X4 e-ink reader)
**Scope:** Full codebase audit -- 7 phases, ~90 source files
**Excluded:** 6 wireless testing activities (manually maintained)

---

## Executive Summary

| Severity | Count |
|----------|-------|
| CRITICAL | 18    |
| HIGH     | 18    |
| MEDIUM   | 27    |
| LOW      | 28    |
| **Total**| **91**|

### Top 3 Risks

1. **Data race in ISR/callback contexts (MEM-003, MEM-004, LIFE-004):** Multiple activities modify `std::vector` from WiFi promiscuous callbacks without synchronization. This causes heap corruption and crashes on the single-core ESP32-C3.

2. **Security features are non-functional or trivially bypassed (SEC-004, SEC-006, SEC-007, SEC-010):** Duress mode path redirection is never called (completely dead code). Dead Drop has zero encryption. TLS certificate validation is globally disabled. Password store uses reversible XOR with the publicly-broadcast MAC address.

3. **Radio state leaks across activities (LIFE-011, LIFE-019, LIFE-020, LIFE-021):** At least 4 activities manually tear down WiFi without calling `RADIO.shutdown()`, leaving RadioManager's internal state inconsistent. Next activity may skip radio initialization.

---

## Phase 1: Memory & Stability

### CRITICAL

**MEM-001** | `activities/apps/ProbeSnifferActivity.cpp` | Lines 65-72
Heap allocation (`push_back` with `std::string`) inside `portENTER_CRITICAL`. `malloc` is forbidden with interrupts disabled on ESP32-C3. Can deadlock or corrupt heap.
**Fix:** Collect raw data into a fixed-size ring buffer inside the critical section. Move string construction and `push_back` to `loop()`.

**MEM-002** | `activities/apps/ProbeSnifferActivity.cpp` | Lines 143-146
`std::sort` called on a vector inside `portENTER_CRITICAL`. O(n log n) with up to 100 entries blocks all interrupts, risking watchdog reset. Additionally `push_back` at line 72 does heap allocation with interrupts disabled.
**Fix:** Copy vector out of critical section, sort the copy, swap back. Pre-allocate with `reserve(MAX_ENTRIES)` in `onEnter()`.

**MEM-003** | `activities/apps/DeviceFingerprinterActivity.cpp` | Line 102
`onProbePacket()` called from WiFi promiscuous callback modifies `devices` vector (including `push_back` heap reallocation) with **no mutex**. `loop()` and `render()` read concurrently. Data race -> heap corruption.
**Fix:** Add `portMUX_TYPE` spinlock around all `devices` access, or use a fixed-size ring buffer for the callback.

**MEM-004** | `activities/apps/CrowdDensityActivity.cpp` | Lines 35-43
`addMac()` called from WiFi promiscuous callback modifies `seenMacs`/`seenMacCount` without synchronization. `loop()` resets these fields concurrently.
**Fix:** Add `portMUX_TYPE` spinlock.

**MEM-005** | `activities/apps/PacketMonitorActivity.cpp` | Line 69
`fileMux = xSemaphoreCreateMutex()` in `startPcapRecording()` but `vSemaphoreDelete(fileMux)` never called. Semaphore leaks ~80 bytes per activity session.
**Fix:** Add `if (fileMux) { vSemaphoreDelete(fileMux); fileMux = nullptr; }` to `onExit()`.

**MEM-006** | `network/OtaUpdater.cpp` | Lines 14-15
File-scope globals `char* local_buf` and `int output_len` mutated by `event_handler`. NULL `local_buf` passed to `deserializeJson` is undefined behavior.
**Fix:** Move into class members, pass via `user_data`. Add NULL check.

### HIGH

**MEM-007** | `activities/apps/BleProximityActivity.cpp`
`devices` vector grows without cap. In busy BLE environment, unbounded heap consumption.
**Fix:** Add `if (devices.size() >= MAX_DEVICES) return;` before `push_back`.

**MEM-008** | `activities/apps/BleScannerActivity.cpp`
Three vectors (`devices`, `services`, `characteristics`) grow uncapped. `BLEClient*` managed with raw `new`/`delete` -- leak risk on error paths.
**Fix:** Cap all vectors. Use `std::unique_ptr<BLEClient>`.

**MEM-009** | `activities/apps/WardrivingActivity.h` | Lines 31-39
`MAX_NETWORKS = 500` with `std::string` members. At capacity: ~40-50KB heap usage (>10% of SRAM) plus severe fragmentation from 1000 small string allocations.
**Fix:** Use `char ssid[33]`, `char bssid[18]` instead of `std::string`. Reduce to 200.

**MEM-010** | `activities/apps/BulletinBoardActivity.cpp`, `DeadDropActivity.cpp`
Raw `new WebServer(80)` without RAII. If `onExit()` skips stop method, server leaks.
**Fix:** Use `std::unique_ptr<WebServer>`.

**MEM-011** | `activities/network/CrossPointWebServerActivity.cpp` | Line 31
Raw `new DNSServer()` in anonymous namespace. File-scope global persists if cleanup missed.
**Fix:** Use `std::unique_ptr<DNSServer>` as class member.

**MEM-012** | `activities/apps/EtchASketchActivity.cpp` | Line 22
Canvas allocation: 48,000 bytes (12.6% of SRAM). Properly guarded with `std::nothrow` but very risky at this size.
**Fix:** Document memory requirement. Consider smaller resolution.

**MEM-013** | `activities/reader/TxtReaderActivity.cpp` | Line 179
`buildPageIndex()` called from `render()` iterates entire file with `vTaskDelay(1)`, holding RenderLock the entire time. Blocks rendering indefinitely for large files.
**Fix:** Move `initializeReader()`/`buildPageIndex()` to `onEnter()` or `loop()`.

**MEM-014** | `activities/apps/DeadDropActivity.cpp`
`handleFileList()` allocates `char buf[2048]` + `char name[256]` = 2304 bytes on stack. Runs in WebServer handler context which may have smaller stack.
**Fix:** Reduce buffer sizes or use heap allocation. Build JSON incrementally.

### MEDIUM

**MEM-015** | `activities/apps/NetworkMonitorActivity.h` | Lines 72-77
`std::vector<ApRecord>` with `std::string` members, 200 cap. ~12KB+ with fragmentation.
**Fix:** Use `char ssid[33]`, `char bssid[18]`.

**MEM-016** | `activities/apps/ChessActivity.cpp`
`botMove()` and `hasAnyLegalMove()` create temporary `std::vector<Move>` each call. Heap churn.
**Fix:** Use member vector with `clear()` + `reserve()`.

**MEM-017** | `activities/apps/CredentialViewerActivity.cpp` | Line 69
`creds` vector unbounded. Large `creds.csv` causes many heap allocations.
**Fix:** Cap at 100 entries.

**MEM-018** | `activities/apps/SdFileBrowserActivity.cpp` | Lines 116-131
`viewContent` up to 16KB. `wrappedText()` in render doubles memory temporarily.
**Fix:** Reduce `MAX_VIEW_BYTES` to 8KB. Cache wrapped lines.

**MEM-019** | `activities/apps/HttpClientActivity.cpp` | Lines 178, 252
`wrappedText()` called every render, creating new `std::vector<std::string>` each time.
**Fix:** Cache wrapped lines, clear on content change.

**MEM-020** | `activities/apps/EventLoggerActivity.cpp` | Lines 29-39
`lines.erase(lines.begin())` is O(n) on vector. Character-by-character file reading is very slow.
**Fix:** Use `std::deque` or read from file end. Read in 512-byte chunks.

**MEM-021** | `activities/apps/FlashcardActivity.cpp` | Lines 37-58
Character-by-character file reading via SPI. Extremely slow on SD card.
**Fix:** Read in chunks (512 bytes).

**MEM-022** | `activities/apps/GameOfLifeActivity.h`
`uint8_t grid[48][80]` + `nextGrid[48][80]` = 7680 bytes. Contiguous heap allocation.
**Fix:** Use packed bit arrays (960 bytes total).

**MEM-023** | `activities/apps/MdnsBrowserActivity.cpp` | Lines 125-131
`ServiceResult` with four `std::string` members, up to 50 results. Fragmentation.
**Fix:** Use fixed-size char arrays.

### LOW

**MEM-024** | `activities/apps/PacketMonitorActivity.h`
`uint64_t macTable[256]` = 2KB member. Acceptable but document.

**MEM-025** | `activities/apps/BleScannerActivity.cpp`
`saveToCsv()` builds Arduino String with `+=` in loop. Heap fragmentation.
**Fix:** Write directly to file handle.

**MEM-026** | `activities/apps/TrackerDetectorActivity.cpp` | Line 361
`std::vector<int> flaggedIndices` created in every render call.
**Fix:** Cache as member, update when `checkForFollowers()` runs.

**MEM-027** | `activities/reader/EpubReaderActivity.cpp` | Line 32
`const std::vector<int> PAGE_TURN_LABELS` file-scope global. Heap allocation at startup.
**Fix:** Replace with `static constexpr int PAGE_TURN_LABELS[] = {1, 1, 3, 6, 12};`

**MEM-028** | Multiple activities
`onExit()` doesn't clear member vectors/strings. Destructor handles it but memory freed late.
**Fix:** Add `.clear()` calls in `onExit()` for earlier reclamation.

---

## Phase 2: RadioManager & Wireless

### CRITICAL

**LIFE-001** | `activities/apps/MeshChatActivity.cpp` | Lines 106-121
`onDataRecv()` ESP-NOW callback modifies `activeInstance->peers` vector (including `push_back`) with **no mutex**. `loop()` and `render()` access concurrently. Heap corruption.
**Fix:** Add FreeRTOS mutex. ESP-NOW callbacks run in task context (not ISR), so `xSemaphoreTake` is safe.

**LIFE-002** | `activities/apps/SweepActivity.cpp` | Lines 86-254
Entire scan chain (`scanWifiCameras` -> `scanWifiKarma` -> `scanBleThreats`) blocks `loop()` for ~30+ seconds synchronously. Watchdog timer (default 5s) will fire. Button input completely blocked.
**Fix:** Break into async phases driven by `loop()` iterations. Feed watchdog between phases.

### HIGH

**LIFE-003** | `activities/apps/MeshChatActivity.cpp` | Line 18
Redefines `#define RADIO RadioManager::getInstance()`. Fragile if RadioManager.h definition changes.
**Fix:** Remove redundant `#define`.

**LIFE-004** | `activities/apps/BleContactExchangeActivity.cpp` | Line 18
Same redundant `#define RADIO` redefinition.
**Fix:** Remove.

### MEDIUM

**LIFE-005** | `activities/apps/PerimeterWatchActivity.cpp` | Line 168
Unsigned underflow: `SCAN_INTERVAL_MS - (millis() - lastScanMs)` when elapsed > interval. Displays garbage "Next scan: 4294967XXXs".
**Fix:** `unsigned long elapsed = millis() - lastScanMs; unsigned long nextScan = (elapsed < SCAN_INTERVAL_MS) ? (SCAN_INTERVAL_MS - elapsed) / 1000UL : 0;`

**LIFE-006** | `activities/apps/BreadcrumbTrailActivity.cpp` | Lines 325-326
Similar unsigned underflow in "Scan in" display.
**Fix:** Compute elapsed first, guard subtraction.

**LIFE-007** | `activities/apps/WardrivingActivity.h` | Lines 31-39
500-entry vector with `std::string` members. See MEM-009.

**LIFE-008** | `activities/apps/BreadcrumbTrailActivity.h` | Lines 31-32
`trail` vector (200 entries, 48 bytes each) without `reserve()`. Repeated reallocations.
**Fix:** Call `trail.reserve(MAX_CRUMBS)` in `onEnter()`.

---

## Phase 3: Activity Lifecycle

### CRITICAL

**LIFE-009** | `activities/apps/WifiConnectActivity.cpp` | Lines 33-37
`onExit()` calls `WiFi.scanDelete()` but NOT `RADIO.shutdown()`. Radio left powered on.
**Fix:** Call `RADIO.shutdown()` or document the contract.

### HIGH

**LIFE-010** | `activities/network/WifiSelectionActivity.cpp` | Lines 468-469
`render()` returns early in `PASSWORD_ENTRY` state without calling `displayBuffer()`. Stale frame visible.
**Fix:** Call `renderer.displayBuffer()` before returning or skip update request.

**LIFE-011** | `activities/network/CalibreConnectActivity.cpp` | Lines 51-62
Manual `WiFi.disconnect()`/`WiFi.mode(WIFI_OFF)` without `RADIO.shutdown()`. RadioManager state inconsistent.
**Fix:** Replace with `RADIO.shutdown()`.

**LIFE-012** | `activities/settings/OtaUpdateActivity.cpp` | Lines 66-73
Same issue: manual WiFi teardown without `RADIO.shutdown()`.
**Fix:** Call `RADIO.shutdown()`.

**LIFE-013** | `activities/reader/KOReaderSyncActivity.cpp` | Lines 217-221
`wifiOff()` does manual WiFi teardown without `RADIO.shutdown()`.
**Fix:** Call `RADIO.shutdown()`.

### MEDIUM

**LIFE-014** | Multiple activities
`static unsigned long lastUpdate = 0` in loop() persists across activity instances, causing stale timer behavior on re-entry.
**Fix:** Use member variables initialized in `onEnter()`.

**LIFE-015** | `activities/apps/PerimeterWatchActivity.cpp` | Lines 133-140
`buttonNavigator` called unconditionally when count == 0. Meaningless navigation.
**Fix:** Guard with `if (count > 0)`.

### LOW

**LIFE-016** | `activities/apps/WifiCredsActivity.h`
No `onExit()` override. WiFi password strings persist in memory longer than necessary.
**Fix:** Add `onExit()` that clears `ssid` and `password`.

**LIFE-017** | Multiple activities
`static` local arrays (`TransitAlertActivity`, `VehicleFinderActivity`, `BreadcrumbTrailActivity`) persist in BSS permanently. Not a bug but wastes ~400 bytes.

---

## Phase 4: Display & UI

### CRITICAL

**DISP-001** | `activities/settings/OtaUpdateActivity.cpp` | Line 93
`render()` returns early (after `clearScreen()`) without calling `displayBuffer()` during OTA progress. E-ink controller left in undefined state.
**Fix:** Move early-return check before `clearScreen()`, or always call `displayBuffer()`.

**DISP-002** | `activities/apps/SdFileBrowserActivity.cpp` | Line 350, `HttpClientActivity.cpp` | Line 252
`wrappedText()` with `maxLines=0` creates unbounded heap allocation in render (holds RenderLock). 16KB text file produces hundreds of wrapped lines with separate heap allocations.
**Fix:** Pass `visibleLines + scrollOffset` as maxLines.

### HIGH

**DISP-003** | `activities/apps/MedicalCardActivity.cpp` | Lines 302-310
QR buffer `char qrBuf[512]` can overflow. Max field content = 508 bytes + ~40 bytes format overhead = ~548 bytes.
**Fix:** Increase to 640 bytes.

**DISP-004** | `activities/home/HomeActivity.cpp` | Line 150
`coverBuffer = static_cast<uint8_t*>(malloc(bufferSize))` allocates 48,000 bytes. Freed in `onExit()` but no RAII.
**Fix:** Use `std::unique_ptr<uint8_t[]>`.

### MEDIUM

**DISP-005** | `activities/apps/CredentialViewerActivity.cpp` | Lines 200-217
DETAIL_VIEW renders long strings without truncation. Text drawn off-screen.
**Fix:** Use `renderer.truncatedText()` or pass max width.

**DISP-006** | `activities/network/CrossPointWebServerActivity.cpp` | Lines 354, 374
Header drawn twice in `renderServerRunning()`. Redundant work.
**Fix:** Remove duplicate call.

**DISP-007** | `activities/apps/CredentialViewerActivity.cpp` | Line 26
Entire `creds.csv` loaded into Arduino String with no size limit. Large file exhausts heap.
**Fix:** Check file size first, cap at 16KB.

### LOW

**DISP-008** | `components/themes/lyra/LyraTheme.cpp` | Line 44
Mutable namespace-scope `int coverWidth = 0` retains stale value across calls.
**Fix:** Make local variable.

**DISP-009** | 20+ app activities
Hardcoded English strings in render() instead of `tr(STR_*)`. See Phase 7 for full list.

---

## Phase 5: Storage & Filesystem

### CRITICAL

**STOR-001** | `activities/apps/QuickWipeActivity.cpp` | Lines 46-123
Recursive `wipeDirectory()` allocates ~4.7KB stack per frame (`char names[16][256]` + `char fullPath[512]`). Directory nesting depth of 2 overflows the 8KB stack.
**Fix:** Flatten recursion into iterative approach with explicit heap stack. Or reduce `NAME_LEN` to 64.

### HIGH

**STOR-002** | `CrossPointSettings.h` | Lines 186-188
OPDS credentials stored as plaintext char arrays. Serialized to JSON on SD without obfuscation. WiFiCredentialStore and PasswordStore both use XOR obfuscation.
**Fix:** Apply `obfuscation::obfuscateToBase64()` for OPDS credentials.

**STOR-003** | `activities/apps/MedicalCardActivity.cpp` | Lines 62-78
Binary struct written/read without versioning. Firmware update changing struct layout corrupts data.
**Fix:** Add version header byte, or migrate to JSON serialization.

**STOR-004** | `activities/apps/HabitTrackerActivity.cpp`
Binary struct read without validating return value matches `sizeof(data)`. Truncated file leaves struct partially initialized with garbage.
**Fix:** Check `file.read()` return value. `memset(&data, 0, sizeof(data))` before reading.

### MEDIUM

**STOR-005** | `activities/apps/DeadDropActivity.cpp` | Line 181
JSON buffer `char buf[2048]` silently truncates with many/long filenames. Produces malformed JSON.
**Fix:** Use ArduinoJson for dynamic building, or limit enumerated files.

**STOR-006** | `stores/PasswordStore.cpp` | Line 11
Path `/.crosspoint/passwords.json` uses legacy naming. Inconsistent with `/biscuit/` convention.
**Fix:** Migrate path or add backward compatibility.

**STOR-007** | `activities/boot_sleep/SleepActivity.cpp` | Lines 54-82
`std::vector<std::string> files` collects all BMP filenames from sleep directory without limit.
**Fix:** Cap at 50 files or use reservoir sampling.

### LOW

**STOR-008** | `activities/apps/FlashcardActivity.cpp`
CSV parser uses fixed `char line[300]` buffer. Long entries silently truncated.
**Fix:** Use `std::string` or document 299-char limit.

---

## Phase 6: Security

### CRITICAL

**SEC-001** | `activities/apps/SecurityPinActivity.cpp` | Line 28
PIN comparison uses `memcmp` (non-constant-time). Timing side-channel leaks hash bytes.
**Fix:** Use `mbedtls_ct_memcmp()` or volatile XOR-accumulator.

**SEC-002** | `activities/apps/TotpActivity.cpp` | Lines 74-81
TOTP secrets stored as plaintext binary on SD card (`/biscuit/totp.dat`). Physical SD access = all 2FA secrets compromised.
**Fix:** Apply device-bound obfuscation. Ideally encrypt with PIN-derived key via AES-128-CBC.

**SEC-003** | `activities/apps/TotpActivity.cpp` | Lines 56-60
TOTP key material (`uint8_t key[40]`) not zeroized after use. Persists on stack.
**Fix:** `mbedtls_platform_zeroize(key, sizeof(key))` after use.

**SEC-004** | `util/DuressManager.h` | Lines 17-21
**Duress mode is completely non-functional.** `resolvePath()` is never called anywhere in the codebase. All file access uses real `/biscuit/` paths regardless of duress state. An attacker with the duress PIN sees real data.
**Fix:** Route all file paths through `DURESS.resolvePath()`. Integrate into `HalStorage.open()`. Pre-populate `/biscuit_safe/` with dummy data.

**SEC-005** | `activities/apps/CredentialViewerActivity.cpp` | Line 26
Captured credentials in `/biscuit/creds.csv` are plaintext. `deleteAllCredentials()` only unlinks FAT32 entry -- data remains on SD sectors, recoverable with forensics.
**Fix:** Overwrite-before-delete (like QuickWipeActivity). Apply obfuscation.

**SEC-006** | `activities/apps/DeadDropActivity.cpp` | Lines 107-118
Dead Drop web server is plain HTTP on open WiFi AP. Zero encryption at any layer. All transferred files visible to nearby sniffers.
**Fix:** Add WPA2 password to SoftAP at minimum. Display password on screen.

**SEC-007** | `network/HttpDownloader.cpp` | Line 60
`secureClient->setInsecure()` disables TLS certificate verification globally. OPDS credentials sent via HTTP Basic Auth over connection with no cert validation. MITM trivial.
**Fix:** Remove `setInsecure()`. Use `esp_crt_bundle_attach` or `setCACert()`.

### HIGH

**SEC-008** | `network/OtaUpdater.cpp` | Lines 203-272
OTA firmware downloaded and flashed with no signature verification. `skip_cert_common_name_check = true` further weakens TLS.
**Fix:** Implement firmware signing (SHA-256 hash or Ed25519 against hardcoded public key). Remove CN check skip.

**SEC-009** | `activities/apps/SecurityPinActivity.cpp` | Lines 22-23
PIN hashing uses plain SHA-256 with no salt. 10K-1M PIN space cracked in milliseconds.
**Fix:** Use PBKDF2 with 100K iterations and random 16-byte salt.

**SEC-010** | `stores/PasswordStore.cpp` | Lines 22-23
Password store uses XOR with 6-byte key derived from device MAC (publicly broadcast in every WiFi frame). Trivially reversible.
**Fix:** AES-128/256-CBC with key derived from security PIN via PBKDF2 + MAC salt.

**SEC-011** | `activities/apps/MeshChatActivity.cpp` | Lines 82-137
ESP-NOW frames broadcast in cleartext (`encrypt = false`). No HMAC, no sequence numbers, no authentication. Message injection and replay trivial.
**Fix:** Add HMAC-SHA256 per frame with pre-shared key. Include monotonic counter.

**SEC-012** | `activities/apps/EmergencyActivity.cpp` | Lines 56-78, 111-133
Emergency SOS broadcasts personal identity, medical data, and phone number in plaintext via ESP-NOW and open WiFi AP SSID containing user's name.
**Fix:** Display clear warning before arming. Allow field-level opt-out.

**SEC-013** | `network/CrossPointWebServer.cpp` | Lines 94-193, `network/WebDAVHandler.cpp`
Web server and WebDAV have zero authentication. Any device on same WiFi can browse/upload/download/delete all files including password stores and TOTP secrets.
**Fix:** Implement HTTP Basic Auth with configurable password.

### MEDIUM

**SEC-014** | `activities/apps/DeadDropActivity.cpp` | Lines 138-141
Filename sanitization strips `/` but not `\`. Path traversal possible on some FAT32 implementations.
**Fix:** Strip both `/` and `\`. Reject `..` entirely.

**SEC-015** | `activities/apps/DeadDropActivity.cpp` | Lines 156-159
No upload size limit. Malicious client can fill entire SD card.
**Fix:** Track bytes written, abort at configurable max (e.g., 10MB).

**SEC-016** | `activities/apps/QuickWipeActivity.cpp` | Lines 126-128
QuickWipe only covers `/biscuit/`. Password store at `/.crosspoint/passwords.json` and WiFi creds survive.
**Fix:** Add `/.crosspoint/` to wipe paths.

**SEC-017** | `activities/apps/SecurityPinActivity.cpp` | Lines 56-69
Normal and duress PIN hashes stored at known file offsets. Both unsalted SHA-256 -- both crackable. Attacker can distinguish which is real vs duress.
**Fix:** Compounds SEC-009. With salted PBKDF2, store hashes in random order.

**SEC-018** | `lib/FsHelpers/FsHelpers.cpp` | Lines 9-43
`normalisePath()` doesn't handle trailing `..` (no trailing slash). Path traversal via `"/foo/bar/.."`.
**Fix:** Add `..` check in trailing-component block.

**SEC-019** | `network/CrossPointWebServer.cpp` | Lines 488-498
Download handler checks only last path segment for dot-files. `/.crosspoint/passwords.json` passes check.
**Fix:** Port per-segment check from WebDAVHandler's `isProtectedPath()`.

### LOW

**SEC-020** | `stores/PasswordStore.h` | Line 14
Password entries persist in global singleton heap memory after activity exit. Memory dump reveals all passwords.
**Fix:** Add `clearEntries()` method, call from `PasswordManagerActivity::onExit()`.

**SEC-021** | `activities/apps/SecurityPinActivity.cpp` | Lines 264-295
`newPin` buffer not zeroized on all exit paths. Persists in RAM.
**Fix:** Zero both `pinBuffer` and `newPin` in `onExit()`.

**SEC-022** | `activities/apps/ScreenDecoyActivity.cpp` | Line 314
Fake error screen shows `"crosspoint.dev/help"` revealing device identity.
**Fix:** Use generic text: `"Contact manufacturer support"`.

**SEC-023** | `activities/apps/RfSilenceActivity.cpp` | Lines 27-32
RF Silence doesn't verify BLE fully de-initialized. `ESP_BT_CONTROLLER_STATUS_IDLE` may be false positive.
**Fix:** Call `esp_bt_controller_disable()` and `esp_bt_controller_deinit()` explicitly.

---

## Phase 7: Code Quality

### HIGH

**CODE-001** | 5+ activities
WiFi teardown pattern `WiFi.disconnect(); delay(100); WiFi.mode(WIFI_OFF); delay(100);` copy-pasted across OtaUpdateActivity, KOReaderAuthActivity, KOReaderSyncActivity, CalibreConnectActivity, CrossPointWebServerActivity with inconsistent delay values.
**Fix:** Consolidate into `RadioManager::shutdownWifi()`. All activities use `RADIO.shutdown()`.

**CODE-002** | `activities/apps/UsbHidActivity.cpp` | 9 TODOs
9 TODO comments mark completely unimplemented TinyUSB HID stubs. Activity renders full UI but does nothing.
**Fix:** Implement TinyUSB integration or hide from app menu.

**CODE-003** | `activities/apps/UsbMassStorageActivity.cpp` | 2 TODOs
Same: completely unimplemented stubs with rendered UI.
**Fix:** Implement or hide.

**CODE-004** | `activities/network/WifiSelectionActivity.cpp` | Line 225
WiFi hostname hardcoded as `"CrossPoint-Reader-"` + MAC. User-visible on routers.
**Fix:** Change to `"Biscuit-Reader-"`.

### MEDIUM

**CODE-005** | `CrossPointSettings.h`, `CrossPointState.h`, `CrossPointWebServer.h`, `CrossPointWebServerActivity.h`
Core classes still named `CrossPoint*`. Propagates to 50+ references.
**Fix:** Rename to `Biscuit*` or add `using` aliases.

**CODE-006** | `activities/apps/DeadDropActivity.cpp`, `BulletinBoardActivity.cpp`
Nearly identical `stopDrop()`/`stopBoard()` and `setupWebServer()` patterns.
**Fix:** Extract shared AP+WebServer lifecycle helper.

**CODE-007** | 15 files, 22 occurrences
`"/.crosspoint"` string literal hardcoded. No single source of truth for data directory path.
**Fix:** Define `constexpr char DATA_DIR[] = "/.crosspoint"` in shared header.

**CODE-008** | `activities/Activity.h` | Line 9
`Activity.h` includes `"ActivityManager.h"` creating circular conceptual dependency. Every activity transitively includes ActivityManager, vector, FreeRTOS headers.
**Fix:** Remove include. Move extern declaration to .cpp files that need it.

**CODE-009** | `util/ScreenshotUtil.cpp` | Line 31
`delay(1000)` blocks main loop for 1 second during screenshot feedback.
**Fix:** Use millis()-based timer.

**CODE-010** | `main.cpp` | Line 256
TODO: `"move this to an activity when we have one to display the panic info"`. `clearPanic()` called unconditionally, losing crash diagnostics.
**Fix:** Create PanicInfoActivity or log panic info to SD.

**CODE-011** | `activities/reader/EpubReaderActivity.cpp` | Lines 489, 626, 752
Three TODOs: missing error recovery for section load, no retry bound on page load, hyphenation applied without font capability check.

**CODE-012** | 20+ app activities
80+ hardcoded English strings not wrapped in `tr()`. Core reader/settings use i18n but most apps don't.
**Fix:** Systematic i18n pass on app activities.

**CODE-013** | `activities/apps/UsbMassStorageActivity.cpp`, `CasinoActivity.cpp`
Dozens of hardcoded pixel values for drawing (y offsets, box dimensions, icon sizes).
**Fix:** Define named layout constants.

### LOW

**CODE-014** | `activities/boot_sleep/SleepActivity.cpp` | Line 134, 351
Uses `tr(STR_CROSSPOINT)` -- old brand name in i18n key.
**Fix:** Rename to `STR_BISCUIT`.

**CODE-015** | `main.cpp` | Line 284
`CROSSPOINT_VERSION` build define still uses old name.
**Fix:** Rename to `BISCUIT_VERSION`.

**CODE-016** | 5 files
`Storage.mkdir("/.crosspoint")` called redundantly before every save.
**Fix:** Call once during boot.

**CODE-017** | `activities/reader/ReaderUtils.h` | Line 3
`#include <CrossPointSettings.h>` uses angle brackets for project-local header.
**Fix:** Use quotes.

**CODE-018** | `activities/home/FileBrowserActivity.cpp` | Line 84
`char name[500]` on stack -- oversized for FAT32 max 255.
**Fix:** Reduce to 256.

**CODE-019** | 8 groups (~20 calls)
`delay()` calls in `onExit()` WiFi teardown across 6 activities. Total ~310ms+ blocking.
**Fix:** Consolidate into RadioManager (see CODE-001).

**CODE-020** | `activities/apps/DeviceInfoActivity.cpp` | Line 63
`snprintf(..., 80, ...)` uses hardcoded size that must match array declaration.
**Fix:** Use `sizeof(lines[0])`.

---

## Recommended Fix Order

### Priority 1: Crash / Data Corruption (do first)
| ID | Issue | Est. Effort |
|----|-------|-------------|
| MEM-003 | DeviceFingerprinterActivity data race | 30 min |
| MEM-004 | CrowdDensityActivity data race | 20 min |
| LIFE-001 | MeshChatActivity data race | 30 min |
| MEM-001/002 | ProbeSnifferActivity critical section | 1 hr |
| LIFE-002 | SweepActivity 30s blocking loop | 2 hr |
| STOR-001 | QuickWipeActivity stack overflow | 1 hr |
| MEM-005 | PacketMonitorActivity semaphore leak | 10 min |
| MEM-006 | OtaUpdater NULL pointer | 30 min |

### Priority 2: Security (do second)
| ID | Issue | Est. Effort |
|----|-------|-------------|
| SEC-004 | Duress mode non-functional | 2 hr |
| SEC-007 | TLS validation disabled | 30 min |
| SEC-013 | Web server no auth | 2 hr |
| SEC-001 | PIN timing side-channel | 15 min |
| SEC-002 | TOTP plaintext secrets | 1 hr |
| SEC-009 | PIN unsalted SHA-256 | 1 hr |
| SEC-016 | QuickWipe misses /.crosspoint/ | 15 min |
| SEC-006 | Dead Drop no encryption | 1 hr |
| SEC-019 | Web server path traversal | 30 min |
| SEC-018 | normalisePath traversal | 15 min |

### Priority 3: Radio State Consistency (do third)
| ID | Issue | Est. Effort |
|----|-------|-------------|
| CODE-001 | Extract WiFi teardown into RadioManager | 1 hr |
| LIFE-011/012/013 | Replace manual WiFi teardown in 4 activities | 30 min |

### Priority 4: Memory Optimization (do fourth)
| ID | Issue | Est. Effort |
|----|-------|-------------|
| MEM-009 | WardrivingActivity 500 strings -> char[] | 30 min |
| MEM-012/DISP-004 | Large allocations (48KB canvas, 48KB cover) | 1 hr |
| DISP-002 | Unbounded wrappedText in render | 30 min |
| MEM-013 | TxtReader buildPageIndex in render | 1 hr |

### Priority 5: Quality / Branding (do last)
| ID | Issue | Est. Effort |
|----|-------|-------------|
| CODE-002/003 | Stub activities (UsbHid, UsbMassStorage) | hide: 15 min |
| CODE-005 | CrossPoint -> Biscuit class renames | 2 hr |
| CODE-012 | i18n pass on app activities | 4 hr |

---

## Architecture Notes

### 1. RadioManager is not enforced
Activities can (and do) bypass RadioManager with direct `WiFi.*` and `esp_wifi_*` calls. RadioManager's internal state then diverges from reality. Consider making RadioManager the **sole** interface by removing direct WiFi/BLE API access from activities (enforce via code review or a wrapper).

### 2. No memory budget system
There is no mechanism to track or limit per-activity heap usage. Activities with unbounded vectors (Wardriving: 500, NetworkMonitor: 200, BleProximity: unlimited) compete for the same 380KB. Consider a simple heap watermark check in `ActivityManager::pushActivity()` that warns or blocks if free heap < 120KB.

### 3. Render path allocates memory
Multiple activities create temporary `std::string` vectors inside `render()` (via `wrappedText()`). The render path holds a mutex and runs on the main task. Memory allocation failures here are unrecoverable. Consider pre-computing wrapped text in `loop()` when content changes, then rendering from cache.

### 4. File I/O inconsistency
Three different patterns exist: (a) ArduinoJson via `JsonSettingsIO`, (b) raw binary struct read/write (MedicalCard, HabitTracker), (c) CSV text (Credentials, Flashcards). Pattern (b) is fragile across firmware updates. Standardize on JSON for small config files and CSV for tabular data with header rows for forward compatibility.

### 5. Security architecture needs layering
The current security model has no defense in depth. The security PIN protects boot but doesn't encrypt data at rest. The password store obfuscation uses a publicly-known key (MAC). Duress mode is a no-op. The fix path is:
1. Make PBKDF2(PIN) the master key for all encryption
2. Wire DuressManager.resolvePath() into HalStorage
3. Add WebServer auth using the same PIN or a separate password
4. Enable TLS certificate validation

### 6. CrossPoint -> Biscuit migration
The rebrand is ~60% complete. User-visible remnants (WiFi hostname, decoy screen URL, `STR_CROSSPOINT` i18n key) should be fixed urgently. Internal class names (`CrossPointSettings`, `CrossPointState`, `CrossPointWebServer`) can be aliased with `using` for backward compatibility and renamed incrementally.
