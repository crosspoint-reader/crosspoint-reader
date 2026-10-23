## Songbook (fork feature)

Songbook shows ChordPro songs from `/songbook/` on the SD card for live
playing: setlists, a chords-only chart, lyrics with chords, chord diagrams
for five instruments, and timed auto-scroll. It belongs to this fork only. By `SCOPE.md` it is not an upstream
CrossPoint feature, so never propose it upstream. User documentation is
`docs/songbook.md`; sample SD content is `docs/songbook/sd-card/`.

### Upstream touch points

Keep changes outside `src/activities/songbook/` minimal so upstream merges stay
easy. The only upstream files Songbook touches are:

- `ActivityManager`: `goToSongbook()`, `HomeMenuItem::SONGBOOK`, and the
  `goHome()` name-prefix mapping for `Songbook*` activities.
- `HomeActivity`: the `hasSongbook` row (list themes only, shown when
  `/songbook` exists) and the two index converters, which must stay in sync
  with the render order.
- `HomeActivity` menu scrolling: `menuTop()`, `menuWindow()`, and the
  `menuFirst` slice that `render()` stores and the touch grid reads, plus the
  scroll arrows. This is a behaviour change for every list theme, not a few
  lines to re-apply. It assumes the theme draws rows from the menu rect's top
  (Classic adds `verticalSpacing`, which the fit reserves for all themes). On
  an upstream change to Home rendering or `drawButtonMenu`, re-check the fit
  and touch mapping in the simulator.
- `lib/I18n/translations/*.yaml`: `STR_SONGBOOK_*` keys.
- `test/CMakeLists.txt`: the `songbook` suite.
- `AGENTS.md` (routing row) and `.agents/README.md` (rules list): point agents
  to this rule and the `songbook-simulator` skill.

When an upstream merge conflicts in the other files, re-apply the Songbook
lines on top of upstream rather than reshaping upstream code.

### Module map

| File | Role | Host-testable |
| --- | --- | --- |
| `Song.h` | One text pool plus line, chord, section, and chart tables | yes |
| `ChordProParser` | Line-at-a-time ChordPro subset; skips unknown directives | yes |
| `SongLayout` | Lyrics and chart layout behind `TextMeasurer`; scroll stops | yes |
| `AutoScrollController` | Play/pause/step timer derived from elapsed time | yes |
| `Setlist` | Setlist JSON and safe relative paths | yes |
| `ChordName` | Chord name to root, quality, and bass note | yes |
| `ChordShapes` | Shape tables, movable shapes, baritone rule, bass tone maps, instrument codes | yes |
| `SongChords` | Distinct chords of a song, in fixed storage | yes |
| `DiagramLayout` | Diagram grid: size choice, cells, pages | yes |
| `SongbookPrefs` | `/.crosspoint/songbook.json` (`PersistableStore`) | no |
| `SongbookFiles` | SD access through `HalStorage` | no |
| `SongbookActivity`, `SetlistActivity` | Lists on `SongbookListActivity` (`UiListActivity`) | no |
| `SongActivity` | Performance screen: input, header, drawing, refresh | no |

The host-testable files must not include Arduino, HAL, or renderer headers.

### Contracts to preserve

- **Memory.** With `-fno-exceptions`, a failed container growth aborts. So:
  - `loadSong` reserves every song table from a counting pass after a heap check.
  - `SongLayout` reserves `lyricsBudget`/`chartBudget` once and never grows the row, piece, or label tables past them. Extra content is dropped and `truncated()` is set; the status line then shows `STR_SONGBOOK_CUT`.
  - `SongActivity::layoutsFit` requires the largest free block to hold the two biggest tables.
  - `openSong` frees the previous song and both layouts before loading.
  - `SongChords` is a fixed array of `MAX_CHORDS` inside `SongActivity`; it never allocates. Extra chords are dropped and `truncated()` is set.
  - A diagram is computed at draw time from the `constexpr` tables; nothing is cached.
  - Keep lyric pieces merged per chord run, not per word.
- **Timer.** Position comes from elapsed play time, never from summed
  intervals, so slow e-ink refreshes cannot drift.
  - `elapsedMs` treats a timestamp earlier than the run start as zero, because a song switch calls `start(millis())` after the SD load.
  - All state changes happen in `loop()` under `RenderLock`.
  - `pending()` lets `loop()` skip the lock when nothing is due.
- **Behaviour accepted by the human owner:**
  - Left/Right change the song on a short press in every state. A switch while playing starts the new song from the top; a switch while paused or stopped opens it stopped.
  - Back while playing only shows a hint; holding Back leaves.
  - A song opened from the list starts in Chords, the performance view; Lyrics is mostly for rehearsal. Holding Confirm toggles Lyrics/Chords, Left/Right keep the view, and Play never changes it.
  - A multi-page chart does not turn pages while playing. The owner deferred that design until a real song needs it.
  - `preventAutoSleep()` is true only while playing.
  - The song never advances to the next one by itself at the end.
  - The derived chord chart has no warning label.
  - An empty derived chart (for example, no chords) shows Lyrics through `shownMode()`, and the view toggle is ignored there.
  - The chord chart (derived or `x_chart`) uses Medium fonts at most; Large chords read too big there.
  - Holding Confirm cycles Chords → Lyrics → Diagrams → Chords. Diagrams is skipped, and `shownMode()` shows Lyrics, when the song has no chords.
  - In Diagrams, a short Confirm selects the next instrument instead of Play/Pause/Resume; the timer keeps running. Back keeps its normal meaning.
  - The instrument is one global choice for all songs. Left/Right keep it and the view.
- **Side buttons.**
  - Stepping uses `PageForward`/`PageBack`.
  - Prev/Prev and Next/Next layouts fall back to the fixed Up/Down buttons, orientation-aware via `isNavDirectionSwapped()`.
  - Disabled stays disabled.
- **Chord diagrams.**
  - `ChordShapesTest` checks every chord instrument, root, and quality against chord theory: every sounding note belongs to the chord, every tone but the fifth is present (on four strings a 9th may also drop the root), guitar shapes have the root lowest, and each shape fits `DIAGRAM_FRETS`. Add a shape to a table, never an exception to the test.
  - Exact shapes win; otherwise the lowest movable shape is shifted, and its open strings become the barre.
  - Baritone uses the ukulele shape a fourth up.
  - Bass and Bass5 show a `ToneMap` (`findToneMap`), not a `ChordDiagram`: every chord tone within five frets, starting one fret below the bass note on the low string (all but the top two) where it has the lowest fret; near the nut the box starts at fret 1 and marks open strings. The bass note (root, or the note after '/') is solid and joins the tones. Nothing under the bass note is marked: lower strings, and the start string's frets below it, stay empty. `ChordShapesTest` checks every chord tone appears, nothing else is marked, the bass note is the lowest mark, and the start position for all 12 notes.
  - A barre shape's window starts at its barre; other shapes start at the nut when they fit.
  - `B` and `H` both mean B. A slash chord shows its upper chord on chord instruments.
  - `ChordAnchor::annotation` marks `[*text]`, which is not a chord. Other unknown names are shown with `?` only when they start with A–H and have no space; they never widen the cells and are clipped to theirs.
  - `DiagramLayout` owns every drawn position (grid, fret number, marks); `drawDiagram` only reads `Placement` and `Metrics`.
- **Saved instrument.**
  - `/.crosspoint/songbook.json` holds `{"instrument": "<code>"}`. Codes come from `instrumentCode()`; never persist the enum value. An unknown code falls back to guitar.
  - It loads once, on the first song opened after boot. `apply()` only sets `instrumentChanged` under `RenderLock`; `loop()` releases the lock, starts the redraw with `requestUpdate(true)`, and then writes the file.
- **Parser.**
  - Extensions go in ChordPro's `x_` namespace, like `{start_of_x_chart}` and `{x_cue}`.
  - A `Cue` line is drawn only in the derived chart, also under a "(same chords)" label. A pending `{x_cue}` attaches before the next lyric line in the same section (a bare cue skips chord-only lines and shares that line's text reference). `closeSection` drops a cue pending inside an explicit section; one placed between sections, even after a stray comment, waits for the next section. The cue needs no extra reserved line, because the `{x_cue}` source line adds no line of its own.
  - Unknown directives are counted and skipped, never errors.
  - Keep `MAX_*` limits bounded. `MAX_CHORDS_PER_LINE` sizes an on-stack array, so the parser object must stay under the stack budget.
- **Strings.** Every visible string is a `tr(STR_SONGBOOK_*)`; symbols and
  numbers (`?`, `~`, page and fret numbers) stay literal. Update
  `docs/songbook.md` when behaviour or buttons change.

### Verification

- **Host tests** (parser, layout, timer, setlists, and the shipped samples through `SampleContentTest`):
  `cmake --build build/test --target SongbookTest && ./build/test/songbook/SongbookTest`
- **Firmware build:** `pio run -e default` with the pinned pioarduino core.
- **Screens and input:** use the `songbook-simulator` skill for the X3 desktop simulator and scripted screenshots. For diagrams, check every instrument, a song with more chords than one screen, a barre shape, an unknown chord, and that the instrument survives a restart.
- **Hardware is still required.** The simulator does not model heap, e-ink ghosting, or refresh time. Device checks must cover:
  - heap (`MEM` free and MaxAlloc across repeated song switches);
  - ghosting across about 15 steps;
  - all four orientations with remapped buttons;
  - that thin diagram lines and open/muted marks read clearly on e-ink.
