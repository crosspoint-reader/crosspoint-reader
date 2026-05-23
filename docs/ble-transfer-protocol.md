# BLE Transfer Protocol

CrossPoint Reader exposes a small BLE transfer service while **File Transfer > Bluetooth Transfer** is open.

The browser companion is available at <https://ble.xteink.lol/>. Source and compatibility notes live at
<https://github.com/marginalia-os/ble-xteink>.

## Compatibility

- Protocol version: `1`
- Device name: `CrossPoint Transfer`
- Service UUID: `6f9f0a00-9b1d-4d1f-9f53-5b6b8b3d0f10`

Clients should discover the service by UUID. The user-visible name is not part of the compatibility contract.

## Characteristics

| Name | UUID | Direction | Properties |
| --- | --- | --- | --- |
| `control` | `6f9f0a01-9b1d-4d1f-9f53-5b6b8b3d0f10` | client to reader | write with response |
| `data-in` | `6f9f0a02-9b1d-4d1f-9f53-5b6b8b3d0f10` | client to reader | write, write without response |
| `status` | `6f9f0a03-9b1d-4d1f-9f53-5b6b8b3d0f10` | reader to client | read, notify |
| `data-out` | `6f9f0a04-9b1d-4d1f-9f53-5b6b8b3d0f10` | reader to client | notify |

## Authentication

First use requires the six-digit code shown on the reader:

```json
{"op":"hello","version":1,"code":"123456"}
```

After code authentication, a client can include `pair_host_id`, `pair_host_name`, and `pair_secret` in the `hello`
command. The reader prompts to save the trusted host after a successful authenticated upload. Trusted auth signs this
message:

```text
{device_nonce}|{host_id}|1
```

using HMAC-SHA256 with the saved secret.

## Supported Operations

Uploads use `start_put`, binary frames on `data-in`, then `commit`.

Supported upload kinds:

- `book`: `.epub` saved under `/Books`
- `bmp`: `.bmp` saved under `/Pictures`
- `firmware`: `.bin` staged on SD card, validated, confirmed on device, then flashed through the OTA path

Downloads use `start_get`, notifications on `data-out`, and one `get_ack` per frame.

Supported download kinds:

- `crash_report`: reads `/crash_report.txt`

CrossPoint does not support BLE package upload, package-state download, arbitrary path reads, or arbitrary path writes.

## Status Capabilities

Status JSON includes capability fields so clients can hide unsupported controls:

```json
{
  "protocol_version": 1,
  "firmware_name": "CrossPoint Reader",
  "upload_kinds": ["book", "bmp", "firmware"],
  "download_kinds": ["crash_report"],
  "firmware_ota_supported": true,
  "browser_companion_url": "https://ble.xteink.lol/"
}
```

Clients should still handle `state: "error"` for rejected operations.
