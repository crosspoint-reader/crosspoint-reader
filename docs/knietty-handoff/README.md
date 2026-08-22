# knietty implementation handoff

This directory is the ordered execution plan for the next knietty development
cycle. Read [TTY_PROGRESS.md](../../TTY_PROGRESS.md) first: it is the authority
for what was actually built, flashed, and observed. These playbooks describe
future work and must never be treated as test results.

Milestones 01–05, the capture-driven Codex/parser pass, explicit session close,
and in-session display controls are physically validated. The host-only delayed
clean correction is complete. Milestone 06 TLS/pairing passed its principal
X4/macOS physical gate and is committed. Milestone 07 is complete with a
negative optimization result: zero-copy Mode 2 was visually incorrect, while
inactive-bank synchronization restored correctness but regressed full-frame
timing. Milestone 08 must retain the ordinary legacy baseline contract.

## Locked decisions

- Wi-Fi is the primary transport for the tested China-locked X4. Do not use
  `esptool`, PlatformIO upload, the Unlocker, or alter partitions, bootloader,
  secure boot, or eFuses.
- Safe 20 MHz remains the immutable display control. Adaptive 40 MHz remains an
  explicitly experimental comparison; adaptive 20 MHz is not a release target.
- Terminus 8 x 16 and 80 x 24 remain the default geometry.
- Protocol v3 is one bounded framed stream on the existing discovery/TCP
  service. It supports terminal and diagnostics modes and is wrapped by TLS
  1.3 in current firmware. v1/v2 remain only for explicitly selected plaintext
  compatibility with older trusted-LAN development images.
- Diagnostics requires physical approval. It exposes bounded named tests, never
  arbitrary SSD1677 commands, voltages, OTP writes, or overclock values.
- `PRESENTED` means BUSY fell. `READY` means all post-waveform baseline and
  power work completed. Both are required because their gap is a primary target.
- Rust is the sole host implementation; its v3 fixture and diagnostics schema
  remain frozen compatibility contracts.
- BLE keyboard work is backlog, not part of this sequence.

## Productive order

| Milestone | Outcome | Depends on | Blocks |
| --- | --- | --- | --- |
| [01](01-safe-state.md) | Terminal safely owns and restores display settings | Current checkpoint | All hardware experiments |
| [02](02-protocol-v3.md) | Tested framed v3 with v1/v2 compatibility | 01 | Diagnostics and Rust |
| [03](03-diagnostics-mode.md) | Approved host-driven tests and JSONL telemetry | 02 | Comparable measurements |
| [04](04-baseline-campaign.md) | Reproducible safe/adaptive baseline dataset | 03 | Driver optimization claims |
| [05](05-rust-host.md) | Rust host owns foreground bridge and diagnostics | 04 | Production TLS client |
| [06](06-tls-pairing.md) | Authenticated encrypted transport | 05 | Untrusted-LAN use |
| [07](07-mode2-ping-pong.md) | Volatile RAM-ping-pong A/B result | 04 | Baseline-copy decision |
| [08](08-async-pipeline.md) | Latest-frame-wins tail-chained refresh pipeline | 07 | Final latency tuning |
| [09](09-waveform-quality.md) | Measured legibility/speed waveform choice | 08 | Release display profile |
| [10](10-gate-viewport.md) | Independent 800 x 300 feasibility result | 09 | Optional speed viewport |
| [11](11-release-validation.md) | Linux/macOS/device evidence and release docs | 06, 09 | Release candidate |

Milestone 06 and 07 can proceed independently. Keep their commits isolated.
Milestone 08 must consume the measured result from 07; do not assume Mode 2
works.

The parser/TUI improvements discussed during Milestone 05 are saved in
[`TERMINAL_COMPATIBILITY.md`](TERMINAL_COMPATIBILITY.md) and are explicitly
scheduled after Rust host parity. They are not a reason to expand the current
Rust slice into firmware work.

## Rules for every milestone

1. Start from a clean, identified commit and record the parent in
   `TTY_PROGRESS.md`. Preserve unrelated user changes.
2. Inspect the source anchors in the milestone before editing. Re-check line
   numbers because this branch is evolving.
3. Make the smallest change that satisfies that milestone. Avoid CrossPoint
   refactors and keep driver experiments behind knietty/X4 feature gates.
4. Do not add repeated heap allocation to terminal, network, render, or display
   loops. Use fixed-size storage where possible; justify and null-check every
   allocation against the ESP32-C3 RAM ceiling.
5. Run the listed automated checks. Use `./bin/clang-format-fix -g`; do not call
   `clang-format` directly.
6. Hardware observations must identify artifact SHA-256, embedded commit,
   profile, settings, device, workload, and host OS. Never infer Linux parity
   from macOS or physical behavior from a successful build.
7. On a failed hardware gate, restore the known-good image through the normal
   CrossPoint OTA/SD routes and document the failure. Do not stack another
   experiment on top.
8. Update `TTY_PROGRESS.md`, commit the working checkpoint, and record its hash
   before advancing.

## Common verification commands

Run only the relevant firmware profile for a milestone. The current experience
profile is `knietty_adaptive_100ms_sustain_nosettle`; do not generate extra
safe artifacts unless a regression comparison is explicitly requested.

```sh
cd /Users/rodrigomtorres/git/knietty/crosspoint-reader

env PATH="$PWD/.venv/bin:$PATH" ./bin/clang-format-fix -g

./host-rs/scripts/check.sh

native_test_dir=$(mktemp -d /tmp/knietty-tests.XXXXXX)
cmake -S test -B "$native_test_dir" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_FLAGS='-isystem /Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk/usr/include/c++/v1'
cmake --build "$native_test_dir" -j4
ctest --test-dir "$native_test_dir" --output-on-failure

$HOME/.platformio/penv/bin/pio run -e knietty_safe
```

The explicit macOS SDK include is a recorded workaround, not a portable project
requirement. Linux agents should use their normal CMake toolchain and record it.

## Artifact and result layout

Firmware artifacts stay outside `.pio` under the workspace root, with the
commit/profile in the filename and a recorded SHA-256. Diagnostic outputs use:

```text
results/
  YYYY-MM-DD-device-profile-suite-run.jsonl
  YYYY-MM-DD-device-profile-suite-notes.md
```

Do not commit large captures by default. Commit the schema, small representative
fixtures, summaries, and the path/checksum of externally retained raw data.
