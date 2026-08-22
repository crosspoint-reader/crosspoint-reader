# SSD1677 Mode 2 synchronized-bank smoke result

Date: 2026-08-16
Device: XTEINK X4 `knietty-9e54a0`
Host: macOS Darwin 25.5.0
Firmware: CrossPoint `61e61088`, FreeInk `9406d39`
Capture: `mode2-sync-61e61088-smoke.jsonl`
Capture SHA-256: `2c5c280b0e141b8d54e0266b3b66ccd5fbc5e770cf2e1edd6a3c2294ea7854c2`

## Result

The synchronized-bank design passed the bounded visual correctness gate. The
user reported that the stale diagnostics waiting page and alternating old/new
window content were gone and that the display looked very good. All 14 commands
were accepted with error zero, all 13 refreshes produced ordered
PRESENTED/READY pairs, and there were no failed events. Session metadata
reported `ram_ping_pong: true`; all 13 completed updates reported the expected
nonzero synchronization baseline. Free heap was 62,692 bytes and the reported
minimum was 44,644 bytes.

The design is nevertheless rejected as a product optimization because it did
not recover a useful timing budget. Four true windows had a 102.5 ms median
display time versus 102.1 ms in the retained nominal-W100 control. Their
one-bank regional synchronization cost only 0.9 ms median, so interactive typing
was effectively unchanged.

Eight fast full-frame fallbacks had a 349.9 ms median display time versus 207.5
ms in the control. Their primary write increased from 36.2 to 113.4 ms median,
and the new inactive-bank synchronization cost another 122.1 ms median. Total
non-waveform transfer time increased from 106.9 to 249.7 ms. This smoke sample
is sufficient to reject promotion: Mode 2 does not improve optical quality by
itself, and the full-frame regression conflicts with the requirement to retain
the accepted TUI cadence. Latency, cadence, and burst were intentionally not
run.

The session profile byte unexpectedly reported zero even though the build flags
and every interactive BUSY measurement identify the intended approximately
100 ms W100 Sustain1 waveform. The live profile byte can be sampled while a
diagnostics render temporarily selects `PanelDefault`; treat this as telemetry
cleanup for the async milestone, not as evidence that the wrong LUT ran.

## Decision

Keep the Mode-2 implementation isolated behind
`FREEINK_X4_SSD1677_MODE2_PINGPONG` for reproducibility, but do not enable it in
the normal experience profile. Milestone 08 must use the ordinary
`knietty_adaptive_100ms_sustain_nosettle` driver path and its measured legacy
post-BUSY baseline contract.
