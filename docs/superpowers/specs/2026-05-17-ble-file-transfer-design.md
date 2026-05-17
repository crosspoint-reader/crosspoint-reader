# BLE File Transfer Design

## Goal

Add Bluetooth LE package transfer as a first-class File Transfer mode. This is not an Extension entry point and does not belong in the package manager UI.

The first implementation should be fully usable for package installation:

- user opens **File Transfer > Bluetooth Transfer**
- device advertises a Marginalia BLE transfer service while that screen is open
- host sends a `.mpkg.zip` archive
- firmware writes it to `/.marginalia/sideload/`
- firmware verifies byte count and SHA-256
- firmware installs it through the same archive and inbox transaction path used by SD-card sideload

## Placement

The existing File Transfer entry is `CrossPointWebServerActivity`. It first opens `NetworkModeSelectionActivity`, which currently offers Wi-Fi join, Calibre Wireless, and hotspot modes. Add Bluetooth Transfer as the fourth network/file-transfer mode.

The BLE activity should return to the File Transfer mode picker on exit, matching the Calibre subactivity flow. Exiting the whole File Transfer stack should stop BLE advertising and release BLE resources.

## Firmware Components

Add a new `BleTransferActivity` under `src/activities/network/`.

The activity owns the BLE lifetime:

- initialize NimBLE on enter
- create a custom GATT service
- advertise a short device name, for example `Marginalia Transfer`
- display the session code, connection state, received bytes, install state, and errors
- disable auto-sleep while active
- stop advertising and deinitialize BLE on exit

The activity should not start Wi-Fi and should not trigger the web-server silent-restart path unless Wi-Fi was already active for another reason.

## GATT Protocol

Use NimBLE-Arduino from a commit that embeds Apache NimBLE 1.9.0 or newer, because earlier embedded NimBLE versions have
public Bluetooth security CVEs. The first protocol version uses three characteristics:

- `control`: write with response, UTF-8 JSON commands
- `data-in`: write without response, binary chunk frames
- `status`: read and notify, UTF-8 JSON status

Control commands:

```json
{"op":"hello","version":1,"code":"123456"}
{"op":"start_put","kind":"package","name":"example.mpkg.zip","size":1234,"sha256":"..."}
{"op":"commit"}
{"op":"cancel"}
```

Data frames use a simple binary header:

```text
uint32_le sequence
bytes payload
```

The firmware accepts monotonically increasing sequence numbers starting at zero. It writes payload bytes to a `.part` file and reports progress through `status` notifications.

Status payloads:

```json
{"state":"advertising","code":"123456"}
{"state":"receiving","received":512,"size":1234}
{"state":"verifying","received":1234,"size":1234}
{"state":"installing","package":"org.example.package"}
{"state":"installed","package":"org.example.package"}
{"state":"error","error":"sha256_mismatch"}
```

## File Safety

The first PR only accepts package archives:

- filename must be a basename with no slash or backslash
- filename must not start with `.`
- filename may contain only ASCII letters, digits, `.`, `_`, and `-`
- filename must end with `.mpkg.zip`
- maximum filename length is 96 bytes
- maximum upload size should be bounded conservatively for available SD-card and RAM behavior

The temporary path is `/.marginalia/sideload/.ble-<safe-name>.part`. On successful verification, rename to `/.marginalia/sideload/<safe-name>`.

If install succeeds, the archive may remain available for manual reinstall. If install fails, keep the verified archive in sideload and show the install error.

## Security Model

BLE transfer is explicit and temporary. The device only advertises while the Bluetooth Transfer screen is open.

The screen shows a six-digit session code. The client must send that code in `hello` before transfer begins. This is not a replacement for BLE pairing security, but it prevents accidental writes from nearby generic BLE clients and keeps the first PR simple.

Future hardening can add NimBLE bonding, MITM passkey confirmation, and phone UI pairing. The protocol shape should not depend on those later additions.

## Host Tool

Add a small host utility:

```sh
python3 scripts/ble_transfer.py put-package path/to/package.mpkg.zip --code 123456
```

The tool should:

- use Python `bleak`
- scan for the Marginalia BLE service
- connect to the device
- send `hello`
- send `start_put`
- stream chunk frames to `data-in`
- send `commit`
- print progress and final install status

Use a conservative default chunk payload size so macOS, Linux, and common BLE adapters work. Expose `--chunk-size` for debugging.

## Error Handling

Firmware should surface these errors on-device and through `status`:

- invalid session code
- unsupported protocol version
- unsupported transfer kind
- unsafe filename
- open/write/rename failure
- size mismatch
- SHA-256 mismatch
- archive inspection failure
- archive extraction failure
- package install failure
- client disconnect during transfer

On cancel, disconnect, or activity exit, close the current file and remove any `.part` file.

## Testing

Local validation:

- build firmware with NimBLE dependency enabled
- verify flash size remains within partition limits
- run existing native/unit test scripts that do not require hardware
- syntax-check the Python BLE tool

Hardware validation:

- flash Xteink
- open File Transfer > Bluetooth Transfer
- upload a known `.mpkg.zip` with the host tool
- verify on-device progress
- verify installed package appears in Extensions
- verify bad SHA fails and leaves no `.part` file
- verify Back exits BLE and returns to File Transfer mode selection

## Out Of Scope

The first PR does not implement arbitrary SD-card browsing, EPUB upload, crash-report download, USB serial transfer, USB mass storage, phone-native UI, or permanent background BLE advertising.
