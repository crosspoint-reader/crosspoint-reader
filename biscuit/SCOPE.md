# Project Vision & Scope: Biscuit

Biscuit turns a $40 e-ink reader into a general-purpose smart device. The Xteink X4 is not just for reading — it's a pocket tool for wireless diagnostics, personal security, private communication, and everyday utilities.

Forked from [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader). All core reading functionality comes from upstream. Biscuit builds everything else on top.

## 1. Core Mission

To maximize the potential of the ESP32-C3 + e-ink hardware as a versatile, offline-first, privacy-respecting device that fits in your pocket and runs for days on a single charge.

Reading is one of eight equal categories, not the sole focus.

## 2. Design Principles

- **Offline-first.** Every feature must work without internet access. WiFi and BLE are tools, not dependencies.
- **Privacy by default.** No telemetry, no accounts, no cloud. Data lives on the SD card and nowhere else.
- **Memory-conscious.** 380KB SRAM, no PSRAM. Every allocation counts. Prefer stack over heap, fixed buffers over dynamic ones, streaming over buffering.
- **Radio-aware.** WiFi and BLE share one radio. Features must use RadioManager and yield the radio when done.
- **Battery-respectful.** E-ink holds its image without power. Features should minimize radio-on time and avoid polling loops.
- **Seven-button UX.** No touchscreen. Every interaction must be navigable with directional buttons, confirm, back, and menu.

## 3. Scope

### In-Scope

**Reading & library management** — EPUB rendering, library browsing, OPDS, Calibre sync. Inherited from CrossPoint.

**Network diagnostics** — WiFi analysis, host scanning, ping, DNS lookup, mDNS discovery, HTTP client. Tools for understanding and troubleshooting local networks.

**Wireless reconnaissance** — BLE scanning, packet monitoring, probe sniffing, wardriving, device fingerprinting, vendor lookup, AP history, network change detection, perimeter watch. Passive observation of the wireless environment.

**Personal security** — Tracker detection, counter-surveillance sweeps, rogue AP detection, deauth monitoring, emergency beacon, quick wipe, PIN with duress mode, RF silence, screen decoy, MAC randomization, SD encryption, phone tether alerts.

**Private communication** — ESP-NOW mesh chat with relay mode, SSID-based covert channels, BLE contact exchange, dead drops, local bulletin boards. No servers, no accounts.

**Utilities & productivity** — TOTP authenticator, QR TOTP display, password manager, medical card, clock, calculator, QR generator, morse code, cipher tools, OTP generator, event logger, flashcards, habit tracker, breadcrumb trails, vehicle finder, transit alerts, signal triangulation, WiFi heat mapping, steganography, etch-a-sketch.

**Games** — Lightweight games that work well with e-ink refresh rates and button input.

**System management** — Settings, file transfer, task manager, battery monitor, device info, automation (geofence triggers + scheduled tasks), reading stats.

### Out-of-Scope

- **Always-on connectivity.** No RSS readers, news aggregators, or web browsers. Background WiFi drains the battery and complicates the single-core CPU.
- **Media playback.** No audio, no video. The hardware has no speaker or audio codec.
- **Touchscreen interaction.** The X4 has no touch digitizer. Don't design for tap/swipe.
- **Cloud services.** No feature should require an account, API key, or internet connection to function.
- **Complex text input.** The device has 7 buttons. Features requiring paragraphs of typed input are better suited for devices with keyboards. Short inputs (PINs, SSIDs, short messages) are fine via the existing character selector.

### In-Scope — Hardware Limited

- **Clock accuracy.** The ESP32-C3 RTC drifts during deep sleep. NTP sync helps but requires WiFi. Clock-dependent features (TOTP) should warn when time may be stale.
- **PDF rendering.** Fixed-layout documents require pan/zoom on a small e-ink screen. Supported but not a priority.
- **Color or grayscale.** The display is 1-bit monochrome. All UI must be black and white.

## 4. Feature Evaluation

Before adding a new feature, ask:

1. **Does it work offline?** If it requires internet to function, it doesn't belong.
2. **Does it fit in memory?** If it needs large buffers or dynamic allocation beyond what the device can handle, rethink the approach.
3. **Can it be operated with 7 buttons?** If the UX requires a keyboard or touchscreen, it doesn't fit this hardware.
4. **Does it respect the radio?** If it holds WiFi or BLE indefinitely, it blocks other features. Use RadioManager, do your work, release.
5. **Does it store data responsibly?** All persistent data goes under `/biscuit/` on SD. No writing to flash. Encrypt sensitive data.

> **Note to Contributors:** If you are unsure whether your idea fits the scope, open a Discussion before you start coding. The bar is "useful on a 7-button e-ink device with WiFi and BLE" — which is broader than you might think.
