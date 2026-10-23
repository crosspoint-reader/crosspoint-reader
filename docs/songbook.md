# Songbook

Songbook shows songs with chords for live playing. You pick a setlist and a
song. Then you can show the chords only, the full lyrics with chords that
scroll by themselves while you play, or chord diagrams for guitar, ukulele,
baritone ukulele, or 4- and 5-string bass. It works offline and uses only the
device buttons.

Songbook is a feature of this fork. It is not part of upstream CrossPoint.

## Put songs on the SD card

Make this folder layout in the root of the SD card:

```text
/songbook/
  songs/
    amazing-grace.cho
    luch-solntsa-zolotogo.cho
  setlists/
    concert.json
```

- `songs/` holds the songs, one ChordPro file per song. Supported extensions
  are `.cho`, `.chordpro`, `.chopro`, and `.crd`. Files must be UTF-8.
- `setlists/` holds setlists, one JSON file per setlist.

The **Songbook** item appears on the Home menu when the card has a
`/songbook` folder. It shows in every Home theme except Cover Grid. If the
Home menu has more items than fit on the screen, it scrolls with the
selection, and small arrows show that more items are above or below.

A ready-made example is in [songbook/sd-card](songbook/sd-card/). Copy its
`songbook` folder to the root of the card. You can copy files with a card
reader, USB Drive mode, or the File Transfer web page.

Songbook never changes your songs or setlists. It writes one small file,
`/.crosspoint/songbook.json`, which keeps the instrument you chose in the
Diagrams view.

## Setlist format

```json
{
  "title": "Demo concert",
  "songs": ["when-the-saints.cho", "luch-solntsa-zolotogo.cho", "rock/another.cho"]
}
```

- `title` is shown in the list. If it is missing, the file name is used.
- `songs` is the play order. Paths are relative to `/songbook/songs/` and may
  use subfolders. Use `/` between folder names. Paths that start with `/`, or
  that contain `..`, `\`, or an empty folder name, are skipped.
- In the song list, a song that cannot be opened is marked "missing", and a
  file over 64 KB is marked "too large".
- A setlist that cannot be read is marked "Setlist file is invalid". A setlist
  with no usable songs is marked "This setlist has no songs".
- **All songs** lists the files directly in `/songbook/songs/`, sorted by
  name.

## Song format (ChordPro)

Songbook reads a subset of [ChordPro](https://www.chordpro.org/). It skips
directives that it does not know, so files written for other ChordPro tools
still open.

```text
{title: Example Song}
{artist: Example Artist}
{key: Em}
{capo: 2}
{tempo: 100}
{duration: 3:30}

{start_of_verse}
[Em]First line of the song
[Am]Second line of the song
{end_of_verse}

{start_of_chorus}
[C]Chorus line
{end_of_chorus}

{chorus}
```

| Directive | Meaning |
| --- | --- |
| `title` / `t`, `artist`, `key`, `capo`, `tempo` | Shown in the header (`tempo` is read but not shown yet) |
| `duration` | Song length as `210` seconds or `3:30`. Used by auto-scroll. Without it, 3:00 is used and the header shows `~3:00` |
| `start_of_verse` / `sov`, `start_of_chorus` / `soc`, `start_of_bridge` / `sob` and their `end_of_…` | Sections. A label is optional: `{start_of_verse: Verse 2}` or `{sov label="Verse 2"}` |
| `start_of_intro`, `start_of_outro`, any other `start_of_NAME` | Named sections |
| `start_of_tab` / `sot`, `start_of_grid` / `sog` | Text shown as written, with no chord parsing |
| `chorus` | Repeat the last chorus here |
| `comment` / `c`, `ci`, `cb`, `highlight` | A note line, for example `{c: Solo x2}` |
| `x_cue` | Words shown only in the Chords view. See [Words in the chord chart](#words-in-the-chord-chart) |
| `[Chord]` inside a line | The chord is played on the next character. `[*text]` is an annotation |
| `# text` | A source comment. It is not shown |

### Explicit chord chart

The Chords view normally builds its chart from the chords in the lyrics. That
chart is not checked and can be wrong. To show your own chart, add a block
that uses ChordPro's custom `x_` namespace (other tools ignore it):

```text
{start_of_x_chart}
Intro: G D G
Verse: G G7 C G | G Em D
Chorus: C G D G | x2
{end_of_x_chart}
```

Each line is `Label: chords`. The label is optional.

### Words in the chord chart

The Chords view shows no lyrics. To be reminded how a verse or chorus starts,
put `{x_cue}` before a lyric line. The Chords view then shows the words of
that line, without chords, in a smaller font above its chords. The Lyrics
view does not change. Other ChordPro tools ignore `{x_cue}`.

```text
{start_of_chorus}
{x_cue}
[C]First line of this chorus
[G]Second line
{end_of_chorus}
```

- When two verses or choruses have the same chords, the chart shows the
  second one as "(same chords)". The `{x_cue}` words stay under that label,
  so you can still tell them apart.
- To show your own text instead, give it a value:
  `{x_cue: Quiet, then build up}`. With a value, the cue may also stand
  before a line with chords only, for example in a solo.
- Without a value, lines with chords only are skipped, and the cue uses the
  next line with words.
- A cue belongs to the next lyric line in the same section. You can also put
  it just before `{start_of_…}`, after the previous section's `{end_of_…}`.
  A cue with no lyric line after it in its section is not shown.
- `{x_cue}` works only in the chart built from the lyrics. With an explicit
  `x_chart`, write the words in the label, for example
  `First line: C G Am F`. The label ends at the first `:`.

### Limits

- A song file can be up to 64 KB. Parsing stops at 2000 lines or 48 KB of
  text. Bytes past 1024 in one line are dropped. Content past a limit is
  dropped and an error is logged.
- Before a song opens, the device checks that it has enough free memory for
  the song and both views. If not, the screen shows "Not enough memory"
  instead of the song. A typical song of a few KB usually needs much less
  than 32 KB.
- If a song still does not fit in the space reserved for its view, the end of
  the song is not shown and the status line says "cut short".
- An explicit chord chart (`x_chart`) can have up to 64 lines; later lines
  are dropped.
- The Diagrams view shows up to 48 different chords per song.
- A setlist file can be up to 16 KB and list up to 200 songs.
- Only the first 200 files of each folder are read.
- Opening a setlist reads the header of every song in it, so a very long
  setlist or **All songs** with many files can take a few seconds to open.

## Views

A song that you open from the list starts in the Chords view. Hold Confirm
to go to the next view: Chords, then Lyrics, then Diagrams, then Chords
again. Left/Right keep the current view. If a song has no chords, the
Diagrams view is skipped. If the Chords view would be empty too, the lyrics
are shown instead, and holding Confirm does nothing.

- **Lyrics** shows the full lyrics with each chord above the place where it
  is played. Long lines wrap with their chords. Choruses have a bar in the
  left margin. A marker in the right margin shows the position in the song.
- **Chords** shows only the section names and chord sequences. Lines with the
  same chords are shown once with `×N`. A section that repeats earlier chords
  is shown as one line, plus any `{x_cue}` words. Words from `{x_cue}` are
  shown above the chords of their line. The view picks the largest font, up
  to the medium size, and one or two columns, that fit the song on one
  screen. If the song does not fit even at the smallest size, it uses pages.
- **Diagrams** shows how to play each chord of the song on one instrument.
  See [Chord diagrams](#chord-diagrams).

The header shows the position in the setlist, the title, the key, the
artist, the view (in Diagrams, the instrument), and the playback state.

## Chord diagrams

The Diagrams view shows each chord of the song once: first the chords of
the lyrics in the order they are used, then any other chords of an `x_chart`
block. In the Diagrams view, a short press of Confirm changes the instrument.
The Confirm label shows the next instrument.

| Instrument | Strings | What a diagram shows |
| --- | --- | --- |
| Guitar | E A D G B E | A common chord shape |
| Ukulele | G C E A | A common chord shape. Soprano, concert, and tenor ukuleles use the same shapes |
| Baritone | D G B E | A baritone ukulele shape |
| Bass | E A D G | Every note of the chord within five frets (see below) |
| Bass 5 | B E A D G | The same, for a 5-string bass |

The device remembers the instrument for all songs, also after a restart.

How to read a diagram:

- Strings are drawn from left to right in the order shown in the table. A
  thick line at the top is the nut.
- `×` above a string means "do not play it". `○` means "play it open".
- A bar across several strings is a barre. When a shape starts higher on the
  neck, the number to the right of the grid is the fret of the top row.
- The shape is for the chord as written. With `{capo}`, play the shape
  relative to the capo.

A bass diagram is a map of notes to choose from, not a shape to play at once:

- It covers five frets. It starts one fret below the bass note, on the low
  string (E or A, or also B on a 5-string bass) where that note has the
  lowest fret. Near the nut it starts at the nut.
- Solid dots are the bass note (the root, or the note after `/`), in each
  octave within the map. Rings are the other notes of the chord.
- A mark above the nut is an open string: solid for the bass note, a ring for
  another chord note.
- The leftmost solid dot is the lowest note of the map. Strings to its left,
  and positions without a mark, are not used.

Chord names:

- `#`, `b`, `♯`, and `♭` work. `H` means B, as in Russian and German
  songbooks. `B` also means B (not B♭).
- Known chord types: major, `m`, `7`, `m7`, `maj7`, `6`, `m6`, `9`, `add9`,
  `sus2`, `sus4`, `7sus4`, `dim`, `dim7`, `aug`, and `m7b5`, including
  common spellings such as `-`, `M7`, `Δ`, `°`, and `ø`.
- The same chord under two names, such as `A#` and `Bb`, is shown once,
  under the name used first.
- A slash chord such as `C/E` shows the shape of `C`. On bass, the map is
  built around the note after `/`, and that note is solid, even when it is
  not in the chord (as G in `Am/G`).
- A chord with an unknown type, such as `C13`, is shown by name, with `?`
  instead of a diagram. A word that starts with A–H and has no space, such
  as `Coda` in an `x_chart` line, is shown the same way.
- Annotations (`[*Riff]`), names that do not start with a note, such as
  `N.C.`, and text with a space, such as `[Bridge - quiet]`, are not chords
  and are not shown.
- On ukulele and baritone, a 9th chord leaves out the root or the fifth,
  because four strings cannot hold all five notes.

If the chords do not fit on one screen, the view uses smaller diagrams, and
then pages. A song can show up to 48 different chords; more are not shown,
and the header says "cut short".

## Auto-scroll

Play does not change the view. In the Lyrics view, auto-scroll moves the
text about one third of the screen at a time, so you never need to turn pages
while playing. In the Chords and Diagrams views the timer runs, but a view
with several pages does not turn pages by itself.

1. The song's duration comes from `{duration}`.
2. Each step happens when a reader at a steady pace would be about 2/3 down
   the screen. The step time is `duration × (step top + 1/3 screen) / text height`.
3. Time is measured from the start of playback. Pauses are not counted.
   Slow screen refreshes do not add up, because the position is computed
   from elapsed time each time.
4. At the last screen the scroll stops. The next song never opens by itself.

The device does not go to sleep while a song is playing. When you pause or
stop, or when the song ends, the normal sleep timeout starts again. A paused
song can therefore put the device to sleep if it stays paused longer than the
sleep timeout.

## Buttons (X3 / X4)

The front buttons follow your button remapping. The side buttons follow
**Side button layout**. With Prev/Prev or Next/Next, which give only one
direction, Songbook uses the top side button for back and the bottom one for
forward. When the side buttons are disabled, there is no manual stepping, and
a chord chart or a Diagrams view with several pages shows only its first
page.

| Button | Stopped or ended | Playing | Paused |
| --- | --- | --- | --- |
| Confirm (short) | Play. Diagrams: next instrument | Pause. Diagrams: next instrument | Resume. Diagrams: next instrument |
| Confirm (hold) | Next view | Next view | Next view |
| Side page buttons | Lyrics: scroll one step. Chords and Diagrams: change page | Lyrics: one step, and the timer moves with it. Chords and Diagrams: change page | Same as Playing |
| Left / Right | Previous / next song | Previous / next song, which starts playing | Previous / next song (stopped) |
| Back (short) | Back to the song list | Ignored, shows a hint | Stop (back to the top) |
| Back (hold) | Back to the song list | Back to the song list | Back to the song list |

When you switch songs while a song is playing, the new song starts playing
from the top at once. When you switch while stopped or paused, the new song
opens stopped at the top; press Confirm to start it. The view and the
instrument stay the same. If the new song cannot be opened, nothing plays.

In the Diagrams view, Confirm changes the instrument, so it cannot pause a
playing song. Hold Confirm to go to the Chords view, then press Confirm.

Power and the screenshot combination (Power + Down) keep their normal
system actions. On boards with a Home key or touch, the Home gesture is
ignored while a song is playing (a hint is shown). A Bluetooth page turner
does not work in Songbook.

Playing from the start takes two presses: select the song in the list, then
press Confirm.

## Known limits

- Portrait only. The reader orientation setting is not used.
- The Cover Grid Home theme has no Songbook item.
- On boards with a frontlight, the timer keeps running while the frontlight
  panel is open over a playing song.
- No transposing.
- Each chord has one diagram. You cannot choose another shape or add your
  own; `{define}` in a song is ignored.
- Tab and grid text uses the normal proportional font, so columns in a tab
  may not line up.
- Very long words that are wider than the screen are clipped at the edge.
