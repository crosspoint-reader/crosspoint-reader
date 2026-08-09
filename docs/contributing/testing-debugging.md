# Testing and Debugging

CrossPoint runs on real hardware, so debugging usually combines local build checks and on-device logs.

## Local checks

Make sure `clang-format` 21+ is installed and available in `PATH` before running the formatting step.
If needed, see [Getting Started](./getting-started.md).

```sh
./build-and-test.sh all
```

Or by hand:

```sh
./bin/clang-format-fix
pio check --fail-on-defect low --fail-on-defect medium --fail-on-defect high
pio run -e default
```

CI builds more environments than `default`; `.github/workflows/ci.yml` is the
authoritative list, and `./build-and-test.sh all` covers them in one invocation.

`./build-and-test.sh check` is the faster inner-loop version: every gate, no
build. Both cover the Rust workspace, which `pio` alone does not.

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

## Common troubleshooting references

- [User Guide troubleshooting section](../../USER_GUIDE.md#7-troubleshooting-issues--escaping-bootloop)
- [Webserver troubleshooting](../troubleshooting.md)
