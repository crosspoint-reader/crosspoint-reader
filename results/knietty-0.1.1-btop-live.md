# knietty-0.1.1 live btop pipeline sample

Date: 2026-08-21

Host: macOS, installed Rust host `knietty 0.1.0`

Device: XTEINK X4, `knietty-0.1.1`, 80 x 24 Terminus, 20 MHz SPI,
W100 terminal-interactive waveform, adaptive refresh and balanced sustain on,
auto-settle/fading-fix/Mode-2/overclock off, RSSI -52 dBm.

The device was already connected with btop running. Read-only
`knietty display metrics --json` snapshots were taken at start, approximately
30 seconds, and approximately 60 seconds.

## Start

- PTY/device bytes: 1,040,583 / 1,040,583
- Host/device burst ends: 113 / 113
- Device burst snapshots/timeouts/async tails: 112 / 0 / 13
- Updates/window/fallback: 115 / 112 / 3
- Average/min/max total: 313.476 / 127.594 / 814.426 ms
- Heap current/minimum: 53,308 / 24,880 bytes

## Approximately 30 seconds

- PTY/device bytes: 1,872,883 / 1,865,034
- Host/device burst ends: 190 / 190
- Device burst snapshots/timeouts/async tails: 189 / 2 / 15
- Updates/window/fallback: 194 / 191 / 3
- Average total: 313.892 ms
- Heap current/minimum: 33,252 / 24,880 bytes

The temporary 7,849-byte host/device skew was in-flight traffic, not loss; it
fully converged in the final snapshot.

## Approximately 60 seconds

- PTY/device bytes: 2,608,836 / 2,608,836
- Host/device burst ends: 260 / 260
- Device burst snapshots/timeouts/async tails: 258 / 8 / 19
- Updates/window/fallback: 268 / 265 / 3
- Average/min/max total: 314.737 / 127.594 / 814.426 ms
- Heap current/minimum: 53,344 / 24,880 bytes
- Last queue/waveform/render/transfer/total: 155.000 / 120.241 /
  12.629 / 27.296 / 315.166 ms
- Last region: 723 x 432 / 39,042 bytes

## Deltas over approximately 60 seconds

- PTY bytes and device RX: +1,568,253 each; final totals are byte-perfect.
- Host/device burst ends: +147 each, approximately 2.45/s.
- Updates: +153, approximately 2.55/s; all 153 were windowed and there were
  zero new full-frame fallbacks.
- Burst snapshots: +146.
- Burst timeouts: +8, approximately 0.13/s.
- Async-tail updates: +6, approximately 0.10/s.
- Minimum heap did not fall and current heap returned to its starting band.

Compared with the `knietty-0.1.0` 95-second sample (+50 timeouts and +42 async
tails), the timestamp-ordering fix reduces both rates by roughly three quarters
without changing the 80 ms fail-safe. It does not eliminate every timeout: the
remaining eight are consistent with genuinely late boundary delivery under
large repaints and should be treated separately from the fixed unsigned-age
underflow. Transport integrity, cadence, windowing, and heap gates pass.

No SD-update-progress claim is made from this run. The updater performing the
installation was still the old 0.1.0 updater. Saved-Wi-Fi behavior also needs an
explicit enter-without-confirm observation before it is marked physically
validated.
