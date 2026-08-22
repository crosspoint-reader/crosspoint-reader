# knietty display baseline v1

Captured on 2026-08-16 from the available China-locked XTEINK X4. This is the
immutable comparison baseline for later scheduler, SSD1677, TLS, and host-port
work. Raw JSONL is retained beside this note.

## Provenance and conditions

| Profile | Firmware | FreeInk | SPI | Session flags | Battery | RSSI |
| --- | --- | --- | ---: | ---: | ---: | ---: |
| Safe | `1.5.0-dev-feature/knietty-terminal-ae82c301` | `0ff05c6` | 20 MHz | `0x00` | 100% | -66 to -53 dBm |
| Adaptive-40 | `1.5.0-dev-feature/knietty-terminal-ae82c301` | `0ff05c6` | 40 MHz | `0x0c` | 100% | -66 to -61 dBm |

Both profiles reported 800 x 480, 80 x 24, font `1`, orientation `3`, fading
fix off, and Darwin 25.5.0 as the host. Safe minimum free heap across the four
sessions was 60,800 bytes; adaptive-40 minimum free heap was 60,916 bytes.
Ambient temperature and whether the 100% device was externally powered were
not recorded, so do not silently compare this dataset to a different
temperature or power condition. In the post-capture debrief, the user reported
that adaptive-40 sometimes produced no obvious visible reaction, retained some
ghosting, and had better contrast than the previous adaptive attempt but still
not enough. This was a qualitative observation without fixed-camera images or
a numeric score.

## Raw captures

| File | SHA-256 | Records | READY activations |
| --- | --- | ---: | ---: |
| `safe20-ae82c301-smoke.jsonl` | `a58af43f9f9728751fee9ff5c6f0acb69af32cb98617a508ec41251decb1ef01` | 41 | 13 |
| `safe20-ae82c301-latency.jsonl` | `77c936128cf39eb6dda95652aac0123512e1533bf9707683c29576ddfcd1675e` | 230 | 76 |
| `safe20-ae82c301-cadence.jsonl` | `c50567900211730cdd2b73f83355aa91c5e4f764e546b79d1017bcb75625aa42` | 168 | 45 |
| `safe20-ae82c301-burst.jsonl` | `4f60792408d00a2c67893f3c1551201405ae0dc1f09612f8c2fa94cd20c73f3f` | 86 | 28 |
| `adaptive40-ae82c301-smoke.jsonl` | `ecc12cb84a65dc0b11b57da527539a068e4b9b23793921b0de8b27f6b30a3cc0` | 41 | 13 |
| `adaptive40-ae82c301-latency.jsonl` | `db15d45cbdcec4cbfc1edbd1f12a99da8826b95bf1507257b2816e33293061c1` | 230 | 76 |
| `adaptive40-ae82c301-cadence.jsonl` | `4c3f6f9e592f964a74174833e33cdc5e4328c12036298d1ced5dc9511efc6958` | 228 | 75 |
| `adaptive40-ae82c301-burst.jsonl` | `ad8401e9a9ace5d22a820d4856d99a845e24ce88aea9bfb3715e0ec4aaa4ea6c` | 86 | 28 |

Every file contains one matching session record, no rejected command, ordered
PRESENTED/READY pairs, and a final accepted STOP. Smoke, setup reset/polarity,
restore, and final clean operations are excluded from the interactive summary.

## Interactive workload summary

Values are milliseconds and shown as median / p95. Window/fallback results are
kept separate. `Requests` includes commands merged into an activation. Display
`total` excludes queue and model rendering; device RX-to-READY includes them.

| Profile/path | Activations | Requests | Queue | Render | Plane | BUSY waveform | Baseline | Display total | RX-to-READY |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Safe window | 90 | 121 | 2.500 / 492.211 | 4.127 / 19.869 | 0.645 / 6.684 | 503.223 / 504.233 | 0.456 / 11.796 | 505.213 / 522.234 | 524.020 / 1008.761 |
| Safe fallback | 47 | 47 | 2.736 / 4.569 | 20.375 / 109.621 | 35.780 / 39.046 | 503.401 / 503.894 | 70.034 / 73.368 | 609.649 / 614.306 | 632.198 / 723.503 |
| Adaptive-40 window | 120 | 121 | 2.283 / 3.310 | 4.417 / 19.605 | 0.397 / 3.678 | 5.302 / 6.264 | 0.354 / 4.418 | 7.341 / 14.871 | 21.883 / 34.470 |
| Adaptive-40 fallback | 47 | 47 | 2.934 / 4.555 | 20.779 / 106.812 | 20.212 / 23.056 | 5.505 / 6.143 | 38.889 / 41.676 | 65.427 / 67.956 | 88.661 / 177.056 |

For sequential latency and burst commands, host send-to-record-delivery was:

| Profile/path | PRESENTED record median / p95 | READY record median / p95 |
| --- | ---: | ---: |
| Safe window | 613.387 / 718.765 ms | 613.733 / 719.192 ms |
| Safe fallback | 718.647 / 871.683 ms | 719.007 / 872.199 ms |
| Adaptive-40 window | 34.494 / 97.928 ms | 37.255 / 104.940 ms |
| Adaptive-40 fallback | 112.321 / 204.672 ms | 113.553 / 205.051 ms |

These are record-delivery latencies, not optical onset. The blocking firmware
sends both event records after READY, so their host receipt times cannot
independently measure when pigment first became visible. Cadence deliberately
sends its whole group before reading responses, so host receive timing from
that suite is excluded from this table. Device timestamps provide the phase
split without claiming optical presentation.

## Cadence and coalescing

Each row requested 12 changes: six in each polarity. RX-to-READY is measured
from the first request represented by an activation.

| Profile | Interval | Activations | Requests merged | Largest batch | RX-to-READY median |
| --- | ---: | ---: | ---: | ---: | ---: |
| Safe | 600 ms | 12 | 0 | 1 | 523.590 ms |
| Safe | 400 ms | 10 | 2 | 2 | 753.312 ms |
| Safe | 200 ms | 6 | 6 | 3 | 838.854 ms |
| Safe | 100 ms | 5 | 7 | 5 | 961.899 ms |
| Safe | 50 ms | 4 | 8 | 5 | 760.965 ms |
| Safe | 25 ms | 4 | 8 | 5 | 770.613 ms |
| Adaptive-40 | 600 ms | 12 | 0 | 1 | 25.561 ms |
| Adaptive-40 | 400 ms | 12 | 0 | 1 | 18.508 ms |
| Adaptive-40 | 200 ms | 12 | 0 | 1 | 18.499 ms |
| Adaptive-40 | 100 ms | 12 | 0 | 1 | 18.264 ms |
| Adaptive-40 | 50 ms | 12 | 0 | 1 | 19.224 ms |
| Adaptive-40 | 25 ms | 11 | 1 | 2 | 14.517 ms |

Safe first begins coalescing between the tested 600 and 400 ms cadences.
Adaptive-40 preserved one activation per request down through 50 ms and merged
one pair at 25 ms.

## Burst scaling

Median display-call total, excluding queue and render:

| Changed cells | Safe | Adaptive-40 |
| ---: | ---: | ---: |
| 1 | 505.277 ms | 8.170 ms |
| 2 | 504.997 ms | 8.510 ms |
| 5 | 504.413 ms | 7.951 ms |
| 10 | 505.297 ms | 8.546 ms |
| 25 | 508.051 ms | 7.293 ms |
| 100 | 514.107 ms | 12.335 ms |

## Findings

1. The safe profile's “second frame” delay is real serialized panel work, not a
   mysterious idle timer. Every safe window activation spends about 503 ms in
   the SSD1677 BUSY waveform. A following request therefore waits or joins the
   fixed pending aggregate.
2. Safe windowing removes most transfer and baseline cost but cannot shorten
   that stock waveform. Falling back to the full framebuffer raises median
   display time from 505.2 to 609.6 ms.
3. Adaptive-40's short custom waveform reduces median window BUSY to 5.3 ms and
   display total to 7.3 ms. This explains the fast first paint and its ability
   to keep up at 50 ms cadence; its short drive is also consistent with the
   observed low contrast, ghosting, and occasional lack of an obvious visible
   transition.
4. Large adaptive fallback transfers remain material: median plane and baseline
   work are 20.2 and 38.9 ms. At safe 20 MHz they are 35.8 and 70.0 ms. This
   two-image comparison changes waveform and SPI rate together, so it cannot
   attribute the entire difference to 40 MHz alone.
5. Neither profile reported display power-off work during interactive events.
   The earlier sunlight-fix power-off penalty was absent.

No 30/60 Hz legibility claim follows from this dataset. Adaptive-40 completes
electrical activations quickly, but optical contrast/ghosting remains a separate
quality constraint that requires scored images or controlled camera capture.

For the first quality/speed follow-up, prefer 20 MHz SPI and change only the
volatile LUT duration from one 5 ms directional frame toward approximately
100 ms. Baseline v1's small-window plane medians differ by only 0.248 ms between
safe-20 and adaptive-40, so the 40 MHz out-of-spec bus rate offers negligible
typing benefit beside a 100 ms waveform and would confound the waveform result.
