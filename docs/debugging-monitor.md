# Debugging monitor server

The enhanced monitor can share one USB serial connection between an interactive
monitor, memory graphs, and automation scripts. The monitor owns the serial port;
automation connects to its localhost HTTP API. Do not run a second serial monitor
against the same device.

For CLI commands, buttons, verified activity names, command files, and test
fixtures, see [Device control and test automation](device-control.md).

## Start the monitor

Headless mode requires `pyserial`. Graph mode also requires `matplotlib`;
`colorama` is optional. Run these commands from the repository root:

```sh
# Interactive terminal and memory graph
python3 scripts/debugging_monitor.py

# Same monitor, with the local API enabled
python3 scripts/debugging_monitor.py --serve

# API and terminal logs, without a graph or stdin prompt
python3 scripts/debugging_monitor.py --serve --headless
```

Supply the serial port as a positional argument when multiple devices are
connected. `--baud` defaults to 115200. `--http-port` defaults to 8765; use 0 to
choose an available port. The startup message prints the actual API address.
The server binds only to `127.0.0.1` and does not support browser-origin requests.

The graph, terminal, and API share one serial reader and command queue. Closing
the graph or pressing Ctrl-C stops the entire monitor, including the server.
Headless mode neither imports Matplotlib nor reads interactive stdin.

Each run creates a separate directory under `debugging-monitor-output/` (ignored
by Git). Override the parent with `--output-dir`. It contains:

- `serial.bin`: the exact received bytes, including screenshot payloads.
- `events.jsonl`: parsed events with host timestamps and increasing sequence numbers.
  Log events preserve the original device timestamps and text.
- Screenshot `.raw` files and orientation-correct `.pbm` images.

`--filter` and `--suppress` affect terminal output only. They do not filter saved
logs, API events, or graph data. Logs grow on disk for the duration of the run;
the in-memory event history is bounded to 4096 events.

## API

All POST requests require a JSON object and `Content-Type: application/json`.
Errors return `{"error": "..."}` with an HTTP error status. A command response
with `sent: true` means the host wrote the command, **not** that firmware executed
it or completed rendering. Firmware built with `ENABLE_SERIAL_LOG` also supports
the [UI-control protocol](device-control.md#ui-control). Firmware replies appear
as `control` events as well as unchanged `log` events.

| Method | Path | Behavior |
|---|---|---|
| GET | `/status` | Connection state, event cursor, output directory, control-lease status |
| GET | `/events?after=0&wait=0` | Events newer than `after`; optionally wait up to 30 seconds |
| POST | `/command` | Send `{"command":"SCREENSHOT"}`; `CMD:` prefix is optional |
| POST | `/screenshot` | Send the screenshot command; body `{}` |
| GET | `/screenshots/<filename>` | Download a captured `screenshot-N.raw` or `.pbm` |
| POST | `/control/claim` | Acquire control, or renew with the current token; body `{"ttl":60}` |
| POST | `/control/release` | Release control; body `{}` and current token required |
| POST | `/serial/release` | Close serial and disable automatic reconnect; body `{}` |
| POST | `/serial/reconnect` | Resume connection attempts to the configured port; body `{}` |

Example read-only requests:

```sh
curl http://127.0.0.1:8765/status
curl 'http://127.0.0.1:8765/events?after=0&wait=5'
```

Event responses include `events`, `cursor`, and `dropped`. Pass the returned
`cursor` as the next `after`. Every client has an independent cursor, so one
reader does not consume another reader's events. `dropped: true` means the client
fell behind the in-memory history; the missing events remain in `events.jsonl`.
Start a new cursor when the monitor process restarts.

Command responses include `command_id`, `sent`, and `after`, the event cursor
captured before writing. Read events after that cursor to include immediate
responses. `command_sent` is a host event; ordinary firmware log lines do not have
command IDs. UI-control replies carry their own request IDs. Screenshots retain
the single-capture framing protocol. Do not infer completion from a successful
HTTP response.

## Exclusive benchmark control

Any client may issue commands when control is unclaimed. A benchmark should first
claim control. The response contains an opaque `token`; send it in the
`X-Control-Token` header on subsequent writes. Other clients and interactive stdin
receive HTTP 409 or a rejection message while the lease is active. Reading logs,
viewing graphs, and downloading existing screenshots remain available.

Leases default to 60 seconds and allow 1–300 seconds. Renew before expiry by
posting to `/control/claim` with the current token. A dead client therefore cannot
lock the monitor indefinitely. This controls monitor commands only; physical
button presses on the device still work.

## Capture through the API

This Python example uses only the standard library:

```python
import json
from urllib.request import Request, urlopen

base = "http://127.0.0.1:8765"

def post(path, body, token=None):
    headers = {"Content-Type": "application/json"}
    if token:
        headers["X-Control-Token"] = token
    request = Request(base + path, json.dumps(body).encode(), headers=headers)
    with urlopen(request, timeout=5) as response:
        return json.load(response)

token = post("/control/claim", {"ttl": 60})["token"]
try:
    sent = post("/screenshot", {}, token)
    cursor = sent["after"]
    # A missing response or expired lease should fail the run, not hang it.
    for _ in range(4):
        with urlopen(f"{base}/events?after={cursor}&wait=5", timeout=7) as response:
            batch = json.load(response)
        if batch["dropped"]:
            raise RuntimeError("Event history gap")
        cursor = batch["cursor"]
        capture = next((e for e in batch["events"]
                        if e["type"] in ("screenshot", "screenshot_error")
                        and e.get("command_id") == sent["command_id"]), None)
        if capture:
            if capture["type"] == "screenshot_error":
                raise RuntimeError(capture["error"])
            print(capture.get("image_path", capture["raw_path"]))
            break
    else:
        raise TimeoutError("No screenshot result")
finally:
    post("/control/release", {}, token)
```

## Flashing and reconnecting

Keep the benchmark's control lease and call `/serial/release` before starting
PlatformIO or another flasher. Its successful response means the port handle has
closed. The API and graph remain running, and the monitor will not compete with
the flasher by reconnecting automatically.

After the flasher exits, call `/serial/reconnect` and wait for `/status` to report
`connected` (or the equivalent connection event). Renew the lease if flashing
takes longer than its TTL. On an ordinary unplug, the monitor retries once per
second without needing a reconnect request. It reconnects to the same configured
path; restart with a new path if the OS assigns a different device name.

Opening or reopening serial can reboot the device even though the monitor sets
DTR and RTS before opening. This occurred on an X3 connected to macOS, with
`USB_UART_CHIP_RESET` in the boot log. `connected` confirms an open host port,
not firmware readiness. Wait for boot and rendering to settle, then restore the
intended book/page before starting a trial. Keep the connection open across trials.

Disconnected commands are rejected, not replayed after reconnection. A write
failure or timeout can leave delivery uncertain; inspect the device state rather
than automatically repeating an action. The command queue is bounded to 64 jobs.

## Screenshots and measurement limits

The receiver handles fragmented binary transfers and validates the ending marker
before publishing an image. Missing responses time out after 10 seconds. Other
commands are rejected during screenshot transfers; serial release remains
available to interrupt a transfer.

Firmware can report runtime panel geometry, row stride, orientation, and output
inversion before each capture. The device sends its original framebuffer.
**All pixel rotation happens in Python**, with no rotated buffer allocated on the
device. PBM images match the active renderer orientation, including both landscape
directions and inverted portrait. Raw files always retain the physical bytes.

| Active renderer orientation | Clockwise rotation applied in Python |
|---|---|
| Portrait | 90° |
| Landscape clockwise | 180° |
| Portrait inverted | 270° |
| Landscape counterclockwise (native panel coordinates) | 0° |

The dimensions come from the display HAL, not a fixed model lookup. For example,
X3 portrait output is 528×792 and landscape output is 792×528; X4 portrait is
480×800 and landscape is 800×480. The same conversion handles other dimensions
and padded rows. Each capture requires firmware metadata; the monitor has no
model-size inference or manual geometry/rotation overrides. PBM conversion
requires neither Pillow nor Matplotlib.

The required metadata line is
`SCREENSHOT_META:1:<width>:<height>:<stride>:<rotation>:<inverted>`, followed by
`SCREENSHOT_START:<length>`, exactly `length` binary bytes, and `SCREENSHOT_END`.
Version 1 specifies row-major, MSB-first 1bpp framebuffer data (1=white), clockwise
rotation in degrees, and inversion as 0 or 1. The host validates geometry against
payload length and rejects captures with missing or invalid metadata. Screenshot
events include physical dimensions/stride, output dimensions, rotation, and
inversion. Incomplete or rejected transfers remain in `serial.bin` for diagnosis;
they are not published as successful screenshots.

The capture is a **1-bit framebuffer**, not the panel's final grayscale image.
The serial handler holds the existing render lock while reading
metadata and streaming the framebuffer, so cooperating render operations cannot
change it mid-capture. Serial output from other
tasks is still not reserved; trailer validation catches some interleaved-log
corruption but is not an integrity checksum. Capture after rendering settles.

For performance comparisons, use the firmware's standard serial timing fields,
preserving their distinction between preparation, rasterization, and display
refresh. Host event timestamps and HTTP round-trip times are not render timings.
Capture screenshots outside timed trials. The metadata change adds no device
framebuffer or cache allocations; conversion buffers are allocated on the host.

## Verification

Run the host suite (localhost socket access is required):

```sh
python3 -m unittest discover -s scripts/tests -v
```

On hardware, verify both server modes: observe serial logs, request a screenshot,
claim control and check interactive-command rejection, then release the serial
port for flashing and reconnect. Confirm the graph continues updating while an API
client reads the same events.

For button input, reader-state assertions, and fixture-driven tests, see
[device-control verification](device-control.md#verification) and
[test fixtures](device-control.md#test-fixtures).
