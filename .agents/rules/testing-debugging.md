## Testing and Debugging

### Build Commands

Use the [existing environment](environment.md#build-in-an-existing-environment).
Run the selected `pio run -e <environment>` target; `pio run` selects `default`.
Host suites use `pio run -t unit-tests` or their existing CMake/CTest commands.

### Compact build output

The main agent records full build output in a task-specific log, preserving the
process exit status. Report target, outcome, and log path; read bounded error
context on failure. Full compiler output stays in the log unless needed to
diagnose an error.

For an active build, use waits of 10-30 seconds rather than repeated one-second
polls. Inspect a short progress/error excerpt when needed and the final result
on completion. Keep user updates within 60 seconds. A sandbox/cache-lock failure
is an environment issue, not a completed firmware build.

### Code Quality

Use `./bin/clang-format-fix -g` on every host, including formatter diagnostics;
never invoke or probe raw `clang-format`. Run `pio check` when relevant.

### Testing and Verification Workflow

[firmware-handoff](../skills/firmware-handoff/SKILL.md) owns firmware review rounds,
final build ordering, and human acceptance. The main agent owns tests, builds,
and diagnostic code. Reviewers inspect source and recorded results only.

Run the smallest meaningful regression checks for the changed contract. Reuse
green results until a change or failure invalidates them. Test-only, diagnostic,
documentation, and host-only changes use relevant lighter checks unless they
alter firmware output/build. Check ignored outputs before staging, using the
[generated-file rules](generated-files-cache.md#files-generated-by-build-scripts).

Human device checks cover changed behavior, relevant orientations/remapping,
and resource/cache effects. Changed input, display, or layout paths cover all
four orientations: portrait, inverted, landscape CW, and landscape CCW.
C3 baseline target: more than 50 KB free and no leaks;
measure internal/PSRAM heaps and largest blocks separately across relevant repeated
book, dictionary, font, or WiFi transitions. Free bytes alone do not prove an
allocation will fit. For layout changes, back up progress and verify affected
cache regeneration, including TXT/Markdown and partial-build resume when relevant.

### Debugging Crashes

Use serial traces to establish the failure path before changing resources.

- Heap/OOM: measure `HalMemory` internal/PSRAM statistics and largest blocks
  before/after the operation; account for peak coexistence, allocation failure,
  and activity cleanup. See [resource rules](hardware-resources.md#the-resource-protocol).
- Stack: inspect call chains and `uxTaskGetStackHighWaterMark()`; adjust stack
  or buffer placement from evidence, not a copied task size.
- Task lifetime: follow [activity cleanup](ui-activities.md#activity-lifecycle-and-memory-management)
  and shutdown before destruction; inspect shared-state synchronization.
- Cache corruption: reset only affected data after backup and check the
  [format contract](generated-files-cache.md#cache-file-format-versioning).
- Watchdog: inspect blocking I/O and loops against the configured timeout;
  preserve task scheduling/yield behavior when changing the work.

### CI/CD Pipeline Awareness

Inspect current matrices under `.github/workflows/` before claiming board
coverage. Preserve pinned core/packages and patches. Release/RC board-specific
asset naming must agree with OTA discovery. Fix existing CI failures affecting
the change before handoff.

### Serial Monitor Options

Use `python3 scripts/debugging_monitor.py` for enhanced logs or
`pio device monitor` / the IDE monitor. Device access and measurements remain
within the authorized task.

### Live Debugging Patterns

Use `LOG_DBG` for measurements, `LOG_LEVEL=2` for debug logging, and
`logSerial.flush()` when a crash may lose buffered output. Task stack diagnostics
use `uxTaskGetStackHighWaterMark()`; heap diagnostics follow `HalMemory`, not
a combined free-byte reading. The enhanced monitor plots PSRAM separately when
those log values are supplied.
