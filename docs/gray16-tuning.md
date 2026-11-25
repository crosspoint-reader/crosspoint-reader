# Tuning the 16 gray levels of a panel

The gray UI (`CROSSPOINT_GRAY_UI`) shows every screen except book pages as a
16-level image. How the panel reaches each level is a table of numbers that has
to be measured on the glass. This page explains the table, the serial commands
that change it on a running device, and how to run a tuning session.

It has been done for one device: the Metalio E-Ink4-Plus. The gray UI is
compiled only for that board, and the numbers in `kGrayUiLevels`
(`lib/GfxRenderer/GfxRenderer.cpp`) are that panel's. Another panel needs its
own driver support in the SDK and its own session.

## Where 16 levels are used

- **UI screens** use two edge grays by default (text, icon and corner edges),
  drawn with `kGrayUiLevels`. Flat gray fills and gray covers in the UI are
  behind `CROSSPOINT_GRAY_UI_FULL_TONE`; their screen changes looked bad on the
  Plus, because every gray area passes through black on its way in.
- **The sleep cover** is shown in all 16 levels
  (`GfxRenderer::displayGray16Bitmap`). It is a still, so it is drawn in one
  clean frame of the panel's own table, which has the smoothest levels. It needs
  real gray data: a book cover carries an 8-bit copy after its 2-bit pixels, and
  a picture of 8 or more bits per pixel is used as it is. Without either, the
  cover is drawn in four tones as before.
- **Book pages** keep the reader's own four-tone path.

## What the table says

Level 0 is white and level 15 is black. Each of levels 1 to 15 has three
numbers, all counted in waveform frames (about 20 ms each on the Plus):

| Number | Meaning |
| --- | --- |
| black | frames pushed toward black, starting from white |
| pull | frames pushed back toward white after those |
| clear | frames pushed toward white to clear a pixel at this level |

Level 0 holds three settings for the whole table:

| Number | Meaning |
| --- | --- |
| first | black frames a pixel that stays black gets on every update |
| second | white frames a pixel that stays white gets on every update |
| third | 1 draws new ink while old ink clears, both ending together; 0 draws only after the clearing has ended |

A pixel that changes is cleared to white and then driven to its new level. A
gray pixel that does not change is not driven.

## What was learned on the Plus

- A smooth gray is a push to black and a pull back. Pulling back is steep,
  about a quarter to a third of the range per frame, so one black count gives
  three or four grays. Mixing black counts fills the gaps.
- Black is reached in about 10 frames. More frames do not make it darker.
- Driving straight from white (a small `black`, no `pull`) gives a gray without
  passing through black, but it is grainy. Good on a one-pixel edge, poor on an
  area. Starting from 4 black frames is grainy unless pulled back twice.
- Freshly driven white is brighter than white that was left alone (about 5 of
  255 in a photo). Too long a `clear` leaves the old screen as a lighter shape;
  too short leaves a darker one. The white top-up brings resting white up.
- The panel's own table pushes a gray toward black before clearing it. In a
  gray UI that shows the screen being left as a black-and-white picture of
  itself. Clearing with white frames alone removes that.
- Drawing only after clearing looks like a white negative of the old screen on
  screens that redraw several times in a row. Overlapped timing avoids it.

## Serial commands

Built only with `-DCROSSPOINT_GRAY16_TUNING=1`. Add it to the board's
environment as `build_src_flags` in `platformio.local.ini` so it does not
replace the board's `build_flags`. Changes apply at once and are lost on reboot.

| Command | Effect |
| --- | --- |
| `CMD:GRAY16 DUMP` | print the table |
| `CMD:GRAY16 ON` / `OFF` | draw gray frames with the table / with the panel's own |
| `CMD:GRAY16 SET l b p c` | set level `l` (1 to 15): black, pull, clear |
| `CMD:GRAY16 SET 0 b w o` | black top-up, white top-up, overlapped timing |
| `CMD:GRAY16 BARS S hex` | draw 16 bars in each half of the panel as a fast frame. `hex` is 32 digits, one level per bar: the first half's 16, then the second half's |
| `CMD:GRAY16 BARS C hex` | the same from white: a white frame first, then the bars |
| `CMD:GRAY16 SHOW k` / `CLEAN k` | built-in patterns: 0 a ramp and its mirror, 1 the two swapped, 2 all white |

The bars run across the panel's long side. Held upright, the Plus shows them as
16 stripes stacked top to bottom in two columns; the first 16 digits are the
column that `SHOW 0` draws white at the top.

## Running a session

`scripts/gray16_tune.py` is the host side.

1. Build and flash with the tuning flag.
2. Start one bridge and leave it running. Opening the port resets the device,
   so every later command goes through this one connection:

   ```sh
   ~/.platformio/penv/bin/python scripts/gray16_tune.py bridge /dev/cu.usbmodemXXXX /tmp/gray16 &
   ```

3. Send commands and read the answers:

   ```sh
   python3 scripts/gray16_tune.py send /tmp/gray16 "CMD:GRAY16 DUMP"
   ```

4. Stop the bridge before flashing again: `touch /tmp/gray16/stop`.

A measurement is one drawn pattern and one photo:

- **Which recipes give which gray.** Give up to 14 levels different `black` and
  `pull` numbers, keep level 15 as black, and draw
  `BARS C 0123456789ABCDEFFEDCBA9876543210`. Each recipe then appears in both
  columns, at opposite ends.
- **Which clear length suits a level.** Draw test levels in one column and a
  reference in the other, then clear with `BARS S` and 32 zeros. A test stripe
  that still shows was cleared too little (darker) or too much (lighter).
- Photograph the screen from straight above in even light, with no reflection
  on the glass. Then measure one column at a time:

  ```sh
  python3 scripts/gray16_tune.py measure photo.jpg X0 X1 Y0 Y1
  ```

  `X0 X1` is a strip inside the column and `Y0 Y1` runs from the top of its
  first stripe to the bottom of its last, in the photo's own pixels. It prints
  each stripe's lightness between the darkest and lightest stripe, and a grain
  figure; the white stripe's grain is the camera's noise floor.
- Average the two columns to cancel the lighting, pick the recipes nearest an
  even ramp, load them, and draw `SHOW 0` to check the result.

Finish on the real UI: load the table with `SET` and `ON`, change screens a few
times, and look for what a photo cannot show: leftovers of the previous screen,
and how the change itself looks.

When the numbers are right, copy them into `kGrayUiLevels`. That table is what a
normal build uses; the serial commands only change the copy in memory.

## Asking an agent to do it

An agent can run the whole session if it can use the serial port and is given
the photos. It cannot see the screen, so it needs a photo, or a description,
after every pattern. A request that works:

> Run a gray tuning session on the Plus. Start the bridge, draw the recipe grid
> and tell me when to take a photo. I will send each photo; measure it with
> `scripts/gray16_tune.py`, adjust the table, and tell me what to look at next.
> Do not build or flash; tell me when a flash is needed and release the port
> first.

Things the agent must be told or must keep to:

- Opening the port resets the device, and a second program on the port breaks
  the session. Keep one bridge, and stop it before any flash.
- Nothing set over serial survives a reboot. After a flash, load the table
  again before testing.
- A photo taken at an angle or with a reflection on the glass gives wrong
  numbers for the stripes it covers. Ask for another instead of guessing.
- What the change between two screens looks like can only be judged by a
  person. Ask, and ask whether a leftover is lighter or darker than its
  surroundings: the two mean opposite corrections.
