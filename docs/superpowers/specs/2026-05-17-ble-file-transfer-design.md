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

The initial authentication flow is code-based: the host sends `hello` with the visible six-digit code, and the device
accepts the session only if the code matches the current Bluetooth Transfer screen.

The paired-host flow should extend `hello` without changing the transfer commands:

```json
{"op":"hello","version":1,"host_id":"host-uuid","response":"hex"}
```

The device generates a single-use nonce when advertising starts and includes it in the status payload for trusted-host
clients. The host computes `HMAC-SHA256(device_nonce + "|" + host_id + "|1")` with the shared secret from its
trusted-host record and sends the hex result as `response`. The device verifies the HMAC, rotates the nonce after a
successful match, and accepts the session. Failed nonce authentication should leave the session unauthenticated, log the
failure, and fall back to the visible-code `hello` path.

Data frames use a simple binary header:

```text
uint32_le sequence
bytes payload
```

The firmware accepts monotonically increasing sequence numbers starting at zero. It writes payload bytes to a `.part` file and reports progress through `status` notifications.

Status payloads:

```json
{"state":"advertising","device_id":"...","device_nonce":"...","has_trusted_host":true}
{"state":"receiving","received":512,"size":1234}
{"state":"verifying","received":1234,"size":1234}
{"state":"installing","package":"org.example.package"}
{"state":"installed","package":"org.example.package","paired":true}
{"state":"error","error":"sha256_mismatch"}
```

## File Safety

BLE writes only accept the explicit file kinds implemented by the protocol. Package uploads go to the package inbox and
book uploads go to `/Books`:

- filename must be a basename with no slash or backslash
- filename must not start with `.`
- filename may contain only ASCII letters, digits, `.`, `_`, and `-`
- package filenames must end with `.mpkg.zip`
- book filenames must end with `.epub`
- maximum filename length is 96 bytes
- maximum upload size should be bounded conservatively for available SD-card and RAM behavior

Package uploads use `/.marginalia/sideload/.ble-<safe-name>.part` while receiving. On successful verification, rename
to `/.marginalia/sideload/<safe-name>` and install from the package inbox path.

Book uploads use `/Books/.ble-<safe-name>.part` while receiving. On successful verification, rename to
`/Books/<safe-name>`.

If install succeeds, the archive may remain available for manual reinstall. If install fails, keep the verified archive in sideload and show the install error.

## Security Model

BLE transfer is explicit and temporary. The device only advertises while the Bluetooth Transfer screen is open.

The screen shows a six-digit session code. The client must send that code in `hello` before transfer begins. This is not a replacement for BLE pairing security, but it prevents accidental writes from nearby generic BLE clients and keeps the first PR simple.

The first hardening step should mirror the existing Wi-Fi credential UX instead of forcing users to type a code every time:

- first connection uses the visible six-digit code
- after the first successful authenticated commit, the device can prompt **Save this host?**
- if accepted, firmware stores a trusted-host record with a host id, display name, and shared secret
- the host stores the matching device id, host id, and shared secret in its local Marginalia config
- later transfers use a nonce-based challenge response in `hello` and do not require a visible code
- the Bluetooth Transfer screen shows when a trusted host is connected
- the screen exposes a **Forget host** action using the same cancel/forget prompt shape as saved Wi-Fi networks
- the six-digit code remains available for new hosts, deleted host config, and recovery tooling

Prompting after commit avoids saving a host that merely guessed or entered the code but never completed a useful file
operation. For package uploads, the prompt should appear after the archive is accepted into sideload and the install
transaction reaches a final state; for book uploads, after the file is verified and saved.

Store trusted hosts with a small `BleTrustedHostStore` modeled on `WifiCredentialStore`: JSON on SD card, hardware-tied obfuscation for the shared secret, an explicit maximum host count, and remove/clear helpers for UI actions. Start with one trusted host unless the host-management UI is expanded to list multiple names.

Avoid OS BLE bonding in the first pairing PR. NimBLE bonding and encrypted characteristics can be layered on later, but a Marginalia application-level trust record is easier to test with Bleak across macOS, Linux, and phones and matches the existing Wi-Fi save/forget product behavior.

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

Trusted-host validation for the pairing follow-up:

- first connect with the visible code, complete one authenticated transfer, and verify the **Save this host?** prompt
- accept the prompt and verify the next connection uses nonce-based `hello` authentication without requiring the code
- decline the prompt and verify the next connection still requires the visible code
- use **Forget host** on the Bluetooth Transfer screen and verify the trusted host can no longer authenticate by nonce
- verify a bad nonce/HMAC falls back to code-based authentication and does not accept writes before a valid `hello`

## Out Of Scope

The first PR does not implement arbitrary SD-card browsing, EPUB upload, crash-report download, USB serial transfer, USB mass storage, phone-native UI, or permanent background BLE advertising.
