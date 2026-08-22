# Milestone 07 — SSD1677 volatile Mode 2 RAM ping-pong

## Status

The first zero-copy candidate from parent `4f105b1f` with FreeInk `8ff8d51`
was rejected at the physical smoke gate. Keystrokes could replace the terminal
with portions of the diagnostics waiting page and content alternated inside
windows. This confirms that R37h/F6 exchanges complete bank roles; an untouched
region in the other bank does not automatically inherit the current frame.

The second candidate uses FreeInk `9406d39`. The separately named
`knietty_mode2_pingpong` environment retains
W100 Sustain1, no automatic settle, 20 MHz SPI, TLS, and the current terminal
layout. It is compile-time gated, runtime-limited to `Board::XteinkX4`, and
advertises `ram_ping_pong: true` through session metadata. Seven pure state-model
tests cover unavailable hardware, required seeding, odd/even full/window
activations, an interrupted synchronization, clean/reset, abort/power loss, and
invalidation. Parent `61e61088`, 174/174 native tests, the dedicated firmware
build, and the ordinary no-flag firmware regression pass. The replacement image
passed the physical visual-correctness smoke gate: stale/alternating regions
were eliminated. It is still rejected for the product profile because its
full-frame fallback median rose from 207.5 to 349.9 ms while true-window latency
remained effectively unchanged at 102.5 ms versus 102.1 ms. No later suite was
needed to establish that it recovered no quality budget.

The rejected candidate's exact parent source checkpoint is `4f105b1f`. Its
experimental build reports 54,308 / 327,680 bytes RAM and 5,702,141 /
6,553,600 bytes flash. The ordinary W100 Sustain1/no-settle environment also
rebuilt successfully without the Mode-2 flag at 54,292 bytes RAM and 5,701,477
bytes flash.

The driver seeds both complete RAMs through the existing absolute path before
writing volatile R37h. While active it writes the new target through 0x24, lets
the controller exchange bank roles after the Mode-2 activation, and then copies
the presented full frame or dirty window through 0x24 into the newly inactive
bank. This replaces the legacy two-plane post-BUSY synchronization with one
target-plane synchronization. Any HALF/FULL, dark-background, settle,
async, dual-buffer, grayscale, turn-off, reset, or sleep transition leaves the
experiment through controller reset plus an absolute seed. The implementation
never references or sends Program OTP Selection command 0x36.

## Objective

Determine whether SSD1677 Display Mode 2 can eliminate the measured post-BUSY
BW/RED baseline rewrite without stale pixels or polarity errors.

The product target is not maximum refresh rate. Preserve the currently accepted
interactive cadence and treat any recovered `READY - PRESENTED` time as budget
for better contrast and lower ghosting in Milestone 09. Mode 2 must first pass
as an isolated bookkeeping optimization; do not alter the waveform in this
experiment or credit it with an optical improvement it cannot produce directly.

## Source anchors

- Current full-frame baseline rewrite:
  [`freeink-sdk/libs/display/FreeInkDisplay/src/driver/Ssd1677Driver.cpp`](../../freeink-sdk/libs/display/FreeInkDisplay/src/driver/Ssd1677Driver.cpp#L506)
- Current window baseline rewrite:
  [`freeink-sdk/libs/display/FreeInkDisplay/src/driver/Ssd1677Driver.cpp`](../../freeink-sdk/libs/display/FreeInkDisplay/src/driver/Ssd1677Driver.cpp#L577)
- Driver state/profile fields:
  [`freeink-sdk/libs/display/FreeInkDisplay/src/driver/Ssd1677Driver.h`](../../freeink-sdk/libs/display/FreeInkDisplay/src/driver/Ssd1677Driver.h#L139)
- Current timing facade:
  [`freeink-sdk/libs/display/FreeInkDisplay/src/FreeInkDisplay.cpp`](../../freeink-sdk/libs/display/FreeInkDisplay/src/FreeInkDisplay.cpp#L942)

Also re-read the retained SSD1677 data sheet section for volatile display-option
register `0x37`, F6, immediately before implementation. Do not infer behavior
from the register name alone.

## Experiment design

Create a separately named 20 MHz experimental environment. On entry:

1. initialize the controller normally;
2. seed both complete RAM banks with the same known screen;
3. set only the documented volatile Mode 2 option;
4. run one update at a time, waiting for `PRESENTED` and `READY`;
5. track the expected bank role explicitly in driver state;
6. disable Mode 2 and perform an absolute clean before returning to CrossPoint.

Do not program waveform/display-option OTP. Do not combine this experiment with
40 MHz, custom waveform changes, gate MUX changes, or async scheduling.

Test full frame and byte-aligned windows separately. Alternate black/white,
single-pixel-cell, disjoint-row, scroll, checkerboard, and inverted patterns so
a wrong old/new bank becomes visually obvious. Force disconnect/abort after odd
and even activation counts.

The first candidate intentionally exercised controller window semantics without
a post-BUSY copy. An earlier screen and old regions did reappear, rejecting that
zero-copy design. The second candidate synchronizes the inactive bank after
every activation. Stop if any earlier screen, cell, cursor, or disjoint row
returns; that would reject synchronized Mode 2 as well.

## Diagnostics questions

- Is the one-plane synchronization `baselineUs` reliably lower than the legacy
  two-plane baseline?
- Is `READY - PRESENTED` reduced after including that synchronization?
- Does the controller swap after every activation, only selected modes, or not
  as expected?
- Do window and full-frame activation use the same bank semantics?
- What happens after clean/HALF, profile change, reset, sleep, and power-off?

## Automated gate

Model the bank-state transition table with unit tests before hardware. Test
entry seeding, odd/even updates, window/full transitions, abort, power-down,
clean, and restore. All unsupported controllers must stay on their existing path.

## Hardware gate

Use the already physically validated W100/TLS experience as the control; no new
safe binary is required for this first trial. Flash only the exact labeled
Mode-2 experiment through the proven SD path. Abort at the first corruption,
stuck BUSY, refresh instability, or wake regression; recover through normal OTA
or the prior known-good SD image. Capture identical baseline-v1 suites
before/after.

Physically correct but timing-rejected synchronized-bank candidate:

```text
/Users/rodrigomtorres/git/knietty/knietty-M7-MODE2-SYNC-61e61088-W100-SUSTAIN1-NOSETTLE-20MHz-EXPERIMENTAL.bin
SHA-256 98370a58aad1b034b68077a93142e1b8bd3cca5dfbb93eac62860a54c4f7bbf2
```

Rejected zero-copy artifact — retain for provenance but do not flash again:

```text
/Users/rodrigomtorres/git/knietty/knietty-M7-MODE2-PINGPONG-4f105b1f-W100-SUSTAIN1-NOSETTLE-20MHz-EXPERIMENTAL.bin
SHA-256 7ab515b7ab154de00feffcf2fae91a88a905f90d6ebdaf8b526540888a9a6811
```

Completed first-run order for the synchronized candidate:

1. Confirm the timing page says `W100 sustain / Mode 2 ping-pong`.
2. Run only `smoke` first and watch every transition. In particular, the first
   cell must not return during the cursor update, and the first dirty row must
   not return during the disjoint-row update.
3. Confirm the JSONL session record contains `"ram_ping_pong":true`. The second
   candidate intentionally reports nonzero `baseline_us` for its one-plane
   post-BUSY synchronization; compare it with the legacy two-plane baseline.
4. Confirm checker/full, invert-and-return, final clean, exit, and normal
   sleep/wake are correct.
5. Only after that pass, run latency, cadence, and burst, followed by ordinary
   typing and btop. Stop at the first stale-region, odd/even, BUSY, or recovery
   failure.

Step 5 was deliberately skipped: the smoke capture already measured a decisive
full-frame timing regression, so further activations could not change the
promotion decision.

## Complete when

Either Mode 2 has repeatable visual correctness and a measured readiness gain,
without making the current waveform's readability or ghosting worse, or it is
rejected with a reproducible failure record. A negative result is a valid
completed milestone; do not retain speculative driver state. A successful
result provides a measured time budget for later waveform-quality work rather
than automatically becoming a faster terminal profile.

## Final decision

Milestone complete with a negative optimization result. Zero-copy Mode 2 is
visually incorrect for window updates. Synchronizing the inactive bank repairs
correctness but makes full-frame fallback about 142 ms slower at the median and
does not improve the waveform or panel clarity. Retain the experiment behind
its compile flag for provenance, leave it disabled in normal builds, and carry
the measured legacy baseline into Milestone 08.
