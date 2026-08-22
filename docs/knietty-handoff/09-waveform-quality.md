# Milestone 09 — Adaptive waveform quality

## Objective

Find the best measured interactive speed/contrast/ghosting tradeoff after
scheduler and baseline overhead are understood. Safe 20 MHz remains the release
control until a replacement clearly wins.

The current interactive speed is accepted. The product target is to retain that
perceived cadence while improving black/white separation, sustained readability,
and ghost resistance. If Mode 2 removes baseline bookkeeping time, reinvest only
the measured saving in controlled waveform variants; do not turn it into a
lower-latency target unless quality is unchanged or better.

## Source anchors

- Adaptive build flags: [`platformio.ini`](../../platformio.ini#L182)
- SSD1677 refresh/profile selection:
  [`freeink-sdk/libs/display/FreeInkDisplay/src/driver/Ssd1677Driver.cpp`](../../freeink-sdk/libs/display/FreeInkDisplay/src/driver/Ssd1677Driver.cpp#L275)
- Custom LUT loading/timing:
  [`freeink-sdk/libs/display/FreeInkDisplay/src/driver/Ssd1677Driver.cpp`](../../freeink-sdk/libs/display/FreeInkDisplay/src/driver/Ssd1677Driver.cpp#L690)
- Terminal settle scheduling:
  [`src/activities/terminal/TerminalActivity.cpp`](../../src/activities/terminal/TerminalActivity.cpp#L642)
- Cursor rendering: [`src/terminal/TerminalFont.cpp`](../../src/terminal/TerminalFont.cpp#L41)

Re-read the SSD1677 waveform table encoding and voltage mapping from the retained
data sheet. Do not copy timings or voltages from a different panel/controller.

## Controlled variants

Change one dimension per build and encode it in the artifact name/telemetry:

- safe stock partial at 20 MHz;
- current one-frame adaptive at 40 MHz;
- twenty-frame adaptive at 20 MHz, targeting about 100 ms of directional drive;
- only after that comparison, shorter and longer 20 MHz durations that bracket
  the best observed quality/latency point.

Start from the controller's existing X4 analog settings. Do not alter source/gate
voltages in the first comparison. SPI overclock affects transfer only; waveform
frames affect particle drive. Keep those facts separate in analysis. The nominal
duration is not a measurement: use diagnostic BUSY time as the actual result.

The first twenty-frame/20 MHz build measured approximately 100 ms BUSY and made
ordinary typing much better, but sustained typing and btop progressively turned
the screen grainy gray. An allocation-free final-state dirty-pruning A/B did not
resolve the optical degradation or apparent TUI cadence. Treat redundant TUI
repainting as falsified as the primary cause; retain the pruning, but do not
credit it with a display-quality win.

The user confirmed btop was already running with an explicit 500 ms interval,
so default btop cadence is not the skipped-frame explanation. Two independent
candidates built from the `c41de6e3` parent completed physical A/B:

- `knietty_adaptive_100ms_nosettle` changes only scheduling. It suppresses the
  250 ms automatic quiet-time DU settle and retains the 80-update HALF clean.
- `knietty_adaptive_100ms_sustain` changes only the volatile LUT. It retains
  automatic settle, 20 MHz SPI, the 100 ms total, VCOM, and X4 analog values.
  The twenty 5 ms frames become one balance frame, one opposite restore frame,
  and eighteen final-target frames. Unchanged black and white receive opposite
  two-phase pulses instead of staying idle.

Do not combine either experiment with RAM ping-pong, async work, VCOM, SPI, or
other voltage changes. The no-settle image is a cadence-attribution tool, not a
quality candidate. The Sustain1 image directly tests whether small global
common-electrode/gate disturbances accumulate because unchanged pixels have no
charge-balanced phase.

Physical verdict: no-settle still displayed btop clock changes only every two
to three seconds and eventually grained, but its run had a rapid 68--78% battery
oscillation. Source inspection found Terminal sampling the plain X4's ADC on
every loop and repainting the full-width header whenever the integer percentage
changed. That header can merge with content into a near-full-screen fallback, so
the no-settle cadence result is confounded. Sustain1 produced near-perfect typing
apart from acceptable cursor ghosting, but btop grained severely. In both images,
switching to inverted output and back temporarily removed the grain.

The next combined experience candidate retains Sustain1, disables automatic
settle, and limits Terminal battery-header sampling to once per minute. This is
the minimal retest of btop foreground cadence. It is not a new grain cure.

The physical W100 result shows that an idle unchanged-pixel LUT is insufficient:
every small RAM-window update still triggers a global 480-gate activation, and
untouched areas progressively drift gray. Reserve a short, charge-balanced
sustain/restore pair for unchanged black and unchanged white, then use the
remaining duration as the final transition-direction drive for changed pixels.

The lock-in candidate replaces the inverse block cursor with a one-pixel
underline before final ghosting judgment; its physical residue still needs
confirmation. Quiet-time settling should cover only cells/pixels changed during
the burst, not blindly repaint the entire terminal.

## Measurement

Run baseline-v1 `latency`, `cadence`, and opt-in `ghosting` without changing
suite order or environmental metadata. Report:

- BUSY and total timing for each waveform;
- black-to-white and white-to-black separately;
- first-frame contrast and residue after 10/25/50/100 changes;
- quality after idle settle and after clean;
- queue/coalescing to ensure scheduler differences are not credited to LUTs;
- subjective reading notes plus fixed-camera images when available.

Normalize photographs for lighting/exposure only if the raw originals are kept.
Do not convert subjective quality into invented precision.

Use this order:

1. Flash `W100-SUSTAIN1-NOSETTLE-BATT60` from a clean endpoint. Run the same
   btop 500 ms workload. Verify zero settles, at most one battery change per
   minute, and record window/fallback/queue/cadence counts.
2. Recheck the already-good typing path independently from btop grain. If the
   stable header lowers fallbacks and btop approaches source cadence, retain the
   scheduler/status fix even if the optical profile remains rejected.
3. Separate the inversion cleanup before another LUT. A full invert-and-return
   both drives the panel in two directions and rewrites both controller planes.
   Compare RAM-only full-plane reseed, current-target-only activation, and the
   two-direction scrub through bounded diagnostics. Only then isolate VCOM or
   unequal-duration balance pulses.

## Safety gate

All variants use volatile LUT/register writes only. The first quality experiment
uses the SSD1677's specified 20 MHz maximum write clock. Restore `PanelDefault`
and perform the established clean on exit. Cap test activations and stop on
stuck BUSY, unexpected flashing, severe persistent residue, or thermal/power
anomaly. Never issue OTP commands.

## Complete when

One profile is selected from reproducible speed and legibility evidence, or safe
20 MHz remains the winner. Record rejected variants and reasons so future agents
do not repeat them under new names.
