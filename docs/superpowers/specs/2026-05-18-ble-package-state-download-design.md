# BLE Package State Download Design

## Goal

Add a narrow read-only BLE download for package state JSON so a trusted host can inspect package diagnostics without
opening arbitrary SD-card access.

This extends **File Transfer > Bluetooth Transfer** and the existing `scripts/ble_transfer.py` host tool.

## Scope

The implementation supports one approved diagnostic object:

- `kind: "package_state"`
- required `package_id`
- SD-card path: `/.marginalia/package-state/<package-id>.json`
- host command: `python3 scripts/ble_transfer.py get-package-state <package-id> [output-path]`

The PR must not add directory listing, package browsing, arbitrary path reads, writes, deletion, or package archive
downloads. Missing state files return `not_found`.

## GATT Protocol

Reuse the existing authenticated `start_get` download path and ACK-based `data-out` stream:

```json
{"op":"start_get","kind":"package_state","package_id":"org.example.package"}
```

Firmware validates the session, validates the package id with the package-store package-id rules, maps it to
`/.marginalia/package-state/<package-id>.json`, and opens only that file.

Download frames keep the crash-report format:

```text
uint32_le sequence
bytes payload
```

The host sends one ACK per frame:

```json
{"op":"get_ack","sequence":0}
```

Final status uses the existing `sent` shape with `kind: "package_state"`, `package`, `name`, `sent`, and `size`.

## Security

Downloads require the same BLE session authentication as crash-report download:

- trusted-host HMAC when available
- visible six-digit code fallback
- no unauthenticated reads

The host command also validates package ids before sending the request. Firmware remains authoritative and rejects
invalid ids.

## Host Tool

Add:

```sh
python3 scripts/ble_transfer.py get-package-state org.example.package
python3 scripts/ble_transfer.py get-package-state org.example.package ./state.json
python3 scripts/ble_transfer.py get-package-state org.example.package --code 123456
```

Behavior:

- default output path is `./<package-id>.json`
- write to `<output>.part`
- fail if output exists unless `--force` is provided
- verify received byte count against final `sent` status
- reuse the same trusted-host auth and code fallback as `get-crash-report`

## Validation

Local:

- `python3 -m py_compile scripts/ble_transfer.py`
- `git diff --check`
- `./bin/clang-format-fix`
- `pio run`

Hardware:

- flash Xteink
- open **File Transfer > Bluetooth Transfer**
- download a known `/.marginalia/package-state/<package-id>.json`
- verify missing package state returns `not_found`
- verify invalid package ids are rejected
