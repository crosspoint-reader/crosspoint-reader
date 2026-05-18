# Bluetooth File Transfer

## Idea

Add a Bluetooth LE transfer mode for sending books, package archives, and diagnostic files between a phone or computer
and the reader without joining Wi-Fi or removing the SD card.

This should not try to expose the SD card as a mounted filesystem. It should be an application-level transfer service
that writes only to approved SD-card destinations and then hands package archives to the normal Marginalia installer.

## Why It Matters

BLE is more pleasant than USB serial for normal users:

- phones can connect without cables
- transfer can be initiated from a companion app or web app
- the reader can show an explicit pairing/transfer screen
- uploads can land directly in known places such as `/Books/` or `/.marginalia/sideload/`
- crash reports can be pulled after a failure without Wi-Fi setup

It is not a replacement for Wi-Fi for large libraries. It is a low-friction path for a few books, package archives, and
diagnostics.

## Hardware And Stack Findings

ESP32-C3 supports Bluetooth LE and Espressif documents both Bluedroid and ESP-NimBLE support on the chip. ESP-NimBLE
requires less heap and flash than Bluedroid, which matters on this firmware because Wi-Fi/TLS and reader code already
run close to memory limits.

The ESP32-C3 Bluetooth LE radio supports 1 Mbps PHY, 2 Mbps PHY, coded PHY for longer range, and LE Data Packet Length
Extension. Espressif's BLE data-exchange docs model the right shape for this feature: a custom GATT service with
read/write characteristics and server notifications or indications.

Context7 lookup also points to NimBLE-Arduino as the practical Arduino-facing stack. Its GATT server API supports custom
services, writable characteristics, encrypted writes, and notifications. That is enough for a framed transfer protocol.

## Current State

The first implementation is now split across focused PRs and uses a custom Marginalia BLE Transfer GATT service under
**File Transfer > Bluetooth Transfer**.

Implemented pieces:

- `control`: write with response; starts sessions, commits uploads, cancels transfers, requests approved downloads
- `data-in`: write/write-without-response; host sends upload chunks
- `data-out`: notify; reader sends download chunks
- `status`: read/notify; current state, progress, errors, session nonce, and trusted-host metadata
- visible six-digit code authentication for first use and recovery
- trusted-host authentication with a nonce-based HMAC challenge response
- **Save Host?** after successful authenticated uploads
- **Forget Host** from the Bluetooth Transfer screen
- host-side trusted-device config in `~/.config/marginalia/ble_hosts.json` with `0600` permissions
- firmware trusted-host storage on SD card with hardware-tied secret obfuscation
- `.mpkg.zip` package upload and install with `scripts/ble_transfer.py put-package`
- `.epub` book upload to `/Books/` with `scripts/ble_transfer.py put-book`
- `/crash_report.txt` download with `scripts/ble_transfer.py get-crash-report`

Download notifications originally arrived out of order on hardware, so crash-report download now uses stop-and-go ACKs:
the device sends one numbered `data-out` frame, the host validates it, then the host sends `get_ack` before the next
frame is emitted. This is slower than raw notification streaming but avoids dropped or reordered BLE notifications.

## Protocol Shape

The Marginalia BLE Transfer GATT service has four characteristics:

- `control`: write with response; starts sessions, commits files, cancels transfers, requests file reads
- `data-in`: write without response; phone/computer sends upload chunks
- `data-out`: notify; reader sends download chunks or progress events
- `status`: read/notify; current state, progress, error code, expected next chunk

Upload flow:

1. User opens **File Transfer > Bluetooth Transfer**.
2. Device advertises `Marginalia Transfer` for a limited window and keeps auto-sleep disabled.
3. Client connects and sends `start_put` with destination class, filename, byte size, and SHA-256.
4. Firmware opens a `.part` file under a staging directory.
5. Client streams numbered chunks to `data-in`.
6. Firmware acknowledges progress through `status`.
7. Client sends `commit`.
8. Firmware verifies byte count and SHA-256, renames into the final approved path, then offers install if it is an
   `.mpkg.zip`.

Download flow:

1. Client sends `start_get` for an approved diagnostic kind such as `crash_report`.
2. Firmware sends one numbered chunk over `data-out`.
3. Client validates the sequence and sends `get_ack`.
4. Firmware sends the next chunk, repeating until complete.
5. Firmware publishes final `sent` status.

Resume from byte offsets is not implemented yet.

## Approved Destinations

Current write support is narrow:

- `/.marginalia/sideload/<safe-name>.mpkg.zip`
- `/Books/<safe-name>.epub`

Current read support is narrow:

- `/crash_report.txt`

Potential next read support:

- package diagnostics under `/.marginalia/package-state/`

Do not provide arbitrary SD-card read/write over BLE. It creates security and corruption risk without much user value.

## Security And UX

BLE transfer mode should be explicit and temporary. The device should only advertise while the user is physically on the
Bluetooth Transfer screen.

Pairing should follow the Wi-Fi credential model already used by the firmware:

- first use requires the visible six-digit transfer code
- after a successful authenticated transfer, prompt **Save this host?**
- save a trusted-host record with a host id, display name, and shared secret
- store the shared secret with the same hardware-tied obfuscation pattern used for saved Wi-Fi passwords
- let later transfers authenticate with a nonce-based challenge response instead of asking for the code again
- show the trusted host name while connected
- provide **Forget host** with the same cancel/forget prompt shape as saved Wi-Fi networks
- keep the visible code fallback for new hosts, deleted host config, and recovery tooling

Use application-level trust before OS BLE bonding. NimBLE bonding and encrypted characteristics can be added later, but
the first paired-host flow should be testable through Bleak on macOS, Linux, and phones without relying on platform-
specific bonding behavior.

The protocol treats disconnects as normal. Incomplete uploads remain as `.part` files in staging and are removed on the
next transfer-mode start. A resumable manifest is still future work.

## Remaining Work

Useful follow-up PRs, in recommended order:

1. Package diagnostics download.
   Add one or more read-only `start_get` kinds for narrowly approved package diagnostics under
   `/.marginalia/package-state/`. Keep this read-only and avoid arbitrary path reads.
2. BMP upload.
   Add a `put-bmp` style upload kind only if there is a clear destination and UI path for viewing imported images. Reuse
   the existing upload helper and keep extension/path validation strict.
3. Phone or web companion UI.
   Build a small user-facing client once the Python CLI protocol has stabilized. The UI should use the same code/trusted
   host model rather than introducing a second pairing concept.
4. Resumable transfers.
   Add transfer manifests and byte-offset resume only if large files or phone clients make interruption recovery
   necessary. This should cover both uploads and downloads.
5. BLE OTA.
   Defer until resumability, rollback UX, and stronger authenticity checks are in place. Firmware images are larger and
   failed updates have higher support cost than book/package transfers.
6. OS BLE bonding or encrypted characteristics.
   Application-level trust is enough for the current CLI and is easier to test across macOS, Linux, and phones. Bonding
   can be layered on later if the phone UI needs platform-native trust.

Still intentionally out of scope:

- arbitrary SD-card browsing or writes over BLE
- permanent background BLE advertising
- exposing the SD card as a mounted filesystem over BLE

## References

- ESP32-C3 BLE overview: Bluetooth LE 5.0/5.4 certification and ESP-NimBLE support.
  https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/api-guides/ble/overview.html
- ESP32-C3 datasheet: BLE PHY support, coded PHY, and LE Data Packet Length Extension.
  https://documentation.espressif.com/esp32-c3_datasheet_en.html
- ESP-IDF BLE data exchange docs: GATT services, reads, writes, notifications, and indications.
  https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32c3/api-guides/ble/get-started/ble-data-exchange.html
- NimBLE-Arduino docs via Context7: custom GATT services, writable characteristics, encrypted properties, notifications.
  https://github.com/h2zero/NimBLE-Arduino
