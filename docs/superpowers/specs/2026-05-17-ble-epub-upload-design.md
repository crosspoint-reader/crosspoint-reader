# BLE EPUB Upload Design

## Goal

Extend Bluetooth Transfer from package-only upload to normal book upload, starting with EPUB files.

The first follow-up PR should let a user:

- open **File Transfer > Bluetooth Transfer** on the reader
- run `python3 scripts/ble_transfer.py put-book path/to/book.epub --code 123456`
- transfer the EPUB over the existing BLE service
- have firmware verify byte count and SHA-256
- save the file under `/Books/`
- see a clear success or error state on both the device and host

This is still File Transfer behavior, not an Extension or package-manager feature.

## Scope

Implement EPUB upload only.

Do not include BMP upload, diagnostic download, or BLE OTA in this PR. Those should build on the same generalized upload path after EPUB works on real hardware.

## Protocol Changes

Reuse the existing BLE transfer service, characteristics, session-code handshake, chunk framing, and status notifications.

Add a second upload kind:

```json
{"op":"start_put","kind":"book","name":"Example.epub","size":1234,"sha256":"..."}
```

The existing binary chunk frame remains unchanged:

```text
uint32_le sequence
bytes payload
```

For reliability and speed, the host should default to a windowed `write without response` strategy instead of fully
unacknowledged streaming. The device publishes progress through `status`; the host sends a bounded byte window, waits
until device progress reaches that window, then continues. Keep acknowledged writes available as a conservative fallback
and reserve fully unacknowledged writes for debugging because real hardware can drop enough frames to trigger sequence
mismatch failures.

Add or reuse status states:

```json
{"state":"receiving","kind":"book","received":512,"size":1234}
{"state":"verifying","kind":"book","received":1234,"size":1234}
{"state":"saved","kind":"book","name":"Example.epub","path":"/Books/Example.epub"}
{"state":"error","error":"sha256_mismatch"}
```

Packages should keep their existing `installing` and `installed` states. Books should stop at `saved`; they do not use the package installer.

## Firmware Design

`BleTransferActivity` should keep one upload pipeline and vary only the validated destination and post-commit action by transfer kind.

For `kind: "package"`:

- keep the existing `.mpkg.zip` validation
- keep writing to `/.marginalia/sideload/`
- keep installing through the package archive/inbox path

For `kind: "book"`:

- require a safe basename
- require `.epub` extension, case-insensitive if local filename helpers already support that pattern
- reject hidden names, path separators, control characters, and unsafe punctuation
- bound filename length with the same 96-byte limit unless an existing file-browser limit is stricter
- write to `/Books/.ble-<safe-name>.part`
- verify byte count and SHA-256 on commit
- rename to `/Books/<safe-name>`
- report `saved` on success

The PR should create `/Books` if it does not already exist. If the target file already exists, fail with a clear `exists` error for the first version rather than silently replacing user content.

On cancel, disconnect, bad SHA, size mismatch, or activity exit, remove the `.part` file.

## Host Tool Design

Extend `scripts/ble_transfer.py` with:

```sh
python3 scripts/ble_transfer.py put-book path/to/book.epub --code 123456
```

Keep `put-package` unchanged for users and scripts.

Internally, share the transfer flow:

- scan/connect
- send `hello`
- send `start_put`
- stream sequence-numbered chunks
- send `commit`
- wait for final status

The shared helper should take transfer kind, required extension, success states, and final success message. This avoids duplicating the transport code for every file type.

## UX

The device screen should continue to show the current code, connection state, bytes received, and final state.

When uploading a book, the final device state should say that the book was saved, not installed. If the UI has room, include the basename.

The host tool should print:

```text
Saved /Books/Example.epub
```

or a clear failure such as:

```text
Transfer failed: exists
```

## Error Handling

Add or preserve these errors:

- invalid session code
- unsupported protocol version
- unsupported transfer kind
- unsafe filename
- unsupported extension
- file already exists
- SD directory creation failure
- open/write/rename failure
- client disconnect during transfer
- sequence mismatch
- size mismatch
- SHA-256 mismatch

The firmware should not leave `.part` files behind after failed book uploads.

## Testing

Local validation:

- `python3 -m py_compile scripts/ble_transfer.py`
- `git diff --check`
- firmware build for `default`
- firmware build for `slim`
- `pio check -e default`

Hardware validation:

- flash current firmware
- open **File Transfer > Bluetooth Transfer**
- run `put-book` with a known small EPUB and the displayed code
- verify host reaches `saved`
- verify device shows saved state
- verify the file exists under `/Books/`
- open the book from the file browser if practical
- retry the same filename and verify it fails with `exists`
- try a bad extension and verify it is rejected

## Later Work

BMP upload should be the next upload kind after EPUB. It should use the same shared transfer helper with a narrow approved destination.

Diagnostic download should add `data-out`, `start_get`, and host-side file writing after uploads are stable.

BLE OTA should wait. OTA needs stronger resumability and rollback UX than book/package upload because the firmware image is large and a failed update has higher support cost.
