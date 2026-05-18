# BLE Crash Report Download Design

## Goal

Add a read-only BLE download path for `/crash_report.txt` so a trusted host can pull diagnostics from the reader without
Wi-Fi, USB SD-card mounting, or removing the SD card.

This is a File Transfer feature, not an Extension feature. It extends the existing **File Transfer > Bluetooth Transfer**
mode and the `scripts/ble_transfer.py` host tool.

## Scope

The first implementation supports one approved diagnostic object:

- `kind: "crash_report"`
- SD-card path: `/crash_report.txt`
- host command: `python3 scripts/ble_transfer.py get-crash-report [output-path]`

The PR must not expose arbitrary SD-card reads, package-state browsing, directory listing, deletion, or writes beyond the
existing package/book upload flows.

## GATT Protocol

Keep the existing custom BLE service and characteristics, and add one new characteristic:

- `data-out`: notify; firmware sends binary download frames to the host

Existing characteristics remain unchanged:

- `control`: write with response; JSON commands
- `data-in`: write/write-without-response; upload chunks
- `status`: read/notify; JSON state and progress

Add this control command after a successful `hello`:

```json
{"op":"start_get","kind":"crash_report"}
```

Firmware validates the session, opens `/crash_report.txt`, publishes a `sending` status with total size, then sends one
numbered frame through `data-out`:

```text
uint32_le sequence
bytes payload
```

The host acknowledges each received frame before firmware sends the next one:

```json
{"op":"get_ack","sequence":0}
```

When the file is fully sent and the final frame has been acknowledged, firmware publishes:

```json
{"state":"sent","kind":"crash_report","name":"crash_report.txt","sent":1234,"size":1234}
```

If the file is missing, firmware publishes an error with `error: "not_found"`. Other SD-card failures use existing clear
error style, for example `could not open crash report`.

## Transfer Model

Crash reports are small enough that the first implementation can use a conservative stop-and-go stream:

1. Host subscribes to `status` and `data-out`.
2. Host sends authenticated `hello` using trusted-host auth when available, or the visible code fallback.
3. Host sends `start_get`.
4. Firmware reads one bounded chunk and notifies `data-out`.
5. Host validates the sequence and sends `get_ack`.
6. Firmware sends the next chunk after the ack, repeating until EOF.
7. Host checks chunk sequence, writes bytes to a local `.part` file, and atomically renames it when `sent` arrives.

Use a default payload of 160 bytes to match upload compatibility. The CLI can expose `--chunk-size` for debugging, but
firmware should clamp to a safe maximum.

The first PR does not implement resume. If sequence validation fails on the host, the host should cancel and delete its
local `.part` file. Stop-and-go ACK is intentionally slower than unacknowledged notifications but avoids dropped BLE
notifications corrupting diagnostics.

## Security

Downloads require the same authenticated BLE session as uploads:

- trusted-host HMAC when available
- visible six-digit code fallback
- no unauthenticated reads

No Save Host prompt appears after downloads. Saving trust remains tied to successful authenticated uploads so a host that
only pulled diagnostics does not automatically become trusted.

## UI

The existing Bluetooth Transfer screen can reuse the same central status area:

- `sending`: show diagnostic transfer progress
- `sent`: show `crash_report.txt`
- `error`: show existing error text

No extra menu item is needed in the first PR because the host initiates the diagnostic pull while the Bluetooth Transfer
screen is open.

## Host Tool

Add:

```sh
python3 scripts/ble_transfer.py get-crash-report
python3 scripts/ble_transfer.py get-crash-report ./crash_report.txt
python3 scripts/ble_transfer.py get-crash-report --code 123456
```

Behavior:

- authenticate exactly like `put-book` and `put-package`
- default output path is `./crash_report.txt`
- write to `<output>.part`
- fail if the output already exists unless `--force` is provided
- verify received byte count against final `sent` status
- print progress and final path

## Validation

Local:

- `python3 -m py_compile scripts/ble_transfer.py`
- `git diff --check`
- `pio run -e default`
- `pio check -e default`

Hardware:

- flash Xteink
- open **File Transfer > Bluetooth Transfer**
- use trusted-host auth to download `/crash_report.txt` without a code
- verify the downloaded file matches the SD-card crash report content
- verify missing `/crash_report.txt` returns `not_found`
- verify bad/no auth cannot download
