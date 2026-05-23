# CrossPoint BLE Transfer Design

## Goal

Add a Bluetooth transfer mode to CrossPoint Reader that works with the Xteink BLE companion and command-line tooling while keeping the firmware scope focused on reader-owned file transfer.

The feature should support:

- EPUB uploads to `/Books`
- BMP uploads to `/Pictures`
- firmware uploads staged on SD card and flashed through the existing firmware flasher
- crash-report download from `/crash_report.txt`
- first-use code authentication
- trusted browser/host authentication after the reader confirms saving the host
- a Bluetooth Transfer entry in the File Transfer menu
- a QR/link to `https://ble.xteink.lol/`

The feature must not add package uploads, plugin support, package-state diagnostics, arbitrary SD-card reads, or arbitrary SD-card writes over BLE.

## Protocol

Reuse the existing Xteink BLE transfer protocol version 1:

- GATT service UUID `6f9f0a00-9b1d-4d1f-9f53-5b6b8b3d0f10`
- `control` write characteristic
- `data-in` write/write-without-response characteristic
- `status` read/notify characteristic
- `data-out` notify characteristic

The firmware accepts `hello`, `save_host`, `start_put`, `commit`, `cancel`, `start_get`, and `get_ack`. Upload frames and download frames use the same little-endian sequence header followed by payload bytes.

Status JSON should include enough capability information for browser clients to hide unsupported features:

- `protocol_version: 1`
- `firmware_name: "CrossPoint Reader"`
- `upload_kinds: ["book", "bmp", "firmware"]`
- `download_kinds: ["crash_report"]`
- `firmware_ota_supported: true`
- `browser_companion_url: "https://ble.xteink.lol/"`

The device name should be CrossPoint-specific, but clients must discover the service by UUID.

## Security

BLE transfer is active only while the Bluetooth Transfer activity is open. First use requires the six-digit code shown on the device. Trusted auth uses host id plus HMAC-SHA256 over the device nonce. The firmware stores at most one trusted host under `/.crosspoint/ble_trusted_hosts.json` and rolls back in-memory trust if saving fails.

All destinations are explicit:

- books: basename-only `.epub` files under `/Books`
- pictures: basename-only `.bmp` files under `/Pictures`
- firmware: basename-only `.bin` file staged under a hidden CrossPoint BLE OTA directory
- downloads: `/crash_report.txt` only

Partial uploads are written to `.part` paths and removed on cancellation or non-resumable disconnect. Firmware uploads validate size and image integrity before flashing and require device-side confirmation before writing the OTA partition.

## User Flow

File Transfer gains a Bluetooth Transfer option. Opening it starts BLE advertising, shows the visible code, and displays the companion URL/QR. Back exits the activity and stops BLE.

The screen shows connection, receive, verify, save, send, firmware confirmation, firmware flashing, and error states. If a trusted host exists, the user can clear it from the idle Bluetooth screen.

## Host Tools

The CrossPoint CLI should support:

```sh
python3 scripts/ble_transfer.py put-book path/to/book.epub --code 123456
python3 scripts/ble_transfer.py put-bmp path/to/image.bmp --code 123456
python3 scripts/ble_transfer.py put-firmware path/to/firmware.bin --code 123456
python3 scripts/ble_transfer.py get-crash-report ./crash_report.txt --code 123456
```

The browser companion at `https://ble.xteink.lol/` should work for book, BMP, trusted-host auth, and crash-report download. Firmware remains CLI-first unless the companion exposes firmware upload after capability detection and reliability testing.

## Testing

Run:

```sh
python3 -m py_compile scripts/ble_transfer.py
git diff --check
./bin/clang-format-fix
pio run
pio check -e default
```

Hardware validation should cover code auth, trusted-host auth, EPUB upload, BMP upload, crash-report download, firmware staging/confirmation, and rejecting package/package-state operations.
