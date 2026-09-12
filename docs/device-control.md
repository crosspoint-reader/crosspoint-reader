# Device control and test automation

Use `scripts/device_control.py` to press buttons, check reader position and UI
readiness, capture screenshots, and run repeatable command files on a connected
device. Commands travel through the debugging monitor, which owns the USB serial
connection.

## Start a session

Run from the repository root. The monitor requires `pyserial`; firmware UI
control requires a build with `ENABLE_SERIAL_LOG`. Start the server in one
terminal:

```sh
python3 scripts/debugging_monitor.py --serve --headless
```

Omit `--headless` to keep the interactive monitor and memory graph, which also
requires `matplotlib`. See [monitor setup](debugging-monitor.md#start-the-monitor)
for serial-port selection, dependencies, logging, and server options. Keep the
monitor running while using the CLI from another terminal.

## Command-line control

With the monitor server running, use `scripts/device_control.py` from a second
terminal. The CLI uses the Python standard library; PNG output additionally
requires Pillow. Use `--help` on any command for its options.

```sh
python3 scripts/device_control.py status
python3 scripts/device_control.py info
python3 scripts/device_control.py state
python3 scripts/device_control.py press confirm --expect-activity EpubReader
python3 scripts/device_control.py press page-forward --repeat 3 --expect-page-change
python3 scripts/device_control.py screenshot page.pbm
python3 scripts/device_control.py logs --follow --filter 'Page render'
```

| Command | Behavior |
|---|---|
| `status` | Show monitor connection, lease status, and saved log directory |
| `info` | Discover firmware protocol, boot ID, request ID, and hold limits |
| `state` | Read activity, reader position, and readiness |
| `press BUTTON` | Send a logical button from the [button list below](#buttons-and-press-options); wait for ready UI before and after input |
| `wait` | Wait for ready UI; optionally require `--activity`, `--spine`, and/or `--page` |
| `cancel` | Cancel pending or held synthetic input |
| `screenshot PATH` | Wait for ready UI and save `.pbm`, `.png`, or `.raw` |
| `send 'COMMAND'` | Write one ASCII serial command; execution is unconfirmed |
| `logs` | Read retained serial logs; `--follow` streams new logs until Ctrl-C |
| `serial release` | Close the monitor's serial handle and suspend reconnects |
| `serial reconnect` | Reopen serial and wait for a connected host port |
| `run FILE` | Execute one command per line under a single control lease; `-` reads stdin |

### Buttons and press options

`BUTTON` must be one of these names. These are logical controls, not GPIO numbers.
The current activity decides what a press does; a press can be ignored when that
activity does not use the control.

| Button | Meaning |
|---|---|
| `back` | Back/cancel using the user's configured front Back button |
| `confirm` | Select/confirm using the user's configured front Confirm button |
| `left` | User-configured front Left button |
| `right` | User-configured front Right button |
| `up` | Fixed side Up button |
| `down` | Fixed side Down button |
| `nav-next` | Next navigation item, following the active orientation policy |
| `nav-previous` | Previous navigation item, following the active orientation policy |
| `page-forward` | Reader's next-page control, following side-button layout and orientation settings |
| `page-back` | Reader's previous-page control, following side-button layout and orientation settings |

Names accept either case and hyphens or underscores: `page-forward` and
`PAGE_FORWARD` are equivalent. Prefer `nav-next`/`nav-previous` for menu navigation
and `page-forward`/`page-back` for reading. The serial control resolves each name
to one physical button and keeps that mapping until release. Disabled mappings
and mappings to Power are rejected. Power, touch, swipe, and screen-direction
buttons are not exposed by this CLI.

| `press` option | Meaning and default |
|---|---|
| `--hold-ms N` | Requested hold duration, 20–2000 ms; default 80. A busy firmware loop can release later; `INPUT_DONE.held_ms` reports the actual hold. |
| `--repeat N` | Perform 1–10000 presses; default 1. Finish each iteration before sending the next. |
| `--wait ready` | Default: wait for input delivery and then UI readiness. Readiness alone does not prove navigation occurred. |
| `--wait input` | Return after input delivery (`INPUT_DONE`). Still wait for readiness before each press. |
| `--expect-page-change` | After every press, require a changed spine/page within the same reader activity. Requires an open reader. |
| `--expect-activity NAME` | After every press, require an exact [activity name](#activity-names), such as `Home` or `EpubReader`. |

Expectations require `--wait ready`. Use `state` to discover the current activity
name and reader position. For example:

```sh
python3 scripts/device_control.py press nav-next
python3 scripts/device_control.py press confirm --hold-ms 500 --expect-activity EpubReader
python3 scripts/device_control.py press page-back --repeat 2 --expect-page-change
```

### Activity names

`press --expect-activity NAME` and `wait --activity NAME` use the same
**case-sensitive** names. Use `EpubReader`, not `EpubReaderActivity` or
`epub-reader`. The CLI compares the string against `STATE.activity`; it does not
navigate to that activity or restrict names to a hardcoded list.

The following 40 targets were verified on an unlocked X3 through the CLI: each had
an available, non-busy `STATE` and a captured screen. The list includes only these
verified usable targets. Testing covered entry, readiness, capture, and selected
navigation/cancellation paths, not every function within each screen.

| Exact activity name | Screen and tested conditions |
|---|---|
| `Home` | Home screen; verified as the final destination after reader/settings navigation. |
| `FileBrowser` | Local files and directories; also used by the SD firmware picker. |
| `RecentBooks` | Recent-book list and removal-prompt navigation. |
| `EpubReader` | EPUB reading; page changes verified using spine/page positions. |
| `TxtReader` | TXT/Markdown reading, page turns, saved-position reopen, and empty-file handling. |
| `XtcReader` | Three distinct XTC pages, page turns, and saved-position reopen. |
| `XtcReaderChapterSelection` | XTC chapter picker; chapter selection and jump verified. |
| `BmpViewer` | Matching standalone BMP/PNG patterns and sibling navigation. |
| `EpubReaderMenu` | EPUB reader menu; available rows depend on the book and settings. |
| `EpubReaderChapterSelection` | Chapter list; chapter jumps and cancellation back to the menu. |
| `EpubReaderPercentSelection` | Percentage picker; progress change and cancellation verified. |
| `EpubReaderBookmarks` | Bookmark creation, selection, exact-position restore, and removal. |
| `EpubReaderFootnotes` | First-page footnote selection, exact destination, and Back to the source page. |
| `DictionaryWordSelect` | Word selection from a rendered EPUB page with a dictionary configured. |
| `DictionaryDefinition` | Known fixture definition verified; three lookup/return cycles. |
| `QrDisplay` | Page-text QR display; requires nonempty EPUB section text. |
| `TextSettings` | Font selection, size change/reflow, reopen, and restoration. |
| `Settings` | Main settings and its four category tabs. |
| `StatusBarSettings` | Status-bar options; entry and navigation. |
| `ClockOffset` | UTC-offset picker on the X3's clock hardware; value left unchanged. |
| `ClockSync` | Saved-network connection and successful clock synchronization. |
| `FontDownload` | Online font catalog and Korean font-group browsing; no installation. |
| `ButtonRemap` | Front-button assignment screen; side Down cancels. Back assigns a slot here. |
| `KeyboardLayouts` | Keyboard-layout settings; entry and cancellation. |
| `LanguageSelect` | Language picker; entry and cancellation. |
| `SleepTimeoutInterval` | Sleep-timeout picker; entry and cancellation. |
| `OpdsServerList` | OPDS server list; verified with no configured servers. |
| `OpdsSettings` | New-server editor; fields viewed without saving a server. |
| `OpdsBookBrowser` | Local catalog, hash-verified download, deliberate HTTP 404, and retry recovery. |
| `KeyboardEntry` | Text-entry keyboard; verified from an OPDS name field without submitting text. |
| `KOReaderSettings` | Sync settings; view/cancel only, without authentication or synchronization. |
| `ClearCache` | Warning screen only; cache clearing was not executed. |
| `Confirmation` | Recent-book removal dialog; cancellation and removal of a test-created recent entry. |
| `NetworkModeSelection` | File-transfer mode picker. |
| `WifiSelection` | Wi-Fi selection and saved-network connection; ready UI alone does not prove connection. |
| `CrossPointWebServer` | Hotspot and joined-network file transfer; uploaded fixtures verified by readback hashes. |
| `CalibreConnect` | Connected, listening screen; no Calibre transfer tested. |
| `OtaUpdate` | Update check and no-update result; no firmware installation. |
| `SdFirmwareUpdate` | Deliberately invalid file rejected; ready error screen and exit verified. |
| `Crash` | Already displayed crash report; inspected in a new CLI session. |

Both activity options require ready UI, not merely an activity's presence in a
log. Unverified, transient, and prerequisite-blocked activities are omitted from
this table; omission does not mean they cannot work on another setup. `unknown`
is an unavailable-snapshot placeholder, not a usable target.

A Back press can affect two screens: some consume button-down and the next screen
consumes button-release. On the tested build, Back from `Crash` passed through
Home and resumed the book; Back from `ClockOffset` also exited its parent status-bar
screen. Use the observed final activity for expectations. An observed crash or
reboot stops an active run even when its expected activity is `Crash`. After an
intentional hotspot teardown, start a new invocation to inspect the new boot.

Read the current name with `python3 scripts/device_control.py state`. For example,
when Confirm opens the highlighted book on Home:

```sh
python3 scripts/device_control.py press confirm --expect-activity EpubReader
python3 scripts/device_control.py wait --activity EpubReader
```

### Waiting, capture, and raw commands

`wait` requires ready UI and all supplied conditions. `--activity NAME` matches
the exact activity string reported by `state`; `--spine N` selects a zero-based
EPUB spine index; `--page N` selects a one-based page within the current section.
It observes state without navigating or requesting a redraw:

```sh
python3 scripts/device_control.py wait --activity EpubReader --spine 4 --page 2
```

`send 'COMMAND'` accepts one quoted ASCII line, with an optional `CMD:` prefix,
at most 127 bytes including that prefix. For example, `send 'INFO 0'` writes a
protocol discovery command. It returns after the serial write; use `logs` to
inspect the reply. Prefer the typed commands above for automatic request IDs and
completion checks. See [UI control](#ui-control) for the firmware command syntax.

`PATH` is an output filename ending in `.pbm`, `.png`, or `.raw`; its parent
directory must exist. Quote paths containing spaces.
Screenshots are downloaded through the monitor API and written atomically to the
chosen path, replacing an existing file only after a successful capture. PBM and
PNG use the monitor's Python orientation conversion; raw output preserves the
physical framebuffer bytes. PNG requires `python3 -m pip install Pillow`.
See [screenshot geometry and measurement limits](debugging-monitor.md#screenshots-and-measurement-limits)
for the capture format and its limits.

### Common options and logs

`--url http://127.0.0.1:PORT`, `--timeout SECONDS`, and `--json` work before or
after the command. The default timeout is 60 seconds for the entire command,
including all repetitions. Individual firmware discovery/state replies also
have a 10-second limit. For `logs --follow`, the timeout bounds each HTTP poll.
`logs --after N` resumes from an event sequence number; `--filter TEXT` matches
case-insensitive substrings. Older logs remain in the saved monitor directory.

### Repeatable command files

Command files use the same command syntax, shell-style quoting, and `#` comments.
They do not execute shell commands. Set `--url` and `--json` on `run` itself;
each line may override `--timeout`. The run timeout is the default **per line**,
not a deadline for the whole file. Nested runs and `logs --follow` are unsupported.

```sh
python3 scripts/device_control.py --json run - <<'EOF'
# Start with the desired book already open.
wait --activity EpubReader
press page-forward --repeat 3 --expect-page-change --timeout 90
screenshot "after three pages.pbm"
state
EOF
```

The CLI parses the whole file and validates argument syntax, including raw-command
contents and screenshot extensions, before executing it. File access and device
conditions are checked when each step runs. It keeps one lease throughout and
stops at the first failure. It never automatically replays uncertain input.
Separate CLI invocations acquire separate leases; standalone `status` and `logs`
remain available while another client holds control.

`--json` emits one JSON object per result, including each repeated press. Batch
results include the command and source line number (`step`). Errors include an
exit code, the last command's delivery stage, last observed state, boot ID,
event cursor, and monitor log directory when available. An interruption before
completion may leave input delivered; inspect the device before retrying.

| Exit code | Meaning |
|---|---|
| 0 | Success |
| 1 | Local file/output failure |
| 2 | Invalid arguments, command file, or unavailable optional dependency |
| 3 | Monitor/serial connection failure |
| 4 | Lease conflict, protocol rejection, or capture failure |
| 5 | Timeout; command delivery or completion may be uncertain |
| 6 | Observed crash/reboot or event-history gap |
| 7 | Ready UI failed a state expectation |
| 130 | Interrupted with Ctrl-C |

A deadline reached between successful state polls uses the last confirmed state:
ready but mismatched is exit 7; still busy is exit 5. An unanswered state query
remains exit 5 even if an earlier reply was ready. Inspect `last_command` and
`last_state` before deciding what happened; neither failure automatically repeats
the input.

For flashing, run `serial release`, flash normally, then run `serial reconnect`.
See [serial handoff](debugging-monitor.md#flashing-and-reconnecting) for connection
and lease behavior.
Reconnecting can reset the device; follow it with `wait` to check firmware/UI
readiness. Within a batch, an explicit serial handoff starts a new boot session.
An unexpected reboot stops the batch. `send`, `status`, `logs`, and serial
handoff work without the firmware UI-control protocol. `send` reports only that
the host wrote the command; for example, `send 'INFO 0'` does not wait for its reply.

## UI control

The Python client uses the [monitor API](debugging-monitor.md#api) and acquires
an [exclusive control lease](debugging-monitor.md#exclusive-benchmark-control).
Run from the repository root with the server already running:

```python
import sys
sys.path.insert(0, "scripts")
from device_control import DeviceControl

with DeviceControl() as device:
    before = device.wait_ready(lambda s: s["activity"] == "EpubReader")
    device.press("PAGE_FORWARD")
    after = device.wait_ready(
        lambda s: s["activity"] == "EpubReader"
        and (s["spine"], s["page"]) != (before["spine"], before["page"])
    )
    print(after)
```

Each command is a single ASCII line. The monitor supplies the `CMD:` prefix.

| Firmware command | Response |
|---|---|
| `CMD:INFO 0` | `INFO` with protocol version, boot ID and last request ID, followed by supported `BUTTON` records |
| `CMD:PRESS 1 CONFIRM 80` | `ACCEPTED`, then `INPUT_DONE` after release passes through the firmware loop |
| `CMD:STATE 2` | Current activity/reader position and render readiness; does not request a render |
| `CMD:CANCEL 3` | Cancels pending or held synthetic input; returns `CANCELLED` |

Request IDs must increase within a boot; `INFO 0` is a read-only discovery
exception that lets a new client find the last ID. The Python client allocates
IDs automatically. Firmware rejects reused IDs with `STALE_ID` rather than
replaying a possibly delivered action. Replies contain a random boot identifier;
the client fails on an observed boot change or disconnect. Open a new client
session after rebooting. IDs correlate firmware commands; the monitor's
`command_id` is a separate host-side identifier.

Controls are `BACK`, `CONFIRM`, `LEFT`, `RIGHT`, `UP`, `DOWN`, `NAV_NEXT`,
`NAV_PREVIOUS`, `PAGE_FORWARD`, and `PAGE_BACK`. They use the normal button
mapping, including remapping and active orientation. The chosen physical slot is
latched until release. Disabled mappings and mappings to Power are unsupported;
there are no serial power, touch, or swipe commands in this version. Button holds
default to 80 ms and accept 20–2000 ms. A slow firmware loop can release later
than requested; `INPUT_DONE.held_ms` reports the actual duration.

Only one synthetic press can be active. Busy rendering/navigation and physical
input reject a new press with `BUSY`; physical input during a press produces
`INTERRUPTED`. Cancellation clears synthetic state without emitting a release
click. It cannot undo an action already triggered on button-down. Requests have
a bounded 127-byte buffer and a one-second partial-line timeout. An overlong,
invalid, or timed-out line is discarded through its next newline.

`INPUT_DONE` means input was delivered, not that the intended navigation occurred.
Always check a state predicate: a button may be ignored by the current activity,
or a page turn may be deferred. `STATE` reports `available=0,busy=1` when the
renderer is locked. Otherwise it includes `activity`, reader type (`0` none,
`1` EPUB, `2` TXT, `3` XTC), one-based `page`, `pages`, and zero-based EPUB `spine`.
`orientation` uses renderer enum values: portrait 0, landscape clockwise 1,
inverted portrait 2, landscape counterclockwise 3.

`requested` and `completed` are render generations, including coalesced requests.
`busy` also includes pending activity changes, runnable EPUB builds/repositioning, exclusive
USB-storage mode, and active injected input. Readiness describes the known UI work;
it is not a guarantee that every network or background task is idle. A chapter
builder paused at its lookahead window or heap limit does not keep the UI busy. The client
uses bounded waits and never requests an extra refresh to obtain readiness.

Control uses fixed RAM state and the existing main/render tasks. It does not add
a device command queue or allocate a new input buffer on the heap. Fixed state
still consumes DRAM: use identical instrumentation and polling cadence in develop
and experiment builds. Continue using the standard serial render timings; client
wait duration is not a rendering measurement.

The simulator can consume these same `CMD:` lines from stdin and emits replies
on stderr. Its HAL includes the shared `SerialInput` state when the consuming
firmware supplies it. Existing scheduled simulator input still works; it counts
as competing physical input during serial control. Performance measurements must
come from the real device, not simulator timing.

## Test fixtures

Generate repeatable book, image, and dictionary fixtures with the standard
library only:

```sh
python3 scripts/generate_device_control_fixtures.py /tmp/crosspoint-fixtures
```

Use a dedicated output directory; rerunning replaces the generated files.
Upload the books, images, and `invalid/` directory to a dedicated SD folder
through File Transfer. Put the four files in `dictionary/` under
`/dictionaries/CLI-Test/`, then select that dictionary in reader settings.
`manifest.json` records file sizes and SHA-256 hashes for upload verification.
Additional books are supplied separately.

`controls.epub` has numbered paragraphs, three chapters, an asymmetric image,
and a first-page footnote whose destination says “Footnote one: the destination
is correct.” The dictionary contains `apple`, `chapter`, `reader`, and
`starlight`, plus the `apples` synonym. `chapters.xtc` has three distinct pages
and two chapters. Matching PNG/BMP patterns, TXT/Markdown, an empty text file,
a broken EPUB, and deliberately invalid firmware bytes cover other paths.
The invalid firmware fixture exercises rejection only.

Record the initial settings, book position, and event cursor. Check content and
saved positions as well as ready activity names, then restore changed settings
and remove test bookmarks. Keep firmware application failures as separate
findings with CLI commands, screenshots, and serial evidence; fixes to this
automation should preserve the device behavior being tested.

## Verification

Run the host suite (localhost socket access is required):

```sh
python3 -m unittest discover -s scripts/tests -v
```

The input state machine also has a standalone native check for press/release
edges, cancellation, delayed loops, and timer wraparound:

```sh
c++ -std=c++20 -Ilib/SerialInput tests/serial-control/input.cpp -o /tmp/test-serial-input
/tmp/test-serial-input
```

For UI control, open a book and use the client example to turn forward and back.
Confirm the reader position changes and `requested == completed` when ready.
Repeated `STATE` queries must not increase the render generation. Cancel a long
Confirm before release and confirm no selection opens. Compare standard `MEM`
logs at the same activity before and after repeated navigation; use serial render
timings for performance comparisons.

For graph/headless mode and serial-port handoff checks, see
[monitor verification](debugging-monitor.md#verification).
