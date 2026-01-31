# Xteink X4 Extension System Testing Guide

## Pre-Test Setup

### Prerequisites
- Xteink X4 device with USB-C cable
- SD card (formatted as FAT32)
- Computer with PlatformIO installed
- Serial monitor (VS Code or `pio device monitor`)

### Files You Need
1. **Main firmware**: `.pio/build/default/firmware.bin`
2. **Hello World extension**: `.pio/build/hello-world/firmware.bin`
3. **Test app manifest**: `app.json`

---

## Step 1: Prepare the SD Card

### 1.1 Create Directory Structure
On your computer, create this folder structure on the SD card:
```
/.crosspoint/apps/hello-world/
```

### 1.2 Create App Manifest
Create file `/.crosspoint/apps/hello-world/app.json` with content:
```json
{
  "name": "Hello World",
  "version": "1.0.0",
  "description": "Test extension for CrossPoint",
  "author": "Test User",
  "minFirmware": "0.14.0"
}
```

### 1.3 Copy Hello World Binary
1. Build Hello World: `pio run -e hello-world`
2. Copy `.pio/build/hello-world/firmware.bin` to SD card
3. Rename it to `app.bin` on the SD card:
   ```
   /.crosspoint/apps/hello-world/app.bin
   ```

---

## Step 2: Flash Main Firmware

### 2.1 Build Main Firmware
```bash
pio run
```

### 2.2 Upload to Device
Connect Xteink X4 via USB-C and run:
```bash
pio run -t upload
```

### 2.3 Verify Boot
Open serial monitor:
```bash
pio device monitor -b 115200
```

You should see:
```
[    0] [   ] Starting CrossPoint version 0.14.0-dev
```

---

## Step 3: Test the Apps Menu

### 3.1 Navigate to Apps Menu
1. Power on the device
2. You should see the Home screen with menu items:
   - Continue Reading (if applicable)
   - Browse Files
   - OPDS Library (if configured)
   - File Transfer
   - **Apps** ← New menu item
   - Settings

3. Use **Down** button to navigate to "Apps"
4. Press **Select** button

### 3.2 Expected Behavior
- AppsActivity should load
- Screen shows "Hello World v1.0.0" in the list
- "No apps found" message should NOT appear

---

## Step 4: Test App Flashing

### 4.1 Launch Hello World
1. In AppsActivity, "Hello World" should be highlighted
2. Press **Select** button
3. Screen should show "Flashing App..." with progress bar
4. Progress should go from 0% to 100%

### 4.2 Verify Serial Output
Check serial monitor for:
```
[xxxxx] [AppLoader] Flashing to partition: ota_X (offset: 0xYYYYYY)
[xxxxx] [AppLoader] Battery: XX% - OK
[xxxxx] [AppLoader] Writing chunk X/Y
[xxxxx] [AppLoader] Flash complete. Rebooting...
```

### 4.3 Device Reboots
- Device should automatically reboot
- Should boot into Hello World app (not main firmware)
- Screen shows "Hello World!"

---

## Step 5: Test Return to Launcher

### 5.1 Exit via Back Button
1. While in Hello World, press **Back** button
2. Device should reboot
3. Should return to main CrossPoint launcher (HomeActivity)

### 5.2 Verify Return
- Screen shows HomeActivity (not Hello World)
- All menu items visible
- Serial shows:
  ```
  [HelloWorld] Triggering return to launcher...
  ```

### 5.3 Alternative: Power Button
1. Go back to Apps → Launch Hello World
2. Press **Power** button to sleep
3. Press **Power** again to wake
4. Should return to launcher

---

## Step 6: Test Safety Features

### 6.1 RTC Watchdog (Boot Loop Protection)
**⚠️ WARNING: This test simulates a crash. Have USB cable ready.**

To simulate a bad extension:
1. Create a dummy `/.crosspoint/apps/crash-test/app.json`
2. Copy a corrupted or incompatible `app.bin` (can use random bytes)
3. Try to launch it
4. Device will boot, crash, and reboot
5. After 3 failed boots, should auto-return to launcher

**Expected**: Device returns to launcher after 3 failed attempts, not stuck in boot loop.

### 6.2 Low Battery Protection
1. Discharge device to < 20% battery
2. Try to launch an app
3. Should show error: "Battery too low (XX%). Charge to continue."
4. Flash should NOT proceed

### 6.3 Missing app.bin
1. Delete `/.crosspoint/apps/hello-world/app.bin` (keep app.json)
2. Try to launch Hello World
3. Should show error: "app.bin not found"
4. Should NOT crash

---

## Step 7: Test Ping-Pong States

### 7.1 Check Current Partition
In serial monitor during boot:
```
esp_ota_get_running_partition() = ota_0 (or ota_1)
```

### 7.2 Test from ota_0
1. If running from ota_0: Launch Hello World → Return
2. Verify successful cycle

### 7.3 Force OTA Update to Swap Partitions
1. Perform a normal OTA update (or use debug tool)
2. This moves launcher to ota_1
3. Reboot and verify launcher now on ota_1

### 7.4 Test from ota_1
1. Launch Hello World from Apps menu
2. Verify it flashes to ota_0
3. Exit and return
4. Verify successful return to ota_1

**Critical**: Return must work regardless of which partition launcher is on.

---

## Expected Serial Output Summary

### Normal Operation
```
[    0] [   ] Starting CrossPoint version 0.14.0-dev
[AppsActivity] Found 1 apps
[AppsActivity] Launching app: Hello World
[AppLoader] Flashing to partition: ota_1 (offset: 0x650000)
[AppLoader] Battery: 85% - OK
[AppLoader] Flash complete. Rebooting...
[HelloWorld] Starting...
[HelloWorld] Activity started
[HelloWorld] Triggering return to launcher...
```

### Error Cases
```
[AppLoader] Battery: 15% - TOO LOW
[AppLoader] Aborting flash

[AppLoader] Magic byte check failed: expected 0xE9, got 0xXX
[AppLoader] Invalid firmware image

[AppsActivity] No apps found
```

---

## Troubleshooting

### Issue: Apps menu not showing
**Solution**: Verify `onGoToApps` callback passed to HomeActivity in main.cpp

### Issue: "No apps found" message
**Check**:
- SD card mounted properly
- `/.crosspoint/apps/` directory exists
- `app.json` is valid JSON
- File permissions (readable)

### Issue: Flash fails with "partition error"
**Check**:
- `esp_ota_get_next_update_partition()` returns correct slot
- Not trying to flash to currently running partition
- File size < partition size (6.4MB)

### Issue: Return to launcher fails
**Check**:
- Hello World calls `esp_ota_set_boot_partition()` before `esp_restart()`
- Bootloader not corrupted
- RTC memory accessible

### Issue: Boot loop after flashing bad app
**Recovery**:
1. Hold Power button for 10 seconds
2. Connect USB cable
3. Flash good firmware via `pio run -t upload`

---

## Success Criteria Checklist

- [ ] Main firmware builds and flashes successfully
- [ ] Hello World extension builds successfully
- [ ] SD card structure created correctly
- [ ] Apps menu appears in HomeActivity
- [ ] App list shows "Hello World"
- [ ] Flashing shows progress bar (0-100%)
- [ ] Serial output shows correct partition and battery info
- [ ] Device reboots into Hello World
- [ ] Back button returns to launcher
- [ ] Power button sleep/wake returns to launcher
- [ ] RTC watchdog works (returns after 3 failed boots)
- [ ] Low battery prevents flashing
- [ ] Missing app.bin shows error (no crash)
- [ ] Works from both ota_0 and ota_1 states
- [ ] OTA update after extension cycle works

---

## Post-Test Cleanup

1. Delete test apps from SD card if desired:
   ```
   /.crosspoint/apps/hello-world/
   ```

2. Revert to stock firmware if needed

3. Document any issues found

---

## Phase 2 (Future Work)

Once basic extension system is validated:
- [ ] Chess Puzzles as extension (extract from main firmware)
- [ ] WiFi download from URL
- [ ] App icons in manifest
- [ ] App store server
- [ ] Multiple apps in menu
- [ ] App deletion

---

**Test Date**: ___________
**Tester**: ___________
**Device ID**: ___________
**Firmware Version**: 0.14.0-dev
**Results**: ☐ PASS / ☐ FAIL
