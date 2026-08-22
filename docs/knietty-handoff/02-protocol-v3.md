# Milestone 02 — Negotiated framed protocol v3

## Objective

Add a bounded frame codec and carry ordinary terminal output/input over v3 with
behavioral parity. Preserve v1/v2 fallback throughout this milestone.

## Why framing is required

After the current line handshake, v1/v2 treats every byte as terminal payload.
ANSI and arbitrary application output can contain any byte sequence, so an
escape marker cannot safely distinguish telemetry from terminal data.

## Source anchors

- Firmware greeting and raw stream:
  [`src/terminal/TerminalWifi.cpp`](../../src/terminal/TerminalWifi.cpp#L15)
- Firmware transport state: [`src/terminal/TerminalWifi.h`](../../src/terminal/TerminalWifi.h#L11)
- RX coordination:
  [`src/activities/terminal/TerminalActivity.cpp`](../../src/activities/terminal/TerminalActivity.cpp#L180)
- Rust handshake and fallback: [`host-rs/src/handshake.rs`](../../host-rs/src/handshake.rs)
- Host protocol tests: [`host-rs/src/protocol.rs`](../../host-rs/src/protocol.rs)

## Wire contract

The greeting remains a bounded newline-terminated ASCII negotiation. Define its
exact grammar in a versioned protocol document and test it byte-for-byte. The
v3 greeting includes mode (`terminal` or `diagnostics`) and capability tokens;
the accept response includes geometry and agreed capabilities.

Once accepted, both directions use this network-order header:

```text
u8 type | u8 flags | u16 payload_length | u32 sequence
```

Initial types are `TERMINAL_OUTPUT`, `TERMINAL_INPUT`, `CONTROL_REQUEST`,
`CONTROL_RESPONSE`, `REFRESH_EVENT`, `HEARTBEAT`, and `SESSION_END`. The X4
sends the empty `SESSION_END` frame before a deliberate terminal-mode exit;
host TCP keepalive bounds stale sessions after abrupt WLAN loss. Reserve values explicitly;
unknown optional types are skipped only after validating their bounded length.
Unknown mandatory flags or malformed frames close the connection.

Use a small fixed receive buffer and incremental parser. Never cast incoming
bytes to a packed struct on ESP32-C3; decode integers byte-wise or with `memcpy`
because unaligned multi-byte loads can fault. Cap payloads at a documented
constant no larger than the existing practical RX burst. No per-frame heap.
TCP supplies ordering/checksums; do not add a redundant CRC.

## Implementation order

1. Write the protocol specification and golden byte vectors.
2. Implement the standalone host codec and fragmented/coalesced-read tests.
3. Implement the firmware codec in `src/terminal/` with native tests using the
   same golden vectors.
4. Add v3 negotiation without changing v2 behavior.
5. Carry terminal output and device input through v3 frames.
6. Test v3 disconnect, malformed length, unknown type, sequence rollover, short
   write, and one-byte-at-a-time delivery.
7. Keep v2 as the default fallback until hardware v3 parity is signed off.

## Automated gate

- Golden vectors match between C++ and Rust.
- Fuzz-like table tests cover every header split point and multiple frames in
  one read.
- Payload limits cannot allocate dynamically or overflow arithmetic.
- All existing 24 host tests and 149 native tests remain green, plus new v3
  cases. Safe firmware builds.

## Hardware gate

On macOS, exercise discovery, approval, zsh/tmux, btop, UTF-8 box drawing,
arrows, Enter, Escape, Ctrl+C, Ctrl+\\ exit, device exit, and reconnect. Then
force v2 and repeat a smoke session. Record Linux as untested until repeated on
a real Linux host.

## Complete when

The v3 terminal is behaviorally indistinguishable from v2 on the tested host,
malformed data disconnects safely, and v1/v2 clients still connect.
