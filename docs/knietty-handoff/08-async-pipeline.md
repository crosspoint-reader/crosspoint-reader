# Milestone 08 — Async window pipeline and latest-frame-wins

## Objective

Receive and parse while the panel is BUSY, prepare one bounded pending update,
and launch it immediately when the driver becomes ready. Avoid FIFO buildup of
obsolete terminal frames.

## Phase A finding: remove transport backlog first

Source inspection at parent `e645ff96` found that two parts of the intended
scheduler already exist:

- `ActivityManager` runs `TerminalActivity::render()` on a separate FreeRTOS
  task, so the main task continues TLS receive and ANSI parsing during BUSY.
- `TerminalRenderGate` already retains exactly one replay when any number of
  updates arrive during an in-flight render, and the next render snapshots the
  newest `TerminalScreen` state.

One concrete pre-panel inefficiency was framed TLS input: firmware requested one
decrypted byte per `wolfSSL_read()` and Terminal drained only 256 payload bytes
per main-loop pass. The Rust host also nominally paced PTY output at 64 KiB/s.
The Phase A candidate replaces those per-byte TLS calls with exact, bounded
decoder reads, drains at most 2 KiB or 2 ms per loop, and raises the configured
host ceiling to 256 KiB/s. It adds no framebuffer or persistent heap allocation.

The user physically A/B tested `--max-bps 65536` and the new 256 KiB/s default
on the matching `e645ff96` firmware and saw no meaningful visual difference.
That was a useful negative result for the configured ceiling, but later source
inspection found that the bridge never approached either ceiling during a
multi-frame burst. Retain the bounded bulk reads as an efficiency fix; the A/B
does not exclude the host wake defect described below.

## Phase B candidate: sparse activation overlap

The isolated `knietty_async_window` environment keeps the accepted 20 MHz,
100 ms waveform, sustained interactive profile, and no automatic settle. For
ordinary connected terminal output only, it:

- packs up to 24 byte-aligned dirty terminal row spans into one lazily allocated
  8 KiB staging buffer;
- writes all spans to SSD1677 BW RAM, then starts one asynchronous FAST
  activation;
- continues receiving/parsing on the main task and composes the latest pending
  terminal state into the normal framebuffer while BUSY;
- after BUSY, restores only the active spans to the controller's BW/RED
  differential baseline from immutable staging, then immediately activates the
  newest merged pending spans; and
- yields after at most four consecutive activations so exit, status, clean,
  polarity, and other Activity work cannot starve.

Oversized, full-screen, first-render, disconnected, diagnostics, clean, settle,
and inverted updates retain the proven blocking path. There is no second 48 KiB
framebuffer. The key hardware gate is whether several disjoint SSD1677 RAM-area
writes are all honored by one global activation; compilation cannot prove that.

The user physically validated the Phase B image: terminal contents, controls,
exit, and reader behavior showed no regressions, proving the multi-region
activation and baseline replay are coherent on this X4. It produced no visible
typing or btop improvement. btop still paints in chunks and its clock advances
every two to three seconds despite the host configuration explicitly using
`update_ms = 500`. Live status after the staging buffer was exercised reported
53,416 bytes free heap and a 44,588-byte minimum, so the bounded allocation is
present and did not approach OOM. This is a negative performance result, not a
failed correctness result.

The most likely explanation is coverage: the 8 KiB cap represents only about
19% of the 792 x 432 terminal area, and five full-width 18-pixel terminal rows
already exceed it. A fullscreen TUI can therefore take the unchanged blocking
fallback even when only some cells differ. Confirm this from the existing
timing page's window/fallback counters before increasing memory or changing the
baseline algorithm.

The next candidate adds a direct fetch route instead of asking the tester to
transcribe that page. While the foreground host and btop remain connected, a
second shell can run:

```sh
knietty display metrics --json
```

The command uses the authenticated protocol-v3 control channel and originally
returned one fixed 84-byte snapshot containing update/window/fallback/settle/
clean counts, timing, region, and heap data. It is read-only: no framebuffer
mutation, render request, or panel refresh occurs. The current 108-byte response
appends RX, burst-boundary, snapshot/timeout, and async-tail counters; the Rust
decoder accepts both lengths. Older firmware rejects command 7 cleanly.

## Phase C candidate: wake correctly and present complete PTY bursts

The physical metrics route changed the diagnosis. With btop active, firmware
presented about 5.3 updates per second and almost all were windowed, even while
the visible clock arrived two to three seconds late. The panel pipeline was
active; one logical btop repaint was reaching it in many small pieces.

The Rust v3 bridge encodes at most 512 PTY bytes per frame. After a successful
write it schedules the next frame from `written / max_bps`, about 2 ms at the
256 KiB/s default. Previously, once that encoded frame drained,
`poll_connected()` disabled PTY reads and slept for the generic 100 ms event-loop
interval instead of the pacing deadline. The effective limit was therefore
about 512 bytes per 100 ms, or 5 KiB/s. A typical 10--15 KiB TUI repaint took the
observed two to three seconds before E Ink timing was involved.

The `knietty-0.1.0` candidate makes two bounded changes:

- the host wakes at `next_write_at`, drains consecutive 512-byte frames at the
  configured pacing rate, and tests a 4 KiB burst end to end; and
- matching peers negotiate optional `burst1`. The host sends an optional,
  zero-payload `TerminalOutputEnd` frame after 24 ms without new PTY output.
  Firmware presents the latest accumulated model at that boundary rather than
  refreshing every transport fragment. An 80 ms timeout measured from the last
  payload guarantees progress if a marker is lost without splitting a long
  active burst. Firmware does not latch boundary mode until it observes the
  first marker, so old hosts retain the legacy 8/20 ms batching path.

No new framebuffer is allocated. Read-only metrics now report host PTY
bytes/reads/frames/boundaries and device RX reads/bytes, boundaries, snapshots,
timeouts, and async-tail activations so the physical gate can distinguish
transport, batching, and panel time without a packet capture.

The physical gate confirmed fast btop loading, byte-perfect delivery, and two
completed bursts per second. Fifty apparent fail-safe timeouts were not evidence
that 80 ms was too short. The regular render path sampled its start time before
taking `modelMutex`; RX could then publish a newer `lastQueuedAt`, making the
unsigned age subtraction underflow. `knietty-0.1.1` samples `millis()` after the
timestamp load and retains the 80 ms fail-safe. This is a firmware-only fix.

## Source anchors

- Terminal RX and dirty scheduling:
  [`src/activities/terminal/TerminalActivity.cpp`](../../src/activities/terminal/TerminalActivity.cpp#L180)
- Host PTY pacing and burst boundary:
  [`host-rs/src/bridge.rs`](../../host-rs/src/bridge.rs#L1138)
- Optional protocol frame:
  [`src/terminal/TerminalProtocol.h`](../../src/terminal/TerminalProtocol.h)
- Current blocking window refresh:
  [`src/activities/terminal/TerminalActivity.cpp`](../../src/activities/terminal/TerminalActivity.cpp#L642)
- Activity's two fixed screen models:
  [`src/activities/terminal/TerminalActivity.h`](../../src/activities/terminal/TerminalActivity.h#L63)
- HAL async API: [`lib/hal/HalDisplay.h`](../../lib/hal/HalDisplay.h#L61)
- Facade async state:
  [`freeink-sdk/libs/display/FreeInkDisplay/src/FreeInkDisplay.cpp`](../../freeink-sdk/libs/display/FreeInkDisplay/src/FreeInkDisplay.cpp#L514)
- SSD1677 start/finish:
  [`freeink-sdk/libs/display/FreeInkDisplay/src/driver/Ssd1677Driver.cpp`](../../freeink-sdk/libs/display/FreeInkDisplay/src/driver/Ssd1677Driver.cpp#L411)

## Scheduler contract

There may be one displayed/in-flight snapshot and one newest pending snapshot.
While BUSY:

- network RX and parsing continue;
- new mutations merge into the pending dirty region;
- superseded intermediate presentation requests are coalesced, not queued;
- sequence accounting records everything represented by the pending image;
- the framebuffer/read sources required by the active transfer remain immutable.

When `PRESENTED` occurs, perform only the driver work proven necessary by
Milestone 07, then launch the newest pending state. A continuous stream must not
starve input, exit controls, diagnostics events, idle settle, or mandatory clean.

Do not allocate a second 48 KiB E Ink framebuffer on ESP32-C3. Reuse the two
bounded terminal models and a fixed/capped window staging buffer. Any window
above the cap uses a documented full-frame path without hidden vector growth.

## Implementation order

1. Bulk TLS reads and compare configured host ceilings. Complete; no visible
   difference, later shown to be masked by the poll wake defect.
2. Split sparse window refresh into start/finish using immutable bounded
   staging. Complete and physically coherent, with no visible cadence gain.
3. Expose read-only metrics through the active bridge. Complete and physically
   fetched; it localized the remaining lag before the panel.
4. Wake at the real pacing deadline and add a negotiated complete-burst marker.
   Complete; the physical `knietty-0.1.0` gate passed.
5. Correct the render timestamp race without changing the 80 ms fail-safe.
   Software gates pass in `knietty-0.1.1`; physical metrics are next.
6. Retain latest-state coalescing and add no more display buffering. If the
   follow-up gate fails, compare both host and device pipeline counters before
   changing the SSD1677 path.
7. Keep quiet settle/clean lower priority and retain bounded fairness so a
   never-ending stream still handles Power/Back.

For X4 single-buffer FAST fallback, the framebuffer is the post-waveform source
used to restore the controller's BW/RED baseline. It must remain immutable until
READY unless a bounded active-window staging buffer protects that source. Never
hide a second 48 KiB allocation behind the existing async facade.

## Automated gate

Use a fake display with controlled BUSY transitions. Test input arriving before
activation, during BUSY, during post-work, and exactly at completion. Assert no
framebuffer mutation while a consumer requires it, no lost final state, correct
dirty unions, correct sequence ranges, and bounded pending storage.

## Hardware gate

Install the `knietty-0.1.1` firmware with the matching `knietty-0.1.0` host.
Confirm the build name,
run btop at `update_ms = 500` for one minute, and fetch metrics from a second
shell. Require `host_pipeline.burst1 = true`, advancing host/device boundary and
snapshot counters, normally zero boundary timeouts, coherent final contents,
and materially less 2--3 second chunking. Exercise fast typing, a 200-line
burst, btop, Ctrl+C, display commands, disconnect, exit, polarity, reconnect,
and subsequent Home sleep/wake.

## Complete when

The final terminal state is always correct, memory is bounded, queue latency is
substantially reduced or the result is honestly negative, and safe display
quality/sleep/recovery behavior is unchanged.
