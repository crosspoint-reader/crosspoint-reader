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

## Drive the reader from a script

With the reader on USB, `scripts/device_control.py` presses buttons, opens
books, restarts and takes screenshots through the firmware's serial console
(`src/SerialConsole.cpp`), and can keep everything the reader logs in a file.
That makes on-device measurements repeatable without anyone at the reader:

```sh
python3 scripts/device_control.py state
python3 scripts/device_control.py open --cold "/Books/Some Book.epub"
python3 scripts/device_control.py screenshot screen.png
python3 scripts/device_control.py --log run.log run experiment.txt
```

A run script lists one command per line, in the same form, sharing one
connection and one log across restarts:

```text
restart --wait
open --cold "/Books/Some Book.epub"
wait "Page 100 processed" --timeout 180
state
```

`open --cold` drops the book's laid-out chapters first, so the reader lays them
out again; `wait` matches log lines that arrive after it starts. Run
`python3 scripts/device_control.py --help` for every command. The raw protocol
is one line per command, `CMD:<NAME> ...`, answered with `CMD_OK <NAME> ...` or
`CMD_ERR <NAME> <reason>`; the grammar is in `src/util/SerialCommand.h`.

## Useful bug report contents

- Firmware version and build environment
- Exact steps to reproduce
- Expected vs actual behavior
- Serial logs from boot through failure
- Whether issue reproduces after clearing `.crosspoint/` cache on SD card

## Common troubleshooting references

- [User Guide troubleshooting section](../../USER_GUIDE.md#7-troubleshooting-issues--escaping-bootloop)
- [Webserver troubleshooting](../troubleshooting.md)
