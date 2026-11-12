# Serial Test API

Development builds can be driven over the USB serial port. A host (a person at a terminal, a
script, or a coding agent) sends one text line per command and gets one reply line back: press a
button, open a screen or a book, ask what is on screen, wait until the device has finished
drawing, take a screenshot. This page lists the commands and shows how to use them from a script.

- [Why it exists](#why-it-exists)
- [Which builds have it](#which-builds-have-it)
- [Connecting](#connecting)
- [How a command works](#how-a-command-works)
- [Command reference](#command-reference)
- [Driving it from a script: `scripts/devctl.py`](#driving-it-from-a-script-scriptsdevctlpy)
- [Limits](#limits)
- [Adding a command](#adding-a-command)

## Why it exists

Most reader bugs only show on the device: a page that renders wrong, a popup that stays, a screen
that hangs after a certain sequence of presses. Checking a fix meant someone pressing buttons and
watching the screen. With these commands it can be a script:

- **Automated device tests.** Open a book, turn pages, check the chapter and page, compare a
  screenshot.
- **Reproducible bug reports.** Replay the presses from a report with the serial log captured
  alongside, until the bug shows.
- **Checks on hardware before a merge.** Run the same script against the old and the new build and
  compare the screens reached, the pages, the heap left.
- **Coding agents.** An agent can drive the device in a loop: screenshot, decide, press, check
  `STATE`, repeat. Every reply is a plain line that is easy to parse.

The commands take the routes the UI takes. A press enters the input manager's button state, where a
real key press enters, so the user's button mapping, long presses and double presses apply. A
screen is opened with the same `ActivityManager` call the menus use. So a test exercises the real
code paths, not a back door.

## Which builds have it

The development environments `default`, `x4pro`, `x4c`, `papermono`, `metalio_eink4`, `eego_a4`
and `picco` are built with `-DENABLE_SERIAL_CONTROL=1`. Release, release-candidate and `slim`
builds leave it off and keep only the original `CMD:SCREENSHOT` (`SCREENSHOT_START:<bytes>`, the
frame, `SCREENSHOT_END`); their code is unchanged. `sticky` leaves it off too: its USB-C port
reaches the chip's UART through a bridge, not the USB serial port the commands are read from.

To try it in another environment, make a local one in `platformio.local.ini` that keeps the
environment's own flags:

```ini
[env:my_release_test]
extends = env:gh_release
build_flags =
  ${env:gh_release.build_flags}
  -DENABLE_SERIAL_CONTROL=1
```

The full set costs about 7 KB of flash and 680 B of static RAM on the `default` (ESP32-C3) build:
the command line and reply buffers, the press queue and the idle check. Nothing stays allocated on
the heap.

## Connecting

Any serial terminal works (`pio device monitor`, `screen`, pyserial). The reader's log keeps
flowing on the same port; replies are lines among the log lines, recognised by their prefix.

- Opening the port with the default DTR/RTS lines does not reset the reader. RTS high with DTR low
  is esptool's reset request, which a plain close on Windows can send; `scripts/devctl.py` drops
  RTS first.
- Send a line end after opening the port: some hosts get no output before they have written
  something.
- Only one program can own the port: `scripts/debugging_monitor.py` and a script cannot run at the
  same time.
- In deep sleep the USB port goes away and the host cannot wake the device. Commands, queued
  presses and a running `IDLE` wait count as user activity, so a running script keeps the device
  awake and at full speed; between runs, set the sleep timeout to "Never" when the reader is used
  for testing.

## How a command works

A command is one line:

```text
CMD:<NAME>[:<argument>]
```

ending in `\n` (or `\r`, or both). Lines that do not start with `CMD:` are ignored, so a terminal's
stray keys do no harm. The main loop reads at most one command per pass and acts on it before it
reads the next. Every command answers with exactly one line (`SCREENSHOT` adds its frame), and the
reply's first word says what it is: `BTN_OK`, `ACT_ERROR:...`, `STATE:...`. An unknown command gets
`CMD_ERROR:unknown command <text>`, a line longer than 255 bytes `CMD_ERROR:line too long`. Such a
line does not fit the USB receive buffer, so its end is usually lost: the error then comes with the
next line end, and the command on that line is lost with it.

A reply is sent when the command has been *accepted*, not when its effect is on screen: `BTN_OK`
means the press is queued. Send `CMD:IDLE` after a command to wait until the device has handled it
and finished drawing.

## Command reference

### Buttons: `BTN`

```text
CMD:BTN:<NAME>[:LONG|:DOUBLE|:<ms>]
```

`<NAME>` is one of `BACK`, `CONFIRM`, `LEFT`, `RIGHT`, `UP`, `DOWN`, `POWER`: the input manager's
buttons. On most boards these are the physical keys. Where the SDK makes buttons out of combined
keys (Sticky's shared Confirm/Power key, Paper Mono's two keys), they are the buttons it makes, so
`BTN:POWER` is a power press, not a hold of the shared key. The user's front-button remapping and
side-button layout apply, as for a real key.

| Form | Press |
| --- | --- |
| `CMD:BTN:CONFIRM` | a short press, held 60 ms |
| `CMD:BTN:BACK:LONG` | held 1.1 s, past the 1 s long press most screens use |
| `CMD:BTN:POWER:DOUBLE` | two short presses in a row |
| `CMD:BTN:DOWN:1500` | held 1500 ms (1 to 10000) |

Replies: `BTN_OK`, or `BTN_ERROR:bad press <arg>`, `BTN_ERROR:unknown button <name>`,
`BTN_ERROR:busy` (the queue is full; it holds four presses), `BTN_ERROR:not supported on this
board`.

Presses are queued and played one after the other. Each is held until the input manager has
debounced it, then for its hold time, then released; the next starts once the release has gone
through. A press waits for its button to read up first: if a real key holds it down, the press
is given up after about 1 s (see `btn_dropped` and `IDLE`). A long `POWER` press puts the device
to sleep, as the key does.

### Screens: `ACT`

```text
CMD:ACT:HOME
CMD:ACT:READER:<path>      a book (or image) on the card, at its saved position
CMD:ACT:FILES[:<folder>]   the file browser, at the card's top folder or the one given
CMD:ACT:LIBRARY
CMD:ACT:SETTINGS
```

Replies: `ACT_OK`, or `ACT_ERROR:no such file <path>`, `ACT_ERROR:cannot open <argument>` (the
screen could not be created: a book the reader refuses, or no memory),
`ACT_ERROR:unknown screen <name>`. The screen changes after the reply; wait for it with `IDLE`.

### What is on screen: `STATE`

```text
CMD:STATE
STATE: activity=EpubReader reader=epub spine=3 page=4 pages=12 progress=17 orientation=portrait visit=7 heap=104232 maxblock=81908 minheap=61440 uptime=812345 stack="" title="Alice's Adventures"
```

Space-separated `key=value` fields. A value that is empty or holds a space, `"`, `\` or `=` is in
double quotes, with `"` and `\` escaped by a backslash.

A reply line holds at most 319 bytes. `stack` and `title` come last and are cut to the room left,
before a UTF-8 character and with their quotes kept, so the other fields always arrive whole; a
cut field is named in a last field, `clipped=title` (or `stack`, or `stack,title`). A line that
would still not fit is answered `STATE_ERROR:reply too long` instead.

| Field | Meaning |
| --- | --- |
| `activity` | the screen in front (`Home`, `FileBrowser`, `EpubReader`, `SettingsList`, ...) |
| `stack` | screens under it, bottom first, comma-separated |
| `reader` `title` `spine` `page` `pages` `progress` | only in a reader: `epub`, `txt` or `xtc`; the title, cut at 63 bytes; `spine` (EPUB only) and `page` count from 1; `pages` is the chapter's page count for EPUB and the book's for TXT and XTC; `progress` is the book's percentage |
| `orientation` | `portrait`, `landscape_cw`, `portrait_inverted` or `landscape_ccw` |
| `visit` | counts screen changes: a new value means a new screen, even one with the same name |
| `heap` `maxblock` `minheap` | internal heap: free bytes, largest free block, lowest free since boot |
| `uptime` | milliseconds since boot |
| `btn_dropped` | only when not 0: presses the input manager never took in (see [Limits](#limits)) |
| `clipped` | only when `stack` or `title` was cut: their names, comma-separated |

New fields may be added; a host should ignore keys it does not know.

### Waiting: `IDLE`

```text
CMD:IDLE[:<timeout_ms>]
```

Waits until the device is idle and answers `IDLE:<elapsed_ms>`, or `IDLE_TIMEOUT:<reason>` after
the timeout (default 10000 ms, at most 600000). A timeout that is not a positive number gets
`IDLE_ERROR:bad timeout <arg>`.

Idle means: no screen change queued, no frame requested, being drawn or waited for, and the last
`BTN` press handled and the frame it caused drawn (or 1 s passed without one, for a press that
changed nothing). All of that for 250 ms in a row.

The `<reason>` of a timeout names the last thing that kept the device busy (`transition`, `render`,
`input`), or `settling` if nothing did.

If a `BTN` press was given up since the last `IDLE` answer (the input manager never took it in),
the wait ends with `IDLE_ERROR:press dropped` instead: the screen did not get that press.

One wait runs at a time: an `IDLE` sent while another is waiting gets `IDLE_ERROR:busy` at once,
and the first wait goes on to its own answer. A wait outlives the script that sent it: after a run
stopped during a long `IDLE`, a new `IDLE` is busy until that wait's timeout.

### Screenshots: `SCREENSHOT`

```text
CMD:SCREENSHOT
SCREENSHOT_START:<bytes>:<width>:<height>
<the frame: <bytes> raw bytes>
SCREENSHOT_END
```

The frame buffer as the panel takes it: 1 bit per pixel, rows of `<width>` pixels, most significant
bit first, 1 = white. It is in the panel's native orientation, which `STATE` calls `landscape_ccw`;
in another orientation, turn it by what `STATE` reports to see it upright. The size comes first, as
in release builds, so `scripts/debugging_monitor.py` reads both. Errors:
`SCREENSHOT_ERROR:framebuffer lent` (a chapter build has borrowed it),
`SCREENSHOT_ERROR:transfer stalled` (the host stopped reading).

### `HELP`

`HELP: BTN ACT STATE IDLE SCREENSHOT HELP`: the commands this build has.

## Driving it from a script: `scripts/devctl.py`

`scripts/devctl.py` (pyserial, and Pillow for screenshots; both are in `scripts/requirements.txt`)
runs steps in order and stops at the first that fails. Its port defaults to `/dev/ttyACM0`; on
macOS and Windows pass `--port`.

```sh
python3 scripts/devctl.py --port /dev/ttyACM0 --log run.log \
  cmd:ACT:READER:/books/alice.epub idle:30000 state 'check:activity=EpubReader' \
  btn:DOWN btn:DOWN state 'check:page=3' shot:page3.png
```

| Step | Does |
| --- | --- |
| `cmd:<TEXT>` | sends `CMD:<TEXT>`, prints the reply, fails on an `_ERROR`/`_TIMEOUT` reply or none |
| `btn:<NAME>[:<how>]` | `CMD:BTN:...`, then waits for `IDLE` |
| `state` | sends `CMD:STATE` and keeps its fields |
| `check:<key>=<regex>` | the last `state` step's field must match the regex as a whole |
| `idle[:<ms>]` | `CMD:IDLE`; an `IDLE_TIMEOUT` or `IDLE_ERROR` fails the step |
| `shot:<file.png>` | `CMD:SCREENSHOT`, turned upright for the orientation `STATE` reports (`--rotate` overrides) |
| `wait:<s>` / `expect:<regex>:<s>` | read the log for a while / until a line matches |

`--script steps.txt` reads one step per line. `--log` keeps every device line with a timestamp, so
a failed run comes with its serial log.

A step takes only a reply to its own command: the command's name followed by `_` or `:`, or a
`CMD_ERROR`. A command sent after a reply timed out (with `--keep-going`, which runs every step even
after one failed) first waits until no line has come for 1 s (10 s at most), so a late reply cannot
pass for the next one. If the port goes away while a command is written (a reboot), the tool
reopens it and fails that step as `DELIVERY UNCERTAIN`: the command is not sent again, as the
device may have run it.

## Limits

- A press enters through the SDK's board-button hook (`InputManager::setButtonHook`). On a board
  whose own key uses that hook (LilyGo T5S3), `BTN` answers `BTN_ERROR:not supported on this
  board`. Where the input code does not read the hook (M5Stack PaperColor), `BTN` answers `BTN_OK`
  but the press is given up after 1 s and nothing happens; the next `IDLE` answers
  `IDLE_ERROR:press dropped` and `STATE` shows `btn_dropped`.
- Touch is not covered; the touch boards' buttons are.
- `IDLE` looks at the frame buffer, not the panel: on screens that refresh the panel in the
  background (the reader's overlays), the waveform may still be running when `IDLE` answers.
- No command is read while the USB Drive mode owns the card, or while a screen runs a transfer that
  holds the main loop (a download, an upload, a catalog fetch). A `BTN:BACK` sent to cancel one
  arrives after it.

## Adding a command

1. Write a handler `void cmdName(std::string_view arg)` in `src/SerialTestApi.cpp` and add it to
   `COMMANDS`. Reply exactly once, with a prefix of its own (`NAME_OK`, `NAME_ERROR:<why>`).
2. Parse arguments in `src/util/SerialCommandGrammar.*` and test them in `test/serial_test_api/`.
3. Take the route the UI takes (a `goTo...()` call, a press) rather than reaching into a screen.
4. Keep it inside `#if ENABLE_SERIAL_CONTROL`, and document it here.
