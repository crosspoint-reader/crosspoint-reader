# Milestone 10 — Optional 800 x 300 gate viewport

## Status

Skipped by product decision. Knietty preserves the validated 80 x 24 layout;
the row loss is not acceptable now that ordinary and fullscreen cadence are
usable. Retain this document only as research context and do not implement the
experiment unless that decision is explicitly revisited.

## Objective

Independently determine whether configuring the SSD1677 for its documented
minimum 300-gate scan materially reduces BUSY duration on this panel.

This is an optional alternate layout, not the normal 80 x 24 target. It comes
late because it gives up roughly nine terminal rows and cannot fix transfer,
queue, or baseline overhead by itself.

## Source anchors

- Driver Output Control command and initialization:
  [`freeink-sdk/libs/display/FreeInkDisplay/src/driver/Ssd1677Driver.cpp`](../../freeink-sdk/libs/display/FreeInkDisplay/src/driver/Ssd1677Driver.cpp#L16)
- Runtime display geometry: [`lib/hal/HalDisplay.h`](../../lib/hal/HalDisplay.h#L111)
- Terminal fixed geometry: [`src/terminal/TerminalScreen.h`](../../src/terminal/TerminalScreen.h)
- Host PTY geometry defaults: [`host-rs/src/cli.rs`](../../host-rs/src/cli.rs)

Reconfirm from the data sheet that gate count 300 is valid, how the count is
encoded, which physical edge anchors the scan, and what reset sequence restores
480 gates. RAM windowing does not imply gate-scan windowing.

## Experiment

Create a separate, clearly named safe-waveform/20 MHz build. Do not combine it
with new LUTs or SPI overclock. Negotiate a reduced row count with the host
rather than rendering 24 clipped rows. Keep 80 columns and header if geometry
allows; derive the row count from the actual oriented viewable region.

Use diagnostic patterns at the first/last active gate and just outside the
intended viewport to determine physical anchoring and leakage. Test normal and
inverted orientation, entry/exit, clean, reset, and sleep/wake. Restore the full
480-gate configuration and repaint before returning to CrossPoint.

## Measurement

Compare identical patterns under 480 and 300 gates:

- BUSY/waveform time;
- activation-to-BUSY and readiness gap;
- visible mapping, clipping, edge artifacts, and ghosting;
- usable PTY rows and application behavior;
- restore reliability across every exit path.

## Stop conditions

Stop immediately for panel-wide corruption, scan beyond the intended area,
stuck BUSY, incomplete restore, persistent edge lines, or sleep/wake regression.
Use normal OTA recovery; do not alter OTP or partitions.

## Complete when

The experiment has a measured speed gain and a reliable restore path, or is
rejected with evidence. It enters a release only as an explicit user-selected
compact viewport, never as a silent replacement for 80 x 24.
