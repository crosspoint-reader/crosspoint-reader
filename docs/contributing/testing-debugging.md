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

## Simulator

Use the normal host build when you just need desktop iteration:

```sh
pio run -e simulator -t run_simulator
```

Use the 32-bit host build when you want the simulator closer to ESP32-C3 memory layout and pointer width:

```sh
pio run -e simulator_i386 -t run_simulator
python3 scripts/run_sim_benchmark.py
```

The simulator always runs with a fixed heap arena. Override the default `380 KiB` arena with
`CROSSPOINT_SIM_HEAP_BYTES=<bytes>` when you want deterministic exhaustion or fragmentation behavior.

On Linux, the `simulator_i386` env requires multilib support plus 32-bit SDL/OpenSSL development packages.

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
