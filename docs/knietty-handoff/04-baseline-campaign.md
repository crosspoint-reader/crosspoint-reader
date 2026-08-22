# Milestone 04 — Controlled baseline campaign

## Objective

Produce the dataset that every Rust, TLS, scheduler, driver, and waveform change
will compare against. Do not modify display behavior during this milestone.

## Preconditions

- Milestones 01–03 passed on safe hardware.
- Exact safe and adaptive-40 artifacts are retained with SHA-256.
- The normal OTA recovery route and SD application update route remain proven.
- The X4 is charged, awake, at a recorded approximate room temperature, and on
  the same Wi-Fi network as the host.

## Suites

The campaign needs exactly two matched measurement firmware snapshots:

1. safe 20 MHz;
2. adaptive 40 MHz, explicitly labeled experimental and out of specification.

Official CrossPoint 1.5.0 remains the separately retained recovery image; it is
not a third measurement snapshot. Run each suite once per snapshot through the
normal SD application-update path:

```text
/Users/rodrigomtorres/git/knietty/knietty-M4-ae82c301-SAFE.bin
SHA-256 49aacc5b1c32da48e0a4e09bf9c76be40d3ce198559200b68c29b93ad3d451b3

/Users/rodrigomtorres/git/knietty/knietty-M4-ae82c301-ADAPT40-EXPERIMENTAL.bin
SHA-256 aff854e1f46218e966ffc77a0f5df041d135fb830316ca3ec15ace5afa6c070f
```

Both embed firmware checkpoint `ae82c301` and FreeInk revision `0ff05c6`.
Before each long campaign, run `smoke` once to verify approval, telemetry, and
cleanup for the newly flashed profile. Then run the retained datasets:

```sh
cargo run --release --manifest-path host-rs/Cargo.toml -- \
  diagnose --host auto --suite smoke --output results/PROFILE-smoke.jsonl

cargo run --release --manifest-path host-rs/Cargo.toml -- \
  diagnose --host auto --suite latency --output results/PROFILE-latency.jsonl

cargo run --release --manifest-path host-rs/Cargo.toml -- \
  diagnose --host auto --suite cadence --output results/PROFILE-cadence.jsonl

cargo run --release --manifest-path host-rs/Cargo.toml -- \
  diagnose --host auto --suite burst --output results/PROFILE-burst.jsonl
```

Replace `PROFILE` with the exact artifact profile. Each command establishes a
new physically approved diagnostic session. Host checkpoint `89b9b61d` and
earlier sent only one discovery probe per command, which could race the X4's
post-session cleanup in a shell loop. Use a host version containing the
re-probing fix after that checkpoint; it sends a new probe every 250 ms for the
configured discovery timeout.

Run each direction (white-to-black and black-to-white), normal and inverted:

1. `smoke`: one small update and cleanup.
2. `latency`: top/middle/bottom single cell, adjacent two cells, one full row,
   two disjoint rows, one-line scroll, 8 KiB boundary cases, and near-full frame.
3. `cadence`: tagged updates every 600, 400, 200, 100, 50, and 25 ms. Use a
   fixed count and cool/settle interval between groups.
4. `burst`: fixed 1, 2, 5, 10, 25, and 100-byte terminal-style bursts.
5. Optional `ghosting`: alternating named patterns for 10/25/50/100 updates,
   followed by a clean. Run only after ordinary suites are stable.

Use at least three repetitions for latency medians; keep raw samples rather
than reporting only averages. Randomize or alternate polarity order enough to
avoid always advantaging the first direction. Never compare runs with different
fading state, temperature band, battery/power condition, or workload silently.

## Required output

- Raw JSONL and SHA-256.
- A short Markdown note with device, host OS, firmware/FreeInk commit, artifact,
  profile, SPI rate, ambient estimate, power source, fading state, suite version,
  and subjective contrast/ghosting notes.
- Summary percentiles for host-to-`PRESENTED`, host-to-`READY`, queue, render,
  plane, BUSY, baseline, power-off, total, coalescing, and fallback reasons.
- Separate small-window and full/fallback results. Never aggregate them into one
  latency number.

For visible onset, record slow-motion video showing the host action and panel in
one frame when practical. Record the camera frame rate and do not label BUSY
completion as optical onset.

## Analysis questions

- How large is `READY - PRESENTED` and how much is baseline versus power-off?
- Does a second update wait on BUSY, baseline sync, scheduler state, or all three?
- At what cadence do updates begin coalescing?
- Does dirty size affect plane/render time while BUSY remains constant?
- Which exact condition causes each window fallback?
- Does 40 MHz improve only transfer, and by how much?

## Complete when

Safe 20 MHz and adaptive 40 MHz have comparable, repeatable latency/cadence data
with raw records retained. Add the summarized facts—without invented optical
claims—to `TTY_PROGRESS.md`. This dataset becomes immutable baseline version 1.

## Completion record

Completed on 2026-08-16 with firmware checkpoint `ae82c301` and FreeInk
`0ff05c6`. All eight raw captures and the analysis are retained in
[`results/baseline-v1.md`](../../results/baseline-v1.md). No optical onset,
ambient temperature, or capture-specific subjective quality result was
invented where it was not recorded.
