# SSD1677 Mode 2 zero-copy smoke result

Date: 2026-08-16
Device: XTEINK X4 `knietty-9e54a0`
Host: macOS Darwin 25.5.0
Firmware: CrossPoint `4f105b1f`, FreeInk `8ff8d51`
Capture: `mode2-pingpong-4f105b1f-smoke.jsonl`
Capture SHA-256: `d47cda6711a943e2c0ce79c7bd94e639283adcd2a31b886a56932cc74125970d`

## Result

Rejected at the bounded smoke gate. The user observed that ordinary terminal
keystrokes replaced the terminal with portions of the diagnostics waiting
screen and that old/new content alternated within windows. This is the expected
signature of complete controller RAM banks exchanging roles while only one
bank's dirty window has been updated. No latency, cadence, or burst suite was
run with this image.

The transport and telemetry were internally consistent: all 14 commands were
accepted with error zero, all 13 refreshes produced ordered PRESENTED/READY
pairs, session metadata reported `ram_ping_pong: true`, and all 12 interactive
refreshes reported `baseline_us: 0`. The final absolute clean was the only
nonzero baseline (`273953 us`). Free heap stayed at 62,728 bytes and the
reported minimum was 42,404 bytes.

The zero-copy path was not a timing win even before correcting the stale-bank
failure. Four true windows had a 101.8 ms median display time, essentially the
same as the retained nominal-W100 control (102.1 ms). Eight fast full-frame
fallbacks had a 229.8 ms median versus 207.5 ms in that control. Their primary
plane write rose to a 125.5 ms median while the removed legacy baseline had
previously cost roughly 69 ms. This one unmatched run is not enough to assign a
cause, but it is enough that no speed gain is claimed.

## Next candidate

After each Mode-2 activation, copy only the just-presented full frame or dirty
window through `0x24` into the newly inactive bank. The state machine represents
the interval between presentation and synchronization explicitly and forces an
absolute recovery if it is interrupted. The next smoke gate must show no old
waiting-screen, cell, cursor, or disjoint-row content returning on subsequent
updates. Its `baseline_us` is expected to be nonzero because it measures this
single-bank synchronization copy.
