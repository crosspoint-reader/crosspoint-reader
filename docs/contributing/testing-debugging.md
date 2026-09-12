# Testing and Debugging

CrossPoint runs on real hardware, so debugging usually combines local build checks and on-device logs.

## Local checks

Make sure `clang-format` 21+ is installed and available in `PATH` before running the formatting step.
If needed, see [Getting Started](./getting-started.md).

```sh
./bin/clang-format-fix
pio check --fail-on-defect low --fail-on-defect medium --fail-on-defect high
pio run
```

## Flash and monitor

Flash firmware:

```sh
pio run --target upload
```

Open serial monitor:

```sh
pio device monitor
```

Optional enhanced monitor:

```sh
python3 -m pip install pyserial colorama matplotlib
python3 scripts/debugging_monitor.py
```

## Useful bug report contents

- Firmware version and build environment
- Exact steps to reproduce
- Expected vs actual behavior
- Serial logs from boot through failure
- Whether issue reproduces after clearing `.crosspoint/` cache on SD card

## Simulator smoke checks

`Simulator smoke` builds the real firmware against a pinned revision of the
[official simulator](https://github.com/crosspoint-reader/crosspoint-simulator)
on GitHub-hosted Ubuntu, for both X3 and X4. Its standalone
`platformio.simulator.ini` does not change device or release environments.

The check generates a small original EPUB and an isolated virtual SD card. It
captures Home, resumes that book through the normal persisted-state path,
presses next/previous/next, and restarts the process to reopen the saved page.
It checks screenshot dimensions and content, round-trip page equality, logged
page transitions, and the actual persisted progress record. This is not yet a
file-browser selection test or a comprehensive visual-regression suite.

Each device uploads logs, PNG/BMP screenshots, a machine-readable result, the
generated EPUB, and exact firmware/harness/SDK revisions. Artifacts are retained
for 14 days. Preserve useful evidence in a PR before it expires. A missing
screenshot, crash, timeout, wrong page, or failed persistence check fails the job;
a successful launch alone is not a pass.

### Reproduce on Linux

```sh
git submodule update --init --recursive
sudo apt-get install libsdl2-dev libssl-dev xvfb xauth
python3 -m pip install platformio==6.1.19 PyYAML==6.0.3 Pillow==12.3.0
pio run -c platformio.simulator.ini -e simulator_x4 -j 2
LIBGL_ALWAYS_SOFTWARE=1 xvfb-run -a -s '-screen 0 1280x1024x24' \
  python3 scripts/simulator_smoke.py --device x4 \
  --binary .pio/build/simulator_x4/program --output build/smoke-x4
```

Use `simulator_x3` and `--device x3` for X3. The output directory must be new:
the runner deliberately refuses to overwrite previous results or a developer's
virtual SD card. The build configuration also supports macOS with SDL2 installed;
the screenshot assertions currently target the 1x Linux CI display, not Retina.

### Test a selected PR without changing its branch

Run the workflow manually and set `upstream_ref` to an upstream 40-character
commit SHA or `refs/pull/NUMBER/head`. Prefer a SHA for repeatable reviews. An
empty value tests the selected workflow branch. The workflow records the
resolved SHA and overlays only its native build configuration/helper; it does
not merge the harness into the selected PR. Run the same harness against the
PR's base and head, then compare the two device artifact sets. These are
separate smoke results, not an automated base-versus-head image verdict.

On a contribution fork, pushes to `ci/simulator-*` also run the demonstration,
without needing to open a draft upstream PR just to debug the test setup.

### Limits and safety

- The simulator recompiles firmware for the host; it does not execute the ESP32
  firmware binary or validate real heap limits, page-turn speed, power, SD timing,
  e-ink waveforms, ghosting, or flashing. Hardware testing remains necessary.
- The default simulator uses compatibility image decoders. This text fixture
  does not establish device image-decoder correctness.
- Input and capture schedules use fixed milliseconds with bounded process/job
  timeouts. Missing the expected state fails; the runner does not retry failures
  into passes. If CI timings prove unreliable, use simulator state-aware waits
  rather than weakening the assertions.
- PR builds execute untrusted code. Use disposable GitHub-hosted runners,
  read-only permissions, no secrets and no persisted checkout credentials.
  Never move this workflow to `pull_request_target` or a personal self-hosted
  runner. Do not use personal books, Wi-Fi credentials, or live network accounts
  in test fixtures. Simulator success does not approve a PR for merging.

## Common troubleshooting references

- [User Guide troubleshooting section](../../USER_GUIDE.md#7-troubleshooting-issues--escaping-bootloop)
- [Webserver troubleshooting](../troubleshooting.md)
