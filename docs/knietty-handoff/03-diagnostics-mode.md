# Milestone 03 — Host-controlled diagnostics mode

## Objective

Add a physically approved, deterministic diagnostic session that runs bounded
display tests and writes machine-readable JSON Lines on the host.

## Source anchors

- Approval flow:
  [`src/activities/terminal/TerminalActivity.cpp`](../../src/activities/terminal/TerminalActivity.cpp#L300)
- Existing refresh metrics:
  [`src/activities/terminal/TerminalActivity.cpp`](../../src/activities/terminal/TerminalActivity.cpp#L54)
- Current HAL timing: [`lib/hal/HalDisplay.h`](../../lib/hal/HalDisplay.h#L26)
- Driver timing fields:
  [`freeink-sdk/libs/display/FreeInkDisplay/include/FreeInkDisplayTypes.h`](../../freeink-sdk/libs/display/FreeInkDisplay/include/FreeInkDisplayTypes.h#L15)
- SSD1677 transfer/baseline path:
  [`freeink-sdk/libs/display/FreeInkDisplay/src/driver/Ssd1677Driver.cpp`](../../freeink-sdk/libs/display/FreeInkDisplay/src/driver/Ssd1677Driver.cpp#L431)
- BUSY completion implementation:
  [`freeink-sdk/libs/display/FreeInkDisplay/src/bus/EpdBus.cpp`](../../freeink-sdk/libs/display/FreeInkDisplay/src/bus/EpdBus.cpp#L297)

## Session behavior

The host requests `mode=diagnostics` during v3 negotiation. The X4 displays the
requesting host and an explicit “display diagnostics” approval message using
mapped Confirm/Back controls. No PTY is spawned and normal terminal input is
paused for the session.

Host target command:

```sh
cargo run --release --manifest-path host-rs/Cargo.toml -- \
  diagnose --host auto --suite smoke --output results/run.jsonl
```

Do not silently reuse a terminal approval for diagnostics. Abort immediately on
Back, Power, disconnect, inactivity timeout, command-limit violation, or render
task failure. Always restore activity/display state.

## Events and timing

For every requested update, retain the first/last included sequence and the
number coalesced. Emit:

- `ACCEPTED`: command validated and admitted;
- `PRESENTED`: timestamp captured immediately when BUSY falls;
- `READY`: baseline synchronization and power handling completed;
- `FAILED`: stable error code and last safe phase.

The device reports monotonic relative microseconds; the host reports monotonic
send/event-receive nanoseconds. `PRESENTED` is a BUSY-based proxy, not proof of
human-visible onset. The current blocking renderer captures that timestamp at
BUSY fall but transmits both `PRESENTED` and `READY` after the render call
returns; their device timestamps, not host receive spacing, measure the gap. A
later high-speed-video capture correlates real optics.

Collect RX, parse, queue, render, LUT, first-plane, activation-to-BUSY,
BUSY/waveform, baseline, power-off, total, dirty cells/rows, logical and aligned
rectangles, transfer bytes, requested/actual refresh path, fallback reason,
queue depth, free/minimum heap, RSSI, battery, build revisions, controller,
resolution, profile, SPI clock, font, polarity, orientation, and fading state.

Add timing fields through HAL/FreeInk structs with fixed-width members. Do not
add logging on the terminal payload path or allocate JSON on the firmware; send
compact binary telemetry and serialize JSONL on the host.

## Bounded command set

- reset/seed screen;
- draw named top/middle/bottom cell, adjacent-cell, cursor, row, disjoint-row,
  scroll, 8 KiB boundary, checker, full, and fixed-size burst patterns;
- set normal/inverted polarity;
- request safe or firmware-compiled experimental profile;
- wait for presented/ready;
- clean and stop.

Cap pattern dimensions, repetitions, total activations, payload bytes, and wall
time. Cadence requests may merge only into one fixed pending activation; there
is no dynamically allocated command queue. Do not expose raw
bus/register/OTP/voltage/SPI-frequency commands.

## Automated gate

- Command validation and rejection tests.
- Deterministic event ordering, coalesced sequence accounting, timeout/abort,
  JSONL schema, interrupted write, and disconnect cleanup tests.
- A fake display clock verifies `PRESENTED <= READY` and timing arithmetic across
  `uint32_t` wrap.
- Record static RAM and firmware-size deltas. Keep buffers fixed and small.

## Hardware gate

Run only `smoke` on safe 20 MHz first. Verify the approval text, deny path,
Power/Back abort, host disconnect, normal Terminal session afterward, Home
sleep/wake, and restoration of fading/profile/orientation/polarity. Inspect the
JSONL for one session record and complete event triplets.

## Complete when

The safe build completes and aborts `smoke` without stale display or reader
state, JSONL validates against the documented schema, and no raw diagnostic
control is reachable without physical approval.
