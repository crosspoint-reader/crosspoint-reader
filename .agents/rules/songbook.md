## Songbook (fork feature)

Songbook shows ChordPro songs from `/songbook/` on the SD card for live
playing: setlists, a chords-only chart, lyrics with chords, and timed
auto-scroll. It belongs to this fork only. By `SCOPE.md` it is not an upstream
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
- **Side buttons.**
  - Stepping uses `PageForward`/`PageBack`.
  - Prev/Prev and Next/Next layouts fall back to the fixed Up/Down buttons, orientation-aware via `isNavDirectionSwapped()`.
  - Disabled stays disabled.
- **Parser.**
  - Extensions go in ChordPro's `x_` namespace, like `{start_of_x_chart}` and `{x_cue}`.
  - A `Cue` line is drawn only in the derived chart, also under a "(same chords)" label. A pending `{x_cue}` attaches before the next lyric line in the same section (a bare cue skips chord-only lines and shares that line's text reference). `closeSection` drops a cue pending inside an explicit section; one placed between sections, even after a stray comment, waits for the next section. The cue needs no extra reserved line, because the `{x_cue}` source line adds no line of its own.
  - Unknown directives are counted and skipped, never errors.
  - Keep `MAX_*` limits bounded. `MAX_CHORDS_PER_LINE` sizes an on-stack array, so the parser object must stay under the stack budget.
- **Strings.** Every visible string is a `tr(STR_SONGBOOK_*)`. Update
  `docs/songbook.md` when behaviour or buttons change.

### Verification

- **Host tests** (parser, layout, timer, setlists, and the shipped samples through `SampleContentTest`):
  `cmake --build build/test --target SongbookTest && ./build/test/songbook/SongbookTest`
- **Firmware build:** `pio run -e default` with the pinned pioarduino core.
- **Screens and input:** use the `songbook-simulator` skill for the X3 desktop simulator and scripted screenshots.
- **Hardware is still required.** The simulator does not model heap, e-ink ghosting, or refresh time. Device checks must cover:
  - heap (`MEM` free and MaxAlloc across repeated song switches);
  - ghosting across about 15 steps;
  - all four orientations with remapped buttons.
