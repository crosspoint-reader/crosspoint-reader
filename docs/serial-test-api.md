# Serial Test API

The firmware can be driven and observed over its USB serial port. A host (a person at a terminal,
a script, or an AI agent) sends one text line per command and gets one reply line back: press a
button, open a screen or a book at a given page, read or change a setting, look a word up in the
dictionary, wait until the device has finished drawing and building, ask what is on screen, take a
screenshot. This page explains what the commands do, how to use them, and how to add one.

- [Why it exists](#why-it-exists)
- [Which builds have it](#which-builds-have-it)
- [Connecting](#connecting)
- [How a command works](#how-a-command-works)
- [Command reference](#command-reference)
  - [Buttons: `BTN`](#buttons-btn)
  - [Screens: `ACT`](#screens-act)
  - [What is on screen: `STATE`](#what-is-on-screen-state)
  - [Screenshots: `SCREENSHOT`](#screenshots-screenshot)
  - [Waiting: `IDLE`](#waiting-idle)
  - [Events: `EVENTS`](#events-events)
  - [Settings: `GET` and `SET`](#settings-get-and-set)
  - [Dictionary: `ACT:DICT`](#dictionary-actdict)
  - [Touch: `TOUCH` and `SWIPE`](#touch-touch-and-swipe)
  - [Logging: `LOG`](#logging-log)
  - [Power: `SLEEP` and `REBOOT`](#power-sleep-and-reboot)
  - [Heap ballast: `HEAP`](#heap-ballast-heap)
  - [TrueType fonts: `TTF`](#truetype-fonts-ttf)
  - [Files on the card: `FS`](#files-on-the-card-fs)
  - [Other: `BOOTLOG`, `HELP`](#other-bootlog-help)
- [The USB Transfer screen](#the-usb-transfer-screen)
- [Safety notes](#safety-notes)
- [A worked session with tools/devctl.py](#a-worked-session-with-toolsdevctlpy)
- [Adding a command](#adding-a-command)

## Why it exists

E-reader bugs live on the device: a page that renders wrong, a popup that never goes away, a
setting that does not stick, a screen that hangs after a particular sequence of presses. Checking a
fix used to mean a person pressing buttons and looking at the screen. With these commands that can
be a script:

- **Automated device tests.** Open a book, turn pages, check the chapter and page in the status
  bar, compare a screenshot.
- **Regression checks after a merge.** Run the same script against the new build and see whether
  anything changed: screens reached, settings round-tripped, heap left after a chapter build.
- **Reproducing a bug report.** Replay the exact presses from a report, with the serial log
  captured alongside, until the bug shows up.
- **AI-driven testing.** An agent can navigate the device closed-loop: take a screenshot, decide,
  press, check `STATE`, repeat. Everything replies in plain lines that are easy to parse.

The commands go through the same routes the UI uses. A press is injected where a real key press
enters the firmware, so the user's button mapping and long/double-press handling apply; a screen is
opened with the same navigation call the menus use; a setting is changed by the same code as the
web settings page. A test therefore exercises the real code paths, not a back door.

## Which builds have it

The development builds (`pio run -e default`, `-e x4pro`, `-e lilygo_t5s3`) are compiled with
`-DSERIAL_TEST_API=1` and have everything on this page. The release builds (`gh_release`,
`gh_release_rc` and their X4 Pro and LilyGo twins) and `slim` leave the flag off and keep only the
first commands: `BTN` (the six front and side keys, `:LONG`), `ACT` (`HOME`, `READER:<path>`,
`SETTINGS`, `BOOKMARKS`, `FILES`, `LIBRARY`), `STATE` (screen, stack, the reader's book, spine and
page), `SCREENSHOT`, `BOOTLOG` and `HELP`. Anything else gets `CMD_ERROR:unknown command`, and
`CMD:HELP` lists what the build in hand has.

The full set costs about 10.4 KB of flash and 280 B of RAM on the X4 build (the basic commands
about 3 KB more than before this API). A new command belongs inside `#if SERIAL_TEST_API`.

## Connecting

Every supported board has a USB serial port on the chip itself (see [debugging.md](debugging.md)):
`/dev/ttyACM0` on Linux, `/dev/tty.usbmodem*` on macOS, `COMx` on Windows. Use 115200 baud (the
link is USB, so the number is not critical) and leave DTR/RTS as the serial library opens them.

Things to know about the port:

- **Write something after opening it.** On the ESP32-C3 boards (X3, X4) the device holds back its
  output until the host has written to the port. Send a blank line (`\r\n`) right after opening;
  the firmware ignores it.
- **Opening the port does not reset the device** with the default DTR/RTS lines.
  `tools/serial_transfer.py` deasserts them, and that *does* reset an ESP32-C3 (see
  [The USB Transfer screen](#the-usb-transfer-screen)).
- **Closing it can.** On Windows a plain close drops DTR before RTS, and an ESP32-C3 takes RTS high
  with DTR low as a reset request: it restarts to Home (`resetReason=11` in the boot log). Set RTS
  low, then DTR low, then close, as `tools/devctl.py` does.
- **The port goes away on a reboot or sleep.** The USB device re-enumerates. A long-running host
  has to close and reopen the port; `tools/devctl.py` does this by itself.
- **One program at a time.** Close your monitor before running a script, and never hold the port
  while flashing.
- **Log lines share the wire.** The device keeps printing its log (`[1234] [INF] [ERS] ...`) between
  replies. A host must pick out the reply by its prefix and skip everything else.

## How a command works

A command is one line starting with `CMD:`, ended by LF, CR or CRLF, at most 1 KB long (127
bytes on the USB Transfer screen):

```
CMD:BTN:RIGHT
```

Anything that does not start with `CMD:` is ignored, so a stray key in a terminal does no harm.
The firmware reads one command per pass of its main loop (a pass takes about 10 ms when idle) and
answers with one line that starts with a fixed prefix for that command: `BTN_OK`, `STATE:...`,
`SET_ERROR:...`. Errors always end in `_ERROR:<reason>`; an unknown command gets
`CMD_ERROR:unknown command <text>`. The two replies that are not a single line are `CMD:GET:*`
(one line per setting, then `GET_END:<count>`) and `CMD:SCREENSHOT` (binary frame between two
lines).

A reply means the command was **accepted**, not that its effect is finished: after `BTN_OK` the
press is queued and the screen has not been redrawn yet. Use [`CMD:IDLE`](#waiting-idle) to wait for
the effect before you look at the screen.

Values that contain spaces are written in double quotes, with `"` and `\` escaped by a backslash,
for example `book="/test/The Anarchy - Dalrymple, William.epub"`. Split such a line on spaces that
are outside quotes.

## Command reference

### Buttons: `BTN`

```
CMD:BTN:<BACK|CONFIRM|LEFT|RIGHT|UP|DOWN|POWER>[:LONG|:DOUBLE]
```

Reply: `BTN_OK`, or `BTN_ERROR:unknown button <name>` / `BTN_ERROR:bad press <text>`.

- The names are the **physical keys**. The user's front-button remapping and the configured
  short/double/long actions apply as they would for a finger (on this X4, `POWER` is bound to the
  dictionary; check `CMD:GET:btnShortPower` first).
- `:LONG` holds the key long enough to count as a long press (1.5 s, dated back, so the reply is
  immediate).
- `:DOUBLE` sends two taps 120 ms apart, inside the 300 ms double-press window. A key with no
  double action reads them as two short presses, as it would from a finger. `POWER` and `:DOUBLE`
  need the full API.
- `POWER:LONG` puts the device to sleep, like holding the power key (see
  [Power](#power-sleep-and-reboot) before you use it).

```
> CMD:BTN:DOWN
< BTN_OK
> CMD:BTN:CONFIRM:LONG
< BTN_OK
```

### Screens: `ACT`

```
CMD:ACT:HOME
CMD:ACT:READER:<path>                     open a book where it was left
CMD:ACT:READER:<path>@<spine>:<page>      open it at a spine item and page (1-based, as STATE shows)
CMD:ACT:READER:<path>@<percent>%          open it at a share of the book (0-100)
CMD:ACT:SETTINGS[:<DISPLAY|READER|CONTROLS|SYSTEM|0-3>]
CMD:ACT:BOOKMARKS
CMD:ACT:FILES:<folder>                    the library's file browser in that folder
CMD:ACT:LIBRARY[:<BOOKS|RECENT|NEW|AUTHORS>]
CMD:ACT:FILE_TRANSFER                     the File Transfer picker (no Wi-Fi until a mode is chosen)
CMD:ACT:USB_TRANSFER                      the USB Transfer screen
CMD:ACT:DICT:<word>                       see Dictionary below
```

Reply: `ACT_OK`, or `ACT_ERROR:<reason>` (`no such file <path>`, `bad position <text>`,
`unknown activity <name>`, `unknown settings tab <name>`, `unknown library tab <name>`).

- Paths are paths on the SD card (`/test/book.epub`). A file name may itself contain `@`: only the
  text after the last `@` is read as a position, and only when it has one of the two forms. A book
  under `/test/` stays out of the recent books and the reading stats (see
  [Safety notes](#safety-notes)).
- Positions are for EPUB books, and need the full API (so do `DICT`, `FILE_TRANSFER`,
  `USB_TRANSFER` and the settings tab). `@<spine>:<page>` counts from 1, exactly as `STATE` reports
  `spine=` and `page=`: `@20:12` opens the 12th page of the 20th spine item and `STATE` then says
  `spine=20/.. page=12/..`. That is the page within the spine item; the status bar counts pages in
  the whole TOC chapter (`chapter_page=` in `STATE`), which is a larger number when the chapter
  started in an earlier spine item. `@<spine>:<page>` uses the hand-off the bookmark list uses, so
  it also saves that position as the book's progress. `@<percent>%` lands where the reader's own
  "go to percent" lands.
- The new screen opens on the next loop pass; wait with `CMD:IDLE` before you look at it. Opening a
  book that has never been opened indexes it first, which can take seconds (a full `CMD:IDLE`
  waits for that; `IDLE:...:SCREEN` does not).

```
> CMD:ACT:READER:/test/The Anarchy - Dalrymple, William.epub@20:5
< ACT_OK
> CMD:IDLE:60000:SCREEN
< IDLE:2458
> CMD:STATE
< STATE:activity=EpubReader stack="" book="/test/The Anarchy - Dalrymple, William.epub" spine=20/40 page=5/40 font=Bookerly font_pt=14 chapter="1. THE BRITISH" chapter_page=5/40 percent=1 heap=64616 maxblock=57332
```

### What is on screen: `STATE`

```
CMD:STATE
```

Reply: one line, `STATE:` followed by `key=value` pairs.

| Key | Where | Meaning |
| --- | --- | --- |
| `activity` | always | the screen on top (`Home`, `EpubReader`, `Settings`, `DictionaryWordSelect`, ...) |
| `stack` | always | the screens below it, bottom first, comma-separated (`""` when none) |
| `sel`, `label` | menus and lists | the highlighted row (0-based) and its text; `sel=-1` when a tab bar has the focus |
| `popup`, `popup_ms` | when a popup is up | its text, and how long ago it was drawn |
| `book`, `spine`, `page` | EPUB reader | the file, spine item `n/count`, page within that spine item `n/count`; both count from 1 |
| `font`, `font_pt` | EPUB reader | the font family and point size the book is set in |
| `chapter`, `chapter_page`, `percent` | EPUB reader | what the status bar shows: the TOC chapter title, the page within the whole chapter, which can span several spine items (`~` when the total is still an estimate), the book percentage |
| `heap`, `maxblock` | always | free heap and the largest free block, in bytes |

Everything but `activity`, `stack`, `book`, `spine` and `page` needs the full API. A popup counts as
on screen until the screen is redrawn (the dictionary's "Not found" is up for 1.5 s). Lists report the row of the list drawn last on the current screen. Screens that draw their
own lists without the shared list code (a few older pickers) report no `sel`.

```
STATE:activity=Settings stack="" sel=1 label=Zeichensatz heap=54396 maxblock=38900
STATE:activity=DictionaryWordSelect stack=EpubReader popup="Nicht gefunden" popup_ms=919 heap=55880 maxblock=51188
```

Labels and popups are in the device's UI language. Compare them with what the screen shows, not
with English text.

### Screenshots: `SCREENSHOT`

```
CMD:SCREENSHOT
```

Reply:

```
SCREENSHOT_START:<width>:<height>:<bytes>
<bytes of raw frame>
SCREENSHOT_END
```

or `SCREENSHOT_ERROR:busy` (the display stayed busy for 5 s), `SCREENSHOT_ERROR:framebuffer
released` (the Wi-Fi file server has the memory), `SCREENSHOT_ERROR:transfer stalled` (the host
stopped reading).

The capture waits for the current render pass and copies the frame that is on the panel, not one
being drawn. In the reader, whose next page may already be drawn off-screen in the same memory, the
page on screen is drawn again, status bar included, for the capture. The frame is 1 bit per pixel, rows from top to bottom, 8 pixels per byte with the
leftmost pixel in the highest bit; a set bit is white. It is in the panel's **native** orientation
(800x480 on the X4, landscape), so rotate it to the orientation the screen is used in:

| Screen orientation | PIL rotation |
| --- | --- |
| Portrait (the default) | `rotate(-90, expand=True)` |
| Portrait, inverted | `rotate(90, expand=True)` |
| Landscape, clockwise | `rotate(180)` |
| Landscape, counter-clockwise | none |

```python
from PIL import Image
image = Image.frombytes("1", (width, height), data).rotate(-90, expand=True)
image.save("screen.png")
```

`tools/devctl.py` (option `--rotate`) and `scripts/debugging_monitor.py` decode the frame for you.
Replies, event lines and the frame are all written by the main loop, so they never cut into each
other.

### Waiting: `IDLE`

```
CMD:IDLE[:<timeout_ms>][:SCREEN]
```

Reply, once the device is idle: `IDLE:<milliseconds waited>`. If it is not idle within the
timeout (default 10000 ms, at most 600000): `IDLE_TIMEOUT:<what kept it busy>`.
`IDLE_ERROR:...` for a malformed argument.

`IDLE` follows the last command that should change the screen (`BTN`, `TOUCH`, `SWIPE`, an `ACT`
that opens a screen) to its end, so it never answers between the command and its effect. The
device stays busy until the press has been taken in by the input path (and the double-press window
is over), or until the new screen is on top; then until the frame that causes has been drawn. A
press that changes nothing on screen counts as done 1 s after it was taken in.

"Idle" means that for 250 ms in a row none of these was true:

| Reason | What it means |
| --- | --- |
| `transition` | a screen change is queued |
| `input` | a press (or touch) has not been taken in yet, or is waiting out the double-press window |
| `render` | a frame is queued, being drawn, or still being refreshed onto the panel; or the screen is about to change by itself (the reader's page still being built, a page turn on its way, the anti-aliasing redraw of the page); or the frame of the last command's effect is still to come |
| `background` | the screen has work of its own to finish: the reader's chapter builds (the current one and the look-ahead), the pre-render of the next page, image decodes; Home's cover loading |
| `settling` | nothing was busy, but not yet for 250 ms when the timeout came |

With `:SCREEN` it waits only until the screen has settled (everything above but `background`).
That is what you want before a screenshot after a page turn or after opening a book. A full
`CMD:IDLE` in the reader also waits for the look-ahead builds of the next chapters, which start
1.5 s after a page appears and can take tens of seconds on a long book.

Send `CMD:IDLE` after the command it should wait for; the device handles them in order. While an
`IDLE` is pending, the device still answers other commands; a second `IDLE` replaces the first.

**`IDLE:<ms>` is no timing of what it waited for.** It counts from when the device read the `IDLE`
line, and the device reads one command line per loop pass: work that holds the loop before that
is left out. Opening a book whose font family has to be installed first (the "Loading font..."
popup) holds the loop for seconds, so an `IDLE` sent right behind the `ACT` is read only after the
install. A 20.6 s open of a book with a TrueType family not yet installed was answered
`IDLE:3580`. Time an open on the host, from sending `ACT` to the `IDLE` reply:
`tools/devctl.py --log <file>` stamps every line with the host's clock.

```
> CMD:BTN:RIGHT
< BTN_OK
> CMD:IDLE:5000:SCREEN
< IDLE:613
> CMD:SCREENSHOT
```

### Events: `EVENTS`

```
CMD:EVENTS:ON | CMD:EVENTS:OFF | CMD:EVENTS
```

Reply: `EVENTS_OK:on` / `EVENTS_OK:off`. While events are on, the device prints these lines
unasked (off is the default, so normal logs stay clean):

```
EVT:ACT <screen>                    a different screen is on top
EVT:POPUP <text>                    a popup was drawn
EVT:BUILD_DONE spine=<n> pages=<m>  a chapter (spine item n, 1-based) finished building, with m pages
EVT:TTF_FULL fills=<n>              the TrueType glyph store filled up (n since boot)
EVT:TTF_WIPE ms=<t> wipes=<n>       the glyph store was emptied, in t ms (n since boot)
EVT:FONT_INSTALLED family=<name> ok=<0|1>  a font download ended, from the Font Manager or the web page
```

Events are a log of what happened, for a script that wants to know a build finished or a popup
appeared without polling `STATE`. They are written when the main loop gets to them, so they can
arrive after a later reply. Events from before `EVENTS:ON` are not replayed.

### Settings: `GET` and `SET`

```
CMD:GET:<key>
CMD:GET:*
CMD:SET:<key>=<value>
```

The keys are the ones the web settings page uses (`GET /api/settings` in
[webserver-endpoints.md](webserver-endpoints.md)), plus `language` (below); `CMD:GET:*` lists every
key the board offers.

Replies:

```
GET:<key>=<value>[ label=<option>]
GET_END:<number of keys>                     (after GET:*)
SET_OK:<key>=<value>[ label=<option>]        the value as stored afterwards
GET_ERROR:unknown key <key>
SET_ERROR:unknown key <key> | out of range <value> | bad value <value>
SET_ERROR:not writable <key> | expected <key>=<value>
SET_ERROR:not saved <key>                    applied, but the settings file was not written
GET_ERROR / SET_ERROR:font families not loaded on this screen <key>   (boot straight into USB Transfer)
```

- Numbers: an on/off setting is `0` or `1` (any non-zero sets it on); a choice is its option
  index, with the option's text as `label`; a slider (`sleepTimeoutMinutes`, ...) is its number.
- A choice can also be set by its label, exactly as `GET` shows it (`CMD:SET:lineSpacing=Normal`).
  Labels are in the UI language, so scripts should prefer numbers.
- Text settings are written as they are. Passwords are never sent back: `GET` shows `(hidden)`.
- `SET` does what the web page does after a change: it settles the settings that depend on others,
  then saves to the card at once. Screens pick a change up the next time they read the setting,
  which is usually when they are next opened; a screen already open (the settings list itself) may
  show the old value until then.

```
> CMD:GET:btnDoublePower
< GET:btnDoublePower=6 label=Standby
> CMD:SET:btnDoublePower=0
< SET_OK:btnDoublePower=0 label=Ignorieren
...
> CMD:SET:btnDoublePower=6
< SET_OK:btnDoublePower=6 label=Standby
```

**The UI language** is not a setting (it is kept in `/.crosspoint/language.bin`, and the web
settings page has no row for it), but `GET` and `SET` take it as the key `language`, and `GET:*`
lists it last:

```
GET:language=<code> label=<name>
SET_OK:language=<code> label=<name>
SET_ERROR:bad value <value>
```

- The value is the language's code in any case (`EN`, `de`, `ZH`), its name exactly as the
  language picker shows it (`Deutsch`, `简体中文`), or its index in the build's language list
  (English is 0).
- `SET` makes the switch the language picker makes, saves it, and redraws the screen on display.
  A screen that built its rows when it opened keeps them until it is opened again.
- Change it back afterwards: the setting persists, and the next session reads it.

```
> CMD:GET:language
< GET:language=EN label=English
> CMD:SET:language=de
< SET_OK:language=DE label=Deutsch
> CMD:SET:language=EN
< SET_OK:language=EN label=English
```

### Dictionary: `ACT:DICT`

```
CMD:ACT:DICT:<word>
```

Looks the word up in the dictionary chosen in the settings, the way the reader's dictionary does
(same cleaning, synonyms and word-form fallbacks), without opening a screen. Reply:

```
DICT:result=<outcome> dict=<dictionary> word=<word>[ headword=<matched entry> bytes=<definition length>]
DICT_ERROR:no word
```

`<outcome>` is `found`, `not_found`, `low_memory`, `decompress_error`, `read_error`,
`no_dictionary` (none chosen), `open_failed` or `index_failed`. A dictionary used for the first
time builds its index first, which can take several seconds; the device does nothing else meanwhile.
Every lookup, from this command or from the reader, also writes a log line:

```
[209767] [INF] [DICT] Lookup dict=en-de word="house": found headword=house
```

### Touch: `TOUCH` and `SWIPE`

```
CMD:TOUCH:<x>,<y>
CMD:SWIPE:<x1>,<y1>,<x2>,<y2>
```

Reply: `TOUCH_OK` / `SWIPE_OK`; `TOUCH_ERROR:no touch panel` on boards without one (the X3/X4
build does not carry the touch code at all); `TOUCH_ERROR:not on screen <text>` for a malformed
argument or a point off the screen.

Coordinates are screen pixels as the screen is drawn now: origin top-left, x to the right, in the
current orientation (the rotated screenshot shows the same frame). The touch is queued where the
touch controller's own taps and swipes enter, so gestures, list taps and the reader's tap zones read
it as a finger. Make a swipe at least 60 px long, as a finger's would be. Two limits: screens that
watch a finger while it is still down (a row lighting up under it) do not see an injected touch,
and outside the reader taps only work while the "touch controls" setting is on.

### Logging: `LOG`

```
CMD:LOG[:<ERR|INF|DBG|TRC>]          (also ERROR, INFO, DEBUG, TRACE; any case)
```

Reply: `LOG_OK:level=<now> compiled=<most the build has>`, or `LOG_ERROR:unknown level <text>`.

Sets the most verbose log level written to the serial port, for a quieter wire during a test
(`CMD:LOG:ERR`). It cannot go above what the build compiled in (`DBG` for the development build).
The crash-report ring buffer keeps every line regardless, and a few lines the display driver prints
directly (`Wait complete: refresh ...`) are not log lines and stay. A reboot restores the default.

### Power: `SLEEP` and `REBOOT`

```
CMD:SLEEP
CMD:REBOOT
```

Replies: `SLEEP_OK`, `REBOOT_OK`, sent before the device acts.

- `SLEEP` goes to sleep the normal way (sleep screen, progress saved), as a held power key does.
  **Deep sleep switches the USB port off, and only a real button press wakes the device.** Do not
  use it on a device nobody can reach.
- `REBOOT` restarts to Home the way the firmware's own maintenance restarts do: the reading session
  is closed, no boot screen. The port re-enumerates; reopen it.

### Heap ballast: `HEAP`

```
CMD:HEAP
CMD:HEAP:HOLD=<bytes>[,<blocks>]
CMD:HEAP:FREE
CMD:HEAP:REFUSE=<decodes>[,<reloads>]
```

Reply: `HEAP_OK:free=<bytes> largest=<bytes> held=<bytes>` (free heap, its largest block, the ballast
held), `HEAP_OK:refuse decodes=<n> reloads=<n>`, or `HEAP_ERROR:expected HOLD=<bytes>[,<blocks>],
REFUSE=<decodes>[,<reloads>] or FREE`, or `HEAP_ERROR:not all held:...` when the heap could not give
every block (what was taken stays held).

Holds `<bytes>` more of the heap, in `<blocks>` equal blocks (1 to 16 in all, default 1), until
`CMD:HEAP:FREE` gives it all back. Nothing else frees it, so a test can put the device where a short
or broken-up heap takes it, on demand: with enough held, the decode of a page's image is refused and
the reader's memory ladder (`src/activities/reader/ImageMemoryLadder.h`) climbs, one step further
for each step that still leaves too little. Several blocks take several of the heap's free regions,
which shrinks its largest block as well as what is free. Only in builds with the full API.

```sh
python3 tools/devctl.py cmd:HEAP cmd:HEAP:HOLD=24000,4 "cmd:ACT:READER:/test/book.epub@3:1" idle:60000 \
  shot:page.png cmd:HEAP:FREE
```

Free it before you leave the device: held ballast survives every screen until a restart. And take
it once the screen you test is open: code that allocates with a throwing `new` aborts the device when
the heap is short (a book open loads the recent-books list that way), and ballast held across a
screen change can starve it.

`REFUSE` reaches what ballast cannot. With the lent framebuffer as the decoders' arena, their heap
floors are 16 KB, and the ladder's first relief step frees about 15 KB of font memory (X4, four
TrueType families): ballast that makes that step fail leaves the reader no heap to draw with.
`REFUSE=<decodes>` makes the ladder's next `<decodes>` decode steps fail as if the heap had refused
them (the step's font memory is still freed and taken back for real), and `<reloads>` the next
reloads of the TrueType set after its heavy step. `REFUSE=2` shows the heavy step, `REFUSE=3` (more,
for the image lane's tries) the "not enough memory" boxes, `REFUSE=2,1` an accent family left out;
`REFUSE=0` clears them. Test builds only, like `HOLD`.

### TrueType fonts: `TTF`

```
CMD:TTF
CMD:TTF:FULL
```

Reply to `CMD:TTF`, the loaded TrueType families and their glyph store:

```
TTF:families=1 names=NotoSansTC sd=1 log_used=80800 log_size=1835008 slots=512 ring=0 pending=0 renders=540 hits=2210 fills=0 wipes=1 last_wipe_ms=1312
```

`names` lists the set's families in its order (the body's first, then a book's accent families); `sd=1` when one
reads its outlines from the card; `log_used`/`log_size` the store's log in bytes; `slots` its index slots in use;
`ring=1` while new glyphs are drawn without being stored (the store filled up, or a write failed); `pending=1` while
a wipe waits for the next book open; `renders` and `hits` the glyphs drawn from outlines and read from the store
since the set loaded. `fills`, `wipes` and `last_wipe_ms` count since boot, whichever set was loaded. With no
TrueType family loaded the set's fields are replaced by `none`: `TTF:none fills=0 wipes=0 last_wipe_ms=0`.

`CMD:TTF:FULL` makes the store act as if it had just filled up: `TTF_OK:full`, or `TTF_ERROR:no TrueType family
loaded`. New glyphs go to the ring and the wipe is pending, exactly as when the log runs out, so what a full store
does can be tested at any size without reading until it fills. Development builds only.

### Files on the card: `FS`

```
CMD:FS:LS:<folder>
CMD:FS:RM:<path>
```

`LS` lists a folder, one line an entry, then the count:

```
FS:d 0 epub_2247170459
FS:f 32946 cjk_store_fill.epub
FS_END:2
```

`RM` removes a file, or a folder with everything in it: `FS_OK:removed <path>`. Errors: `FS_ERROR:expected
LS:/<folder> or RM:/<path>`, `FS_ERROR:protected <path>`, `FS_ERROR:not found <path>`, `FS_ERROR:not a folder
<path>`, `FS_ERROR:could not remove <path>`.

Both work while the reader runs, without the USB Transfer screen and the restart that leaving it costs: deleting a
book's section cache (`/.crosspoint/epub_<hash>/spines/...`) forces a cold build of a chapter, and a font folder
under `/.crosspoint/fonts` can be looked at or removed between runs. Hidden folders can be reached; the credential
stores (`wifi.json`, `opds.json`, `koreader.json`), a dot-named item itself (`/.crosspoint` cannot be removed) and
any path spelled with an 8.3 alias are refused (`lib/FsHelpers/ProtectedPaths.h`). Removing what an open book is
using is not guarded: do it from Home. Development builds only.

### Other: `BOOTLOG`, `HELP`

- `CMD:BOOTLOG` prints the boot summary again (for a monitor attached after boot) as log lines,
  then `BOOTLOG_OK`.
- `CMD:HELP` replies `HELP:` and the names of the commands this build has (see
  [Which builds have it](#which-builds-have-it)).

## The USB Transfer screen

File Transfer > USB Transfer (`CMD:ACT:USB_TRANSFER`) speaks a binary file-transfer protocol on the
same port (`tools/serial_transfer.py`, compatible with MicroReader's tools). While it is up:

- It still answers `CMD:` lines between transfer messages (full API, lines up to 127 bytes), so a
  script can check it with `CMD:STATE` and leave it with `CMD:BTN:BACK` or `CMD:ACT:HOME`. A
  transfer message starts with `CMND` or `EPUB`; a line starting with `CMD:` is told apart by its
  first four bytes.
- Leaving it always restarts the device to Home (by design: a clean start is the simplest way back
  from what the transfer screen turns off). So does opening any other screen from it.
- Log lines are held back from the wire so they cannot corrupt a transfer; replies still come.
- `tools/serial_transfer.py` opens the port with DTR/RTS deasserted, which resets an ESP32-C3. The
  screen arms the restart to come straight back to itself, so the tool's reset lands on the
  transfer screen again after about 2 s. `tools/devctl.py` does not reset the device.
- That arm survives any reset short of a power loss while the screen is up: a flash, or a reset of
  the board, lands on the transfer screen again, not on Home. Check `CMD:STATE` after flashing.
- A boot straight back into it skips SD-font discovery (the screen uses built-in UI fonts). The card's
  font families are not listed then, so `GET` and `SET` of `fontFamily` and `txtFontFamily` answer
  `font families not loaded on this screen`; leave the screen first.
- Run one tool at a time. A typical sequence: `devctl.py cmd:ACT:USB_TRANSFER`, then
  `serial_transfer.py ls /test` (or `put`, `get`, ...), then `devctl.py cmd:BTN:BACK`.

The Wi-Fi file server (File Transfer > Join network / Hotspot / Calibre) frees the screen memory
while it runs, so `SCREENSHOT` answers `framebuffer released` there; `STATE` and `BTN` work, and
leaving it with Back restarts the device to Home.

## Safety notes

The commands act on a real device with a real user's books and settings:

- **`SET` changes the user's settings and saves them.** `GET` the value first and set it back when
  you are done.
- **Opening a book changes its reading position.** The reader saves where it is, and a
  `@<spine>:<page>` open saves that page at once. Test with books kept for testing (a `/test`
  folder), not the user's own.
- **A book under `/test/` is a test book** in builds with the full API (in any case: `/TEST/` is
  the same folder on FAT, and the file browser opens it so): opening it records it neither in the
  recent books (Home, the library's Recent tab) nor in the reading stats. Both keep a fixed number
  of books (10 and 100), and a test run that opened its books used to push the user's own out of
  them. The rest is as for any book: its position is saved in its cache, `state.json` names it as
  the book last open, and a per-book setting changed on it holds while it stays open, but is not
  kept, since per-book settings live in the recent-books entry it does not get
  (`SerialTestApi::isTestBook`).
- **Presses do what the user configured.** A double press of Power may be bound to sleep, a long
  Confirm to a sync. Check the binding with `GET` first, or bind it to something harmless for the
  test and restore it.
- **Navigate closed-loop.** Menus change length (a "Bookmarked pages" row appears only with
  bookmarks); take a screenshot or a `STATE` before every `CONFIRM`, rather than counting presses.
- **`SLEEP` and `BTN:POWER:LONG` end the session** until someone presses a button.
- **A dictionary index build** (the first lookup in a dictionary) holds the device for seconds.

## A worked session with tools/devctl.py

`tools/devctl.py` sends commands, waits for their replies, decodes screenshots, and logs everything
the device prints. Steps run in order and the run stops at the first failure:

```sh
python3 tools/devctl.py --log run.log \
  cmd:ACT:HOME idle:30000 \
  "cmd:ACT:READER:/test/The Anarchy - Dalrymple, William.epub@20:5" idle:60000:SCREEN \
  state check:spine=20/40 check:page=5/.* shot:page.png \
  cmd:BTN:RIGHT idle:5000:SCREEN state check:page=6/.* \
  cmd:ACT:HOME idle:30000
```

```
ACT_OK
IDLE:2268
ACT_OK
IDLE:2458
STATE:activity=EpubReader stack="" book="/test/The Anarchy - Dalrymple, William.epub" spine=20/40 page=5/40 ...
check spine='20/40' ~ /20/40/: ok
check page='5/40' ~ /5/.*/: ok
screenshot -> page.png
...
```

Steps: `cmd:<TEXT>` (send `CMD:<TEXT>`, fail on an error reply; `GET:*` and `FS:LS:` print every line of
their reply), `state` and `check:<key>=<regex>` (test a field of the last `STATE`), `idle[:<ms>[:SCREEN]]`,
`shot:<file.png>`, `wait:<seconds>`, `expect:<regex>:<seconds>` (wait for a log line), `select:<regex>[:<n>]` (press
Down, waiting for each press to land, until the selected row's label matches; at most n presses, 40 by default).
`--script FILE` reads one step per line (`#` comments), `--keep-going` runs all steps and reports every failure,
`--rotate` sets the screenshot rotation. Paths may be given as Git Bash writes them (`/c/tmp/page.png`); output is
UTF-8 whatever the console, and the port is closed without resetting the device even when a step fails. A script
file makes a reusable regression check:

```
# settings round trip: the binding is restored at the end
cmd:GET:btnDoublePower
cmd:SET:btnDoublePower=0
cmd:BTN:POWER:DOUBLE
idle
state
check:activity=Home
cmd:SET:btnDoublePower=6
```

`select:` reaches a row by its label instead of counting presses: presses sent back to back reach a list faster than
it redraws, and it lands further down than asked. Opening the Font Manager from the Reader settings:

```sh
python3 tools/devctl.py cmd:ACT:SETTINGS:READER idle:20000 "select:Font Manager" cmd:BTN:CONFIRM idle:60000:SCREEN
```

For a long session, capture the full log separately as well (one reader of the port at a time:
`devctl.py --log` already records every line it sees).

## Adding a command

The code is in `src/SerialTestApi.cpp`; the argument grammar is in `src/util/SerialCommandGrammar.*`
so it can be tested on the host (`test/serial_command_grammar`).

1. Write a handler `void cmdName(std::string_view arg)` in `SerialTestApi.cpp` and add it to the
   `kCommands` table. `arg` is the text after `CMD:NAME:`.
2. Parse anything beyond a keyword in `SerialCommandGrammar`, with a host test for it.
3. Reply with exactly one line through `writeLine()` / `replyf()`, with a prefix of its own
   (`NAME_OK...`) and `NAME_ERROR:<reason>` for every failure. Write only from the main loop
   (handlers and `service()` run there); code on other tasks records what it saw in a probe
   (`notePopup()` and friends) and `service()` reports it.
4. Reuse the route the UI takes (an `ActivityManager::goTo*()`, `HalGPIO::injectPress()`, the
   settings rows) rather than a shortcut, so a test exercises what users run.
5. Keep it cheap: no heap held between commands, nothing on the hot path. Put it inside
   `#if SERIAL_TEST_API` so release builds do not carry it (see
   [Which builds have it](#which-builds-have-it)); a probe the UI calls gets an empty inline twin in
   `SerialTestApi.h` for builds without the flag.
6. Add the reply prefix to `REPLY` in `tools/devctl.py` and document the command on this page.

For what a screen reports in `STATE`, override `Activity::appendDebugState()` and write each pair
with `SerialCommandGrammar::appendField()`. For work `IDLE` should wait for, override
`Activity::hasPendingScreenWork()` (the screen is about to change) or
`Activity::hasPendingBackgroundWork()` (work of its own that does not change the screen). The
waiting rules themselves are in `src/util/IdleGate.h`, tested in `test/idle_gate`.
