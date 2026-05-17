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

## Proposed Protocol Shape

Create a Marginalia BLE Transfer GATT service with four characteristics:

- `control`: write with response; starts sessions, commits files, cancels transfers, requests file reads
- `data-in`: write without response; phone/computer sends upload chunks
- `data-out`: notify; reader sends download chunks or progress events
- `status`: read/notify; current state, progress, error code, expected next chunk

Upload flow:

1. User opens **File Transfer > Bluetooth Transfer**.
2. Device advertises `Marginalia Transfer` for a limited window and keeps auto-sleep disabled.
3. Client connects and sends `START_PUT` with destination class, filename, byte size, and SHA-256.
4. Firmware opens a `.part` file under a staging directory.
5. Client streams numbered chunks to `data-in`.
6. Firmware acknowledges progress through `status`.
7. Client sends `COMMIT`.
8. Firmware verifies byte count and SHA-256, renames into the final approved path, then offers install if it is an
   `.mpkg.zip`.

Download flow:

1. Client sends `START_GET` for an approved diagnostic path such as `/crash_report.txt`.
2. Firmware streams numbered chunks over `data-out`.
3. Client acknowledges completion or requests resume from a byte offset.

## Approved Destinations

Initial write support should be narrow:

- `/.marginalia/sideload/<safe-name>.mpkg.zip`
- `/Books/<safe-name>.epub`

Initial read support should also be narrow:

- `/crash_report.txt`
- optional package diagnostics under `/.marginalia/package-state/`

Do not provide arbitrary SD-card read/write over BLE. It creates security and corruption risk without much user value.

## Security And UX

BLE transfer mode should be explicit and temporary. The device should show a pairing code or confirmation prompt before
accepting writes. Encrypted/authenticated characteristics are preferred once pairing is reliable; until then, only allow
writes while the user is physically on the transfer screen.

The protocol should treat disconnects as normal. Incomplete uploads remain as `.part` files in staging and are removed
on the next transfer-mode start unless a resumable manifest says they can continue.

## Recommendation

Build SD-card package archive install first, because BLE uploads should land in the same `/.marginalia/sideload/`
directory and reuse the same installer.

The first firmware implementation should live under **File Transfer > Bluetooth Transfer** and should use
NimBLE-Arduino pinned to an upstream commit that embeds Apache NimBLE 1.9.0 or newer. Start with package upload only,
because `.mpkg.zip` archives are small and already have size/SHA verification in the package path. Add crash-report
download second. Add EPUB upload after the transfer protocol survives real phone testing.

## References

- ESP32-C3 BLE overview: Bluetooth LE 5.0/5.4 certification and ESP-NimBLE support.
  https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/api-guides/ble/overview.html
- ESP32-C3 datasheet: BLE PHY support, coded PHY, and LE Data Packet Length Extension.
  https://documentation.espressif.com/esp32-c3_datasheet_en.html
- ESP-IDF BLE data exchange docs: GATT services, reads, writes, notifications, and indications.
  https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32c3/api-guides/ble/get-started/ble-data-exchange.html
- NimBLE-Arduino docs via Context7: custom GATT services, writable characteristics, encrypted properties, notifications.
  https://github.com/h2zero/NimBLE-Arduino
