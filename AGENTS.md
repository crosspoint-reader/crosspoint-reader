# AGENTS.md

Guidance for agents working in `marginalia-firmware`.

## Project Role

This is the ESP32-C3 firmware for Xteink X3/X4 e-paper readers. It is the core Marginalia product and owns reader UI,
storage, networking, BLE transfer, package installation, extension settings, and future runtime hosts.

The ecosystem is intentionally split:

- `marginalia-sdk`: package manifest schemas and author tooling.
- `marginalia-registry`: reviewed metadata, artifact URLs, hashes, channels.
- `marginalia-hub`: catalog UI/API and publishing surface.
- `marginalia-examples`: reference packages.
- `marginalia-simulator`: host-side PlatformIO simulator dependency.

Do not collapse these responsibilities into firmware.

## Current BLE Context

Recent BLE work is split across PRs:

- PR #19: trusted-host pairing, `Save Host?`, `Forget Host`, no-code auth.
- PR #20: crash-report download via `start_get`, `data-out`, and stop-and-go `get_ack`.
- PR #21: package-state diagnostic download via `get-package-state`.

Current BLE protocol principles:

- BLE transfer is only active on **File Transfer > Bluetooth Transfer**.
- First use uses the visible six-digit code.
- Trusted auth uses host id plus HMAC over the device nonce.
- Downloads require authentication and use `data-out` notifications with per-frame `get_ack`.
- Reads are explicit allowlist only: `/crash_report.txt` and `/.marginalia/package-state/<safe-package-id>.json`.
- Do not add arbitrary SD-card browsing or path-based reads over BLE.
- Pairing remains tied to successful authenticated uploads, not downloads.

Host CLI:

```sh
python3 scripts/ble_transfer.py put-package path/to/package.mpkg.zip --code 123456
python3 scripts/ble_transfer.py put-book path/to/book.epub --code 123456
python3 scripts/ble_transfer.py get-crash-report ./crash_report.txt --code 123456
python3 scripts/ble_transfer.py get-package-state org.example.package ./state.json --code 123456
```

The local host may not have `~/.config/marginalia/ble_hosts.json`; if missing, use `--code`.

## Development Commands

Use these checks before pushing firmware changes:

```sh
python3 -m py_compile scripts/ble_transfer.py
git diff --check
./bin/clang-format-fix
pio run
pio check -e default
```

Flash the attached Xteink on macOS with the detected `/dev/cu.usbmodem*` port:

```sh
ls /dev/cu.usbmodem*
pio run -t upload --upload-port /dev/cu.usbmodem101
```

Do not assume the port is stable. It has appeared as both `/dev/cu.usbmodem1101` and `/dev/cu.usbmodem101`.

## Code Guidelines

- Keep ESP32-C3 RAM and flash pressure in mind; avoid large buffers and long-lived JSON documents.
- Keep file transfers explicit and destination-specific.
- Write partial uploads to `.part` paths and clean them on cancellation/disconnect.
- For constraints and security rules, validate on firmware even if the CLI also validates.
- Use existing `PackageStore` helpers and constants for package ids, state paths, installs, and compatibility.
- Use `RenderLock` around operations that can block while the display/UI state must remain consistent.
- Keep simulator compatibility in mind when adding HAL calls or hardware APIs; update `marginalia-simulator` stubs if needed.
- Do not expose the six-digit BLE code in status JSON.
- Do not store host secrets without preserving `0600` host config behavior and firmware rollback-on-save-failure semantics.

## Documentation

Update docs with behavior changes:

- User-facing transfer docs: `docs/webserver.md`
- BLE roadmap/state: `docs/future-ideas/bluetooth-file-transfer.md`
- Package architecture: `docs/marginalia/architecture.md`
- Design specs: `docs/superpowers/specs/YYYY-MM-DD-<topic>-design.md`

Keep future-ideas docs honest: move shipped work out of "Remaining Work".

