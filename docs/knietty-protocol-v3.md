# knietty protocol v3

Protocol v3 keeps the v1 UDP discovery request and TCP port so old and new
hosts find the same device. It replaces the post-approval raw stream with
bounded frames that can later be wrapped unchanged by TLS.

## Negotiation

All negotiation lines are ASCII, newline terminated, and limited to 127 bytes.
The client name is 1–32 sanitized characters. Integers are decimal.

Terminal request:

```text
KNIETTY/3 HELLO terminal frame <epoch> <UTC-offset-minutes> <client-name>\n
```

Diagnostics request:

```text
KNIETTY/3 HELLO diagnostics frame,diag1 <epoch> <UTC-offset-minutes> <client-name>\n
```

Acceptance is mode-specific:

```text
KNIETTY/3 ACCEPT 80 24 frame\n
KNIETTY/3 ACCEPT 80 24 frame,diag1\n
```

`KNIETTY/1 ERROR`, `DENY`, and `BUSY` retain their existing meanings. A v3 host
may reconnect with v2 and then v1 after `ERROR`. Firmware continues accepting
the exact existing v1/v2 greetings and uses their raw stream after approval.

## Frame format

Every multi-byte integer is unsigned and network byte order:

```text
u8 type | u8 flags | u16 payload_length | u32 sequence | payload
```

The header is 8 bytes. Payload length is at most 512 bytes and flags must be
zero in version 3. Initial types are:

| Value | Name | Direction |
| --- | --- | --- |
| `0x01` | `TERMINAL_OUTPUT` | host to X4 |
| `0x02` | `TERMINAL_INPUT` | X4 to host |
| `0x03` | `CONTROL_REQUEST` | host to X4 |
| `0x04` | `CONTROL_RESPONSE` | X4 to host |
| `0x05` | `REFRESH_EVENT` | X4 to host |
| `0x06` | `HEARTBEAT` | either direction |
| `0x07` | `SESSION_END` | X4 to host |

`SESSION_END` has an empty payload. The X4 sends it before a deliberate exit
from terminal mode. A host closes the connection immediately on receipt; with
reconnect disabled this is a clean successful exit, while reconnect mode goes
back to discovery. TCP keepalive remains the fallback for abrupt power or WLAN
loss where the X4 cannot send this frame.

Unknown types below `0x80`, nonzero flags, and oversized payloads are protocol
errors and close the connection. Types with bit `0x80` set are optional: a
receiver validates their header and bounded length, consumes them, and ignores
their payload. TCP provides ordering and integrity, so frames add no CRC.

Sequence numbers wrap modulo 2^32. Terminal data uses a monotonically
increasing sequence independently in each direction. Control responses and
refresh events copy the request sequence they describe.

Golden frame for sequence `0x01020304` and terminal payload `abc`:

```text
01 00 00 03 01 02 03 04 61 62 63
```

## Resource limits

Firmware decodes into one fixed 512-byte payload buffer and queues outbound
bytes in one fixed 1,024-byte ring. It never allocates per frame. A complete
frame can remain pending while its consumer drains it; the decoder does not
overwrite that payload.

## In-session display control

A protocol-v3 terminal session may carry the same `CONTROL_REQUEST`,
`CONTROL_RESPONSE`, and `REFRESH_EVENT` frames used by diagnostics, but only a
small product allowlist is accepted: session info (`01`), explicit polarity
(`04 00` or `04 01`), and a safe clean (`05`). Reset, pattern, stop, malformed,
and unknown commands are rejected. There are still no raw register, LUT,
voltage, OTP, rectangle, or SPI controls.

Only one refresh-producing display command may be active. Session info returns
after its accepted metadata response. Polarity and clean first return an
accepted response, then PRESENTED and READY events; callers must wait for READY
before reporting success. Clean uses the normal HALF refresh path. Explicit
polarity repaints the current terminal state and does not alter persisted
CrossPoint settings.

The Rust foreground bridge exposes these operations to another local `knietty`
process through a per-user mode-`0600` Unix socket in a mode-`0700` runtime
directory. That local IPC syntax is not part of the X4 wire protocol, and no
second TCP connection or physical approval is created.

## Diagnostics schema `diag1`

Diagnostics is available only after the separate `diagnostics frame,diag1`
greeting and physical approval on the X4. Terminal frames are rejected in this
mode. Command payloads are deliberately not extensible raw driver calls:

| Command | Payload | Meaning |
| --- | --- | --- |
| `1` | `01` | session/build metadata |
| `2` | `02` | reset the deterministic screen model |
| `3` | `03 pattern variant` | draw a named pattern; variant is `0` or `1` |
| `4` | `04 polarity` | normal `0` or inverted `1` |
| `5` | `05` | safe clean refresh |
| `6` | `06` | stop |

Named patterns are cell-top `1`, cursor `2`, row `3`, disjoint rows `4`, scroll
`5`, checker `6`, full `7`, cell-middle `8`, cell-bottom `9`, adjacent cells
`10`, four-row under-8-KiB boundary `11`, five-row over-8-KiB boundary `12`,
and deterministic 1/2/5/10/25/100-cell bursts `13` through `18`. Variant `0`
paints the named cells white and variant `1` paints them black where the pattern
has a directional interpretation. Payloads must have exactly the documented
length. There are no register, LUT, voltage, OTP, arbitrary rectangle, or SPI
clock commands.

Every `CONTROL_RESPONSE` starts with:

```text
u8 schema=1 | u8 command | u8 status | u8 error
```

Status is accepted `0` or rejected `1`. Stable errors are none `0`, malformed
`1`, unknown command `2`, invalid argument `3`, command limit `4`, activation
limit `5`, timeout `6`, busy `7`, transport `8`, and aborted `9`.

An accepted session-info response appends:

```text
u8 profile | u8 spi_mhz | u8 flags | u8 orientation
u8 board | u8 controller | u8 battery_percent | i8 rssi_dbm
u8 columns | u8 rows | u8 font | u16 display_width | u16 display_height
u32 free_heap | u32 minimum_free_heap | u8 build_length | build UTF-8 bytes
u8 freeink_length | FreeInk revision UTF-8 bytes
```

Flag bits are inverted `0x01`, fading fix `0x02`, adaptive build `0x04`, and
out-of-spec 40 MHz build `0x08`. A nominal 100 ms terminal waveform is `0x10`;
the measured BUSY duration in each refresh event remains authoritative.

Each display command produces an accepted response followed by 108-byte
`REFRESH_EVENT` payloads for PRESENTED `1` and READY `2` (FAILED is `3`):

```text
u8 schema, phase, command, requested_path, actual_path, fallback_reason,
   flags, queue_depth
u32 timestamp_us, rx_at_us, parsed_at_us, queued_at_us,
    render_started_at_us, queue_us, render_us, transfer_us, lut_us,
    plane_us, activation_to_busy_us, waveform_us, baseline_us,
    power_off_us, total_us
u16 logical_x, logical_y, logical_width, logical_height
u16 aligned_x, aligned_y, aligned_width, aligned_height
u32 transfer_bytes
u16 dirty_cells | u8 dirty_rows | u8 coalesced
u32 first_sequence | u32 last_sequence | u32 free_heap | u32 minimum_free_heap
```

Paths are none `0`, window fast `1`, full-frame fallback fast `2`, and safe
half `3`. Fallback is none `0` or unsupported/too-large `1`. Refresh flag bits
are inverted `0x01`, windowed `0x02`, and fading fix `0x04`.

The SSD1677 captures PRESENTED's device timestamp immediately after BUSY falls;
READY is captured after baseline and power handling. The current blocking
renderer transmits the two records together after READY, so host receive times
measure delivery while the device timestamps distinguish those phases. Device
timestamps and durations are unsigned 32-bit microseconds and use modular
arithmetic across wrap.

A session permits at most 96 valid commands, 96 admitted display activations,
30 seconds of inactivity, and 180 seconds wall time. Back, Power, disconnect,
timeout, or a limit violation aborts it. One activation may execute while one
fixed pending aggregate collects later named pattern requests. The aggregate
keeps the first/last sequence and count, merges dirty cells in the existing
screen model, and produces one PRESENTED/READY pair. It allocates no command
queue and therefore mirrors latest-frame coalescing without unbounded storage.
