# Touch Architecture

Two boards have a touch panel: the Xteink X4 Pro and the LilyGo T5S3, both with a GT911
controller and a capacitive Home key. This page is for contributors changing touch code. What
each gesture does for the user is in [Touch Gestures](../touch-gestures.md); board hardware is in
[Board Support](./board-support.md).

## Layers

```
InputManager (SDK)     GT911 over I2C; classifies tap, swipe, long press, multi-touch
     | normalized 0..1 coordinates, panel-native frame
HalGPIO                passthrough; latches events across ticks; Home key -> button presses
     | still normalized, still panel-native
MappedInputManager     orientation -> logical pixels; named gestures; the UI touch gate
     | logical pixels
GestureEventManager    gestures the user bound to a BUTTON_ACTION (called from main.cpp)
Activities             FreeInkUI interaction tables, the recorders, or their own hit tests
```

- `HalGPIO` (`lib/hal`) exposes the SDK capability and nothing more: `hasTouch()`,
  `wasTouchTap()`, `wasTouchLongPress()`, `isTouchHeldAt()`, `suppressTouchContact()` and so on,
  with no orientation and no meaning attached. Every SDK touch method is guarded by
  `FREEINK_CAP_TOUCH` inside the SDK and compiles to `false` on the C3, so callers need no `#if`.
- `MappedInputManager` (`src`) holds a `GfxRenderer&` so the transform always uses the renderer's
  orientation. It reports logical pixels (`wasScreenTapped()`, `wasSwipe()`, `listTouch()`,
  `popMultiTouch()`), the edge gestures (`wasMenuGesture()`, `wasLightPanelGesture()`,
  `wasHomeGesture()`), and routes every event query through private `raw*()` wrappers that apply
  the UI gate.
- Names and signatures in both layers follow upstream crosspoint-reader so screens stay
  diff-comparable. Keep it that way when adding methods.

The SDK classifies in panel-native pixels: tap slop 28 px, a swipe needs at least 60 px within
700 ms, a long press fires at 500 ms (`InputManager.h`). There is no double tap anywhere in the
stack. Only pinch and rotation are wired among the two-finger gestures; a two-finger swipe goes to
an SDK queue nothing reads, and it suppresses single-finger classification until every finger is
lifted.

## Coordinate transform

`GfxRenderer::tapToLogical()` maps the SDK's normalized panel-native point into logical pixels for
the orientation being drawn. The math lives in `lib/GfxRenderer/TouchTransform.h`, free of
Arduino, so `test/touch_transform` checks all four orientations on the host. A `static_assert` in
`GfxRenderer.cpp` keeps its orientation enum in step with `GfxRenderer::Orientation`. The board
profile has already corrected for how the digitiser is mounted (`swapXY`, `flipX`, `flipY`), so
the transform only handles the user's orientation. The point is clamped onto the panel before
rotation, so the result is always on screen.

A wrong rotation branch makes taps land mirrored or on the wrong axis, which is slow to find on a
device and quick to catch on the host. Change `TouchTransform.h` only with those tests green.

`tapToLogical(Orientation, ...)` maps into an explicit frame instead of the live one. Use it for
geometry drawn in a fixed frame whatever the screen's rotation: the button hints are always drawn
in Portrait, so their hit test resolves taps into Portrait (`wasScreenTappedIn()`).

## I2C bus and the input sampler

`HalGPIO::sampleOnce()` runs on the `btnsample` task (priority 2, every 10 ms) and calls
`inputMgr.update()`. On a touch board `update()` also runs the SDK's private `serviceTouch()`,
which is a GT911 I2C transaction. `update()` is the only public entry point and always services
touch, so touch I2C runs wherever `update()` runs; moving it to another task would need an SDK
change.

The loop task meanwhile drives the RTC (`HalClock`) and the fuel gauge (`HalPowerManager`) on the
same bus. On the X4 Pro all three devices share bus 0 on SDA39/SCL38: GT911 at 0x5D, BM8563 RTC at
0x51, CW2017 gauge at 0x63. The board wires them to one pin pair, so the profile's per-device bus
field cannot separate them, and two tasks on one bus corrupt each other's transactions. So:

- `HalI2cBus::Lock` is a recursive, fail-closed mutex taken by every I2C user: `sampleOnce()`
  around `update()`, `HalClock`, and the gauge read. Without touch it compiles to an empty class,
  because nothing shares the bus across tasks.
- `sampleOnce()` releases the I2C lock before its `portENTER_CRITICAL(&inputMux_)` section. The
  mutex must never be held inside a critical section.
- The sampler stack is 4096 bytes with `FREEINK_CAP_TOUCH` and 2048 without
  (`HalGPIO::startInputSampler()`). 4096 is the SDK's own figure for a task that calls `update()`
  (`InputManager::beginAsync()`). Measured peaks were about 540 bytes on the X3 and X4 (2026-08-16)
  and 2088 bytes on the T5S3 (2026-08-17), so 2048 would have overflowed there. Watch the
  `btnSampler stack high-water` line after changing anything on this path.
- The SDK's own input task (`beginAsync()`) is not used. It reports a button on its press edge
  only, with no release edge or timestamp, which `ButtonEventManager` needs; and running it beside
  `btnsample` would service the input machine twice.

Who starts the bus, and the T5S3's shared board mutex, are in
[Board Support](./board-support.md#bus-ownership).

## Touch events are latched

The SDK reports tap, swipe and long press as one-shot flags that its next `update()` clears,
about 10 ms of life at the sampler's cadence. `HalGPIO` latches each single-finger event into a
ring on the sampler task (`latchTouchEvents()`), and `update()` on the loop task moves one event
per tick into a snapshot.

- Each event is visible for exactly one loop tick, like a button press.
- Reads within that tick do not consume. The gesture classifier looks at a tap, decides, and only
  then suppresses it before the screen underneath reads it; a pop-per-reader queue would break
  that.
- `suppressTouchContact()` also clears the ring and the snapshot. The SDK can only stop events it
  has not produced yet, and after a slow tick the tap that follows a long press may already be
  queued.
- Activity transitions flush the ring (`ButtonEventManager::drain()` calls `flushTouchEvents()`),
  and so does turning the UI gate off.
- Level reads (`isTouchHeldAt()`, `isTouchTapCandidate()`) stay live: "where is the finger now"
  has no latched meaning.

Swipes made during a slow refresh therefore queue and replay one per tick, as button presses do.
Before latching, about one tap in fifteen was lost on an idle menu, most likely while the loop
light-slept between polls (T5S3, 2026-09).

## How screens receive touch

One loop tick handles touch in this order:

1. `main.cpp`: `GestureEventManager::consumeAction()`. A bound gesture runs through the same
   `runAction()` as buttons and suppresses the contact.
2. The activity's own `loop()`: reader page turns, FreeInkUI routing, screen-specific hit tests.
3. `ActivityManager::loop()`, on touch builds only: `dispatchLightPanelGesture()`,
   `dispatchListSwipe()`, `dispatchListTap()`, `dispatchHintStripTap()`.

The dispatchers in step 3 are fallbacks: a screen that handles a tap itself claims it first.

A screen becomes tappable in one of three ways.

**FreeInkUI.** Screens built on `UiListActivity`, `TabbedUiListActivity` or `UiAppHost` register
interaction rectangles while they draw, and `UiAppHost::routeTouch()` routes the contact against
the last published table. The on-screen keyboard uses FreeInkUI's interaction table the same way,
through its own router. This path also gives tap feedback: FreeInkUI paints the tapped element
gray in the same refresh that shows the result (`FreeInkApp::route()`). A handler that moves to
another screen calls `app.clearTapFlash()`, or the new screen's element with the same action
inherits the gray.

**Recorders**, for screens that draw their own rows. The draw records what it painted and the
input side matches against that record:

| Recorder | Shape | Resolves to |
|---|---|---|
| `ButtonHintStrip` | the four bottom hints and the two side hints | a raw button index |
| `ListTouchBand` | a contiguous run of full-width rows, heights may differ | an item index |
| `TapTargets` | arbitrary rectangles: Home covers, Home menu, links on the reader page, the Exit button of the connection screens | a value |

Recorders are written on the render task and read on the loop task, so they publish without
tearing. All of them are cleared on every activity transition. The list band and the Home covers
and menu are also cleared at the start of every render pass, so they describe exactly the last
frame; a screen can stop drawing its list without changing activity. The others are not:

- `TapTargets::readerLinks()`: the reader has passes that draw without displaying (the
  pre-render), so its links are republished by every path that puts a page on screen.
- `TapTargets::screenActions()`, the Exit button of the connection screens
  (`components/TouchExitButton.h`): File Transfer paints its screen once and then releases the
  frame buffer, so its later passes draw nothing and the button has to outlive them. A screen that
  stops offering the button calls `TouchExitButton::forget()`.
- The button hint strips, which a screen records again whenever it draws them.

**Their own hit test**, for shapes too large or too regular to record: the file browser's cover
grid (`CoverGridLayout::hitTest()`), sliders (`SliderGeometry`) and the dictionary word selector.

Where geometry is computed by a layout helper, the draw and the hit test (or the recorder) must
both read that helper; the carousel's `CarouselCoverLayout` exists because a hand-copied recorder
disagreed with the draw. Each helper has a host test that computes a slot, hit-tests the pixel it
was drawn at, and expects the same item back (`test/cover_grid_layout`,
`test/carousel_cover_layout`, `test/slider_geometry`). Test more than one panel size: the
carousel's left tile hangs off the edge only on the 540 px T5S3 panel, which went unnoticed while
every test used 480 px.

The other host suites for this layer are `test/touch_transform`, `test/tap_zones`,
`test/button_hint_strip`, `test/list_touch_band`, `test/tap_targets`, `test/list_row_tap` and
`test/link_marker_match`.

### The reader

The reading page has no widgets, so readers read touch directly through the helpers in
`ReaderUtils.h`: `detectTouchPageTurn()` (outer-third taps, or horizontal swipes, per
`touchReaderControls`) and `isTouchMenuGesture()` (a centre tap or the menu edge swipe), which
opens the reader's menu or, in the readers without one, their own navigation. The menu edge is
the bottom edge on a board with a light and the top edge otherwise (`wasMenuGesture()`). In the EPUB
reader a tap on a link (footnote marker or cross-reference) is tested first, because a marker can
sit inside a page-turn zone; the markers are located during the render by matching the page's
words (`LinkMarkerMatch`). Gesture actions reach a reader through its `onButtonAction()`, so every
reader must implement one.

### The hint strip

`drawButtonHints()` and `drawSideButtonHints()` record where their boxes landed
(`ButtonHintStrip.h`), and `ActivityManager::dispatchHintStripTap()` turns a tap on a box into a
press of the button it shows. Every screen that draws hints is tappable without per-screen code.

- Box *i* is raw button *i*. `mapLabels()` emits its labels in
  `{BTN_BACK, BTN_CONFIRM, BTN_LEFT, BTN_RIGHT}` order and permutes only the text, so a hit needs
  no mapping and the user's button remapping still applies.
- The tap is injected as a raw press (`MappedInputManager::injectRawPress()`, then
  `HalGPIO::injectPress()`) into the accumulators and edge queue a physical button uses, so
  `wasPressed()` readers and `ButtonEventManager` both see an ordinary press. Inject synthetic
  input at that level or lower; a press pushed only into the edge queue is invisible to the
  bitmask readers.
- A long tap on a box injects a long press, its press edge backdated by
  `HalGPIO::INJECTED_LONG_PRESS_MS`; `ActivityManager.cpp` checks that against
  `ButtonEventManager::LONG_PRESS_MS` with a `static_assert`. On boards whose Back and Confirm come
  from the Home key, which reports press and release in one pass, this is the only way to hold
  those buttons.
- Coordinates are in the Portrait frame, because the hints are always drawn in Portrait.
- An empty label marks its box inactive: not drawn and not tappable. On a board whose only keys
  are these boxes, an empty label leaves that action unreachable, so label it.

## Design rules

### A gesture is a button

Every gesture (swipes, tap zones, long-tap zones, corner holds, pinch, rotate) carries a
`CrossPointSettings::BUTTON_ACTION`, chosen from the same option list a physical key offers.
`GestureEventManager` classifies the contact and resolves the action, and `main.cpp` runs it
through the same `runAction()` as buttons, so a gesture and a button bound to the same action
cannot drift apart.

- `BTN_DEFAULT` means "leave this contact to the screen". `consumeAction()` never claims a contact
  whose gesture is unbound, and when nothing at all is bound it does not look at the contact.
- When a bound gesture fires, `suppressTouchContact()` stops the same contact also reaching the
  screen, so one tap cannot be both a gesture and a page turn.
- Outside the reader a gesture bound to a reader-scoped action
  (`CrossPointSettings::isReaderScopedAction()`) is declined, not swallowed, so the screen
  underneath still gets its touch.
- In the reader every gesture is live. Elsewhere taps and the five long-tap zones belong to the
  screen, while swipes, two-finger gestures and the four corner holds stay live. A zone is a third
  of the glass and would sit on whatever a screen draws; a corner is small, at an extremity, and
  outside the reader only the hint strip also uses holds. That is what lets the corner light toggle
  mean the same on every screen.
- Tap zones stay Built-in by default. The reader's own tap path implements the
  `touchReaderControls` modes, which a direct `BTN_PAGE_BACK` binding would bypass.

### Ordering

- `BUTTON_ACTION` is positional. `SettingsList` builds each option list by position (index *i* is
  value *i*), and stored settings hold the value. Board-gated actions (the light block, then the
  warm/cool pair, a strict subset of it) stay at the end of the enum in narrowing order, so
  dropping them on a board without the hardware renumbers nothing. `buildSettings()` asserts
  the list length against `BUTTON_ACTION_COUNT`, because the failure is a silent shift of every
  stored mapping, not a crash.
- A new unconditional action goes above the gated block, which shifts the gated values by one.
  Stored gesture mappings are then reset with a `GESTURE_DEFAULTS_VERSION` bump (version 2 was
  exactly this); button mappings have no such stamp.
- A default must not name a board-gated action unless `CrossPointSettings::dropUnsupportedActions()`
  cleans up after it. `main.cpp` calls it right after `Frontlight.begin()`, which also rescues an
  SD card moved from a board with a light to one without.
- `ActivityManager::loop()` resolves a list swipe before a list tap: the SDK reports both for a
  contact whose travel sits near the threshold, and the swipe suppresses the contact when it
  claims it. The light-panel edge swipe goes before both, so a full-screen list does not read it
  as a page-down. `GestureEventManager` takes multi-touch first, then long press before tap, and
  corners before zones, because a corner sits inside a zone.

### Orientation is sampled once

`getScreenWidth()` and `tapToLogical()` without an orientation argument read the live draw
orientation, which the themes flip to Portrait mid-pass to draw the hint strips. Loop-task code
that reads width, then height, then maps a tap can resolve each against a different frame. Read
`renderer.getHeldOrientation()` once and pass it to the explicit overloads (`getScreenWidth(o)`,
`wasSwipeIn(o)`, `wasScreenTappedIn(o)`, `peekScreenLongPressIn(o)`), as
`GestureEventManager::consumeAction()` and `MappedInputManager::wasEdgeSwipe()` do.

### T5S3 single-push grayscale

The T5S3's panel driver has a fast grayscale bank whose grey levels assume nothing has driven the
pixel first, so anti-aliased text must reach the panel in one push (`supportsGrayFrame()`).
Pushing the B/W page and then the grey overlay flashed the anti-aliased edges and left them too
light.

- The reader captures both gray planes during the page render (`GfxRenderer::beginGrayCapture()`)
  and shows the page with one `displayGrayscaleFrame()`. The two plane buffers (about 130 KB on
  this panel) are allocated per render without throwing; when they cannot be had, the staged path
  renders the planes separately.
- Planes and page both use the framebuffer as scratch, and the page must be what is left in it.
  So staged planes are rendered before the page, and the pre-render hands its planes to the panel
  during the pre-render, never at display time.
- Leaving the reader on this panel always arms a full refresh
  (`ReaderUtils::enforceExitFullRefresh()`), anti-aliased or not, because every push leaves the
  panel's canvas holding the page.

More in [LilyGo T5 S3 display stack](../lilygo-t5s3-display-stack.md).

### Changed defaults need a version bump

`BTN_DEFAULT` is 0, so "the user chose Built-in" and "this key was saved before that default
existed" are the same byte on disk, and the stored value wins. A changed gesture default would
never reach a device that has saved its settings once, and the symptom, a gesture that does
nothing, looks like a firmware bug.

The settings file therefore carries a `gestureDefaultsV` stamp. When it is older than
`CrossPointSettings::GESTURE_DEFAULTS_VERSION` (currently 3), `JsonSettingsIO` ignores every
gesture key and keeps the compiled defaults. Bump it when a gesture default or a gesture's meaning
changes. A bump discards users' gesture customisation, so stop bumping once the defaults have
settled. Any other setting whose unset value is also a meaningful choice needs the same before its
default can change; for a single flag, renaming the JSON key is the precedent
(`fastAntiAliasing` to `fastAntiAliasingV2`).

### A list is tappable only if its screen says so

`ActivityManager::dispatchListTap()` resolves a tap to a row from the band the render published,
then calls `Activity::selectListRow()`, whose base implementation returns `Rejected`. A screen that
records rows without overriding it resolves the right row and does nothing.
`EnumSelectionActivity` shipped that way. The check is mechanical: every screen that records a
list (`drawList`, `drawWrappedList`, `recordUniformRows`) overrides `selectListRow()` or derives
from `UiListActivity`. `ButtonRemapActivity` deliberately does neither: it assigns roles from the
physical key pressed, so a synthesized Confirm would record itself. The `Rejected` branch logs,
because otherwise it looks exactly like a tap that never arrived. For a new screen, ask "does it
have a selection", not "does it call `drawList`".

### Point, then confirm

A first tap on a row moves the selection there; a tap on the row already selected runs the action
(`ListRowTap`, shared by both list paths). A mis-tap costs one more tap instead of an action to
undo, and the first repaint is the feedback. The activating tap synthesizes a Confirm press rather
than calling the action, so it runs the screen's own Confirm path. Users can switch to single-tap
activation (`touchListActivation`).

Do not act on touch-down. It is reported after 90 ms of contact (`TOUCH_DOWN_SELECT_DELAY_MS`),
which an ordinary tap exceeds, so moving the selection on touch-down and acting on release costs
two refreshes for one tap. Reader links act on a single tap: a page of text has nowhere to show a
highlight, and page-back undoes the jump.

## Gesture defaults

What each gesture does by default is in [Touch Gestures](../touch-gestures.md); the source is the
initialisers in `CrossPointSettings.h`. Why they are what they are:

- Vertical swipes belong to the edge column they start in (`TapZones::edgeColumnFor()`), as in
  KOReader and Kobo, not to a screen half. Half the screen is most of the page, so a resting thumb
  could dim the light, and a half-screen swipe also shadowed the top-edge menu swipe. The start
  point decides, not the end: e-paper cannot animate a drag, so there is nothing to aim at while
  the finger moves. The edge columns stop short of the top and bottom bands so a corner swipe
  cannot mean two things.
- Left edge brightness and right edge warmth follow KOReader; the top-edge pull for the light
  panel follows Kindle and CrossInk; the bottom-edge swipe for the reader menu follows CrossInk on
  the X4 Pro. A flick inside an outer third jumps ten pages (KOReader's page jump) and works in
  either direction; requiring an outward flick left too little room toward the bezel on the T5S3
  (2026-09-11). Holding the top-left corner toggles the light (KOReader). Pinch sets the font size
  and rotate the orientation, and the two rotations are a pair (`BTN_CYCLE_ORIENTATION_BACK`), so
  turning back undoes.
- There is no left-edge back swipe. The left edge already carries the brightness column and the
  ten-page jump, and Back is the Home key hold on both touch boards.
- Light actions step by 5 %: the SDK's perceptual curve puts most of the usable range in the
  bottom third, where a 10 % step is a large jump.

## Gating

- `CP_TOUCH_UI` compiles the app-side touch layer (recorders, gesture classifier, dispatchers,
  gesture settings rows) out of builds for boards without a digitiser. It is set on the command
  line (`[c3]` sets 0) and defaults to 1 (`src/TouchUi.h`), so a new board keeps touch until
  someone says otherwise. It is not derived from `FREEINK_CAP_TOUCH`: the recorder headers are
  dependency-free for the host tests, and a macro that exists only once `BoardConfig.h` is
  included would give translation units different sizes for the same recorder struct.
  `MappedInputManager.cpp` checks the two agree with a `static_assert`.
- `touchUiControls` silences touch outside the reader. `main.cpp` sets the gate every tick from
  the setting and the activity on top, and the reader is always enabled, because
  `touchReaderControls` governs it there. `hasTouch()` is not gated: screens use it for layout.
  Nobody is stranded with the gate off: the Home key's Back and Confirm arrive as button presses
  below this layer, and `BTN_TOGGLE_TOUCH_UI` can be bound to a reader gesture.
- `touchReaderControls` (off, tap, swipe, inverted tap) governs the reading surface, link taps
  included. Off leaves the page inert, so a thumb resting on the glass cannot turn a page.

## Debugging touch

Build with `-DBUTTON_TRACE=1` (commented out in `[lilygo_board]`) to trace both ends of the path:

- `[TCH] release: held=... tap=... swipe=... travel=...` from the sampler task at the release edge,
  while the SDK's one-shot flags are fresh. The trace measures travel itself, in panel-native
  pixels, because the SDK fills in endpoints only for contacts it already classified as swipes.
- `[GEST] <gesture> -> action=... (claimed | unbound ...)` from the loop.
- `[BTN]` lines with raw button edges, expander buttons and the Home key included, before any
  mapping.

A gesture in the `[TCH]` line and missing from `[GEST]` was missed by a busy loop; one reported as
unbound is a settings question. At debug log level, `ActivityManager` also logs list taps
(`[TCH] List tap -> row ...`), rejected ones included.
