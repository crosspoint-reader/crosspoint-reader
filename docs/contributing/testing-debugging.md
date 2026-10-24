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

### Copy files over the cable

`scripts/device_control.py` also copies books and fonts to the SD card and back,
with no Wi-Fi.
This is the only cable path on the X3/X4, whose ESP32-C3 cannot act as a USB
drive:

```sh
python3 scripts/device_control.py push book.epub /Books
python3 scripts/device_control.py push "Some Folder" /Books     # a whole folder
python3 scripts/device_control.py install-font NotoSerif/       # a folder of one family's .cpfont files
python3 scripts/device_control.py ls /Books
python3 scripts/device_control.py pull "/Books/Some Book.epub" copy.epub
python3 scripts/device_control.py rm "/Books/Some Book.epub"
```

- `push`, `install-font`, `mkdir` and `rm` need the reader on Home, where no
  screen holds a book or font file open; `push` and `install-font` take
  `--home` to go there first. `ls` and `pull` work on any screen.
- A file is written to `<name>.part` and renamed only once every byte has
  arrived and its CRC matches, so an interrupted copy never leaves a truncated
  book or font. A push stops at the first file that already exists unless
  `--replace` is given.
- Writes stay in the visible part of the card, like WebDAV: no hidden folders,
  8.3 short names or `System Volume Information`. Fonts are the exception and go
  where the web Fonts page puts them; the family comes from the file names, as on
  that page (`--family` overrides it). A font of the family the reader is using
  is never replaced or removed.
- Each transfer holds the main loop, so the reader does not react to buttons
  until it ends. Ctrl-C stops a `push` or `pull` cleanly.

On an X4 with a Mac, `push` runs at about 400 KB/s and `pull` at about 600 KB/s.
The frame format is described in `src/util/SerialTransfer.h`.

## Useful bug report contents

- Firmware version and build environment
- Exact steps to reproduce
- Expected vs actual behavior
- Serial logs from boot through failure
- Whether issue reproduces after clearing `.crosspoint/` cache on SD card

## Common troubleshooting references

- [User Guide troubleshooting section](../../USER_GUIDE.md#7-troubleshooting-issues--escaping-bootloop)
- [Webserver troubleshooting](../troubleshooting.md)
