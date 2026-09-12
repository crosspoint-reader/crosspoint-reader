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

Add `--serve` to share the monitor's serial connection with automation over a
localhost API. Add `--headless` to run without the memory graph or interactive
stdin. With the server running, control it from another terminal:

```sh
python3 scripts/device_control.py state
python3 scripts/device_control.py press page-forward --expect-page-change
python3 scripts/device_control.py screenshot page.pbm
```

See [Device control and test automation](../device-control.md) for commands,
buttons, verified activities, command files, and test fixtures.
See [Debugging monitor server](../debugging-monitor.md) for the HTTP API,
exclusive control, screenshot transport, and releasing the port for flashing.

## Useful bug report contents

- Firmware version and build environment
- Exact steps to reproduce
- Expected vs actual behavior
- Serial logs from boot through failure
- Whether issue reproduces after clearing `.crosspoint/` cache on SD card

## Common troubleshooting references

- [User Guide troubleshooting section](../../USER_GUIDE.md#7-troubleshooting-issues--escaping-bootloop)
- [Webserver troubleshooting](../troubleshooting.md)
