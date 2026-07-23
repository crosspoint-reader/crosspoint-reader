# Touch and UI Development

CrossPoint now runs on touch devices (Seeed Sticky, M5Paper, LilyGo T5) alongside the button-only Xteink X3/X4. Every screen must work with both input styles. There are two supported ways to get there:

1. **New screens: build them with FreeInkUI components.** Touch hit-testing, tap highlighting, long-press, and button focus navigation come with the component; you never hand-roll coordinate math.
2. **Existing screens and in-flight features: use the MappedInputManager touch bridge.** A small set of helpers adds tap/hold/swipe support to hand-rolled rendering without restructuring the activity.

If you are starting a new activity, use FreeInkUI. If you have a feature branch with a hand-rolled screen already working on buttons, use the bridge; do not rewrite mid-flight.

---

## Path 1: New screens with FreeInkUI

FreeInkUI (`freeink-sdk/libs/ui/FreeInkUI`, namespace `freeink::ui`) is an immediate-mode component library. The core idea:

- While a component renders, it registers its tappable areas ("hit rects") into the frame's interaction buffer via `Frame::hit(rect, action, value, inputMask, state)`.
- Each loop you build an `InputSnapshot` from the input manager and route it against that buffer. If a tap (or mapped button press) lands in a registered rect, the routed `ActionEvent` tells you which action fired and with what value.
- Components query `frame.stateFor(action, value)` while painting, so touch-down highlight and focus states render correctly without any per-activity code.

You get touch, hold highlighting, long-press, minimum touch-target sizing, and orientation-aware coordinates for free. The same action IDs fire from physical buttons, so one code path serves both input styles.

### Component inventory

All under `freeink-sdk/libs/ui/FreeInkUI/include/components/`:

| Category | Components |
|---|---|
| Controls | `button`, `checkbox`, `slider`, `progress-bar`, `header` |
| Lists | `list` (virtualized), `table`, `dropdown`, `radio-group`, `setting-row`, `toggle-row`, `stepper-row` |
| Keyboard | `keyboard` (QWERTY/AZERTY/QWERTZ/ES layouts), `key-grid` |
| Overlays | `popup`, `option-dialog`, `context-menu`, `message-panel`, `toast` |
| Bars | `status-bar`, `tab-bar`, `reader-chrome`, `battery-indicator`, `gesture-bar`, `tap-zones` |
| Media | `book-card`, `cover-grid`, `cover-carousel`, `metric-card` |
| Text | `text-field`, `text-area` |

### Integration skeleton

The in-tree reference is [`src/activities/util/KeyboardEntryActivity.cpp`](../../src/activities/util/KeyboardEntryActivity.cpp); it drives the FreeInkUI keyboard component inside a normal Activity. The shape:

```cpp
#include <FreeInkUIGfxRenderer.h>
namespace fui = freeink::ui;

// Render step: draw the component and (re)register its hit rects.
fui::GfxRendererTarget target(renderer);           // adapts GfxRenderer to FreeInkUI's DrawTarget
target.setFont(fui::GfxRendererTarget::FONT_BODY, UI_12_FONT_ID);
const fui::DeviceContext device = target.deviceContext();  // orientation, safe area, touch sizing

fui::Frame<48> frame(target, device, fui::InputSnapshot{}, interactions);  // 48 = max hit rects
fui::KeyboardProps props;
props.layout = &currentLayout();
props.keyAction = ACTION_KEY;                      // one action id; the key is the event value
props.inputMask = fui::InputTouch | fui::InputLongPress;
fui::keyboard(frame, kbRect, props);               // draws AND registers hit rects
```

```cpp
// Input step (each loop): feed touch state, act on routed events.
int tx, ty, tapX, tapY, hx, hy;
const bool pressedDown = mappedInput.wasScreenTouchDown(tx, ty);
const bool tapped      = mappedInput.wasScreenTapped(tapX, tapY);
const bool inContact   = mappedInput.isScreenTouchHeld(hx, hy);

const auto result = touchRouter.update(interactions, pressedDown, tx, ty,
                                       tapped, tapX, tapY, inContact, millis());
if (result.event) {
  activateValue(result.event.value, result.event.longPress);
  requestUpdate();
} else if (result.activeChanged) {
  requestUpdate();  // repaint touch-down highlight
}
```

Notes:

- `Frame<N>` is templated on the max number of hit rects; the buffer is stack/static, no heap.
- `fui::TouchHoldRouter` (see `components/keyboard/keyboard.h`) synthesizes long-press while the finger is still down and swallows the eventual release. Use it whenever a component distinguishes tap from long-press.
- For simpler screens without long-press, `FreeInkApp` / `Screen<N>` (`FreeInkApp.h`) is the ergonomic layer: it owns the interaction buffer, offers a top-to-bottom builder (`takeTop`, footer helpers, etc.), and its `render(input)` call routes the snapshot and dispatches registered action callbacks in one step. `snapshotFrom(input, device)` in `FreeInkUIInputManager.h` builds the `InputSnapshot` with orientation-aware touch mapping.
- Everything stays allocation-free and works on the button-only devices unchanged: physical buttons route through the same interaction table via each hit rect's `inputMask`.

---

## Path 2: Adding touch to existing hand-rolled screens

For activities that draw their own rows, menus, and buttons, `MappedInputManager` ([src/MappedInputManager.h](../../src/MappedInputManager.h)) exposes bridge helpers. They all return **logical screen coordinates** (orientation already applied via `GfxRenderer::tapToLogical`); never touch raw normalized panel coordinates or the SDK `InputManager` directly.

| Helper | Use for |
|---|---|
| `wasScreenTapped(x, y)` | A completed tap (press + release), with logical coords |
| `wasScreenTouchDown(x, y)` | Touch-down (held > 90 ms, not yet released): draw selection highlight |
| `isScreenTouchHeld(x, y)` | Live contact position while the finger is down (drag tracking) |
| `wasTapInRect(x, y, w, h)` | One-off hit test on a rectangle (a single button, a banner) |
| `wasListItemTapped(index, count, selected, listTop, listHeight, hasSubtitle)` | Taps on a standard UITheme list; does the row/paging math for you |
| `wasListItemTouchedDown(...)` | Same geometry, touch-down phase (highlight before activate) |
| `rowTouch(row, top, rowStep, rowCount, xStart, xEnd, rowHeight)` | Any custom band of equal-height rows; returns `RowTouch::None/Down/Tap` |
| `colTouch(col, left, colStep, colCount, yStart, yEnd, colWidth)` | Horizontal button bands (dialogs, prompts) |
| `wasSwipe()` | Raw swipe direction if you need one beyond the global gestures |
| `hasTouch()` | True when the device has a touch panel (rarely needed; helpers simply never fire without one) |

### Pattern A: standard themed list

One call per loop; UITheme owns the row geometry ([EpubReaderBookmarksActivity.cpp:123](../../src/activities/reader/EpubReaderBookmarksActivity.cpp)):

```cpp
int tapped = -1;
if (mappedInput.wasListItemTapped(tapped, bookmarks.size(), selectorIndex, listY, listHeight, true)) {
  selectorIndex = tapped;
  openBookmark();
  return;
}
```

### Pattern B: custom rows with hold highlight

For non-theme row layouts, use `rowTouch` and distinguish `Down` (highlight) from `Tap` (activate), as in [EpubReaderFootnotesActivity.cpp](../../src/activities/reader/EpubReaderFootnotesActivity.cpp):

```cpp
int row = -1;
const auto touch = mappedInput.rowTouch(row, listTop, lineHeight, visibleCount,
                                        contentX, contentX + contentWidth);
if (touch != MappedInputManager::RowTouch::None) {
  const int touched = scrollOffset + row;
  if (touch == MappedInputManager::RowTouch::Down) {
    if (selectedIndex != touched) { selectedIndex = touched; requestUpdate(); }
  } else {  // RowTouch::Tap
    selectedIndex = touched;
    activateSelection();
  }
  return;
}
```

The `Down` state exists because e-ink repaints are slow: highlight on touch-down gives immediate feedback, activation happens on release.

### Global gestures: do not reimplement these

Three gestures are handled once, for every screen. Activities must not add their own edge-swipe handling:

| Gesture | Trigger | Where it is handled |
|---|---|---|
| Back | Right-swipe starting in the left 25% of the screen | Folded into `Button::Back`, so the existing `wasPressed(Button::Back)` in your activity already fires |
| Home | Up-swipe starting in the bottom 14% | `ActivityManager::loop()`; pops to Home (activities can override via `handleHomeGesture()`) |
| Menu | Down-swipe starting in the top 14% | Activities that have a menu check `wasMenuGesture()` themselves (the reader does this) |

Because the back gesture arrives as `Button::Back`, most button-era activities gain back-swipe support with zero changes. That is the bar to aim for: bridge helpers should make touch an additive layer over the button flow, not a second input state machine.

### Bridge rules

- Handle touch in `loop()` next to the existing button handling, one helper call per interaction zone, and `return` after consuming an event (mirrors the button pattern).
- Never call the SDK `InputManager` or read GPIO directly; the HAL rule from the main guide applies to touch too.
- Coordinates from the helpers are logical and orientation-correct on all four rotations; test at least Portrait and one Landscape mode before PR.
- Nothing to clean up in `onExit()`; the helpers are stateless from the activity's point of view.

---

## Building and testing on non-Xteink devices

Each MCU family is its own binary: X3/X4 are ESP32-C3, Sticky and LilyGo T5 are ESP32-S3, M5Paper v1.1 is a classic ESP32. The Sticky env ships in `platformio.ini` (`pio run -e sticky`). Envs for other devices go in **`platformio.local.ini`**, a gitignored file that PlatformIO merges over `platformio.ini` (see `extra_configs`). Create it next to `platformio.ini`; personal envs, ports, and debug flags live there and never get committed.

Both envs below extend the repo's `[base]`, so they build against the `freeink-sdk` submodule with all the normal deps and scripts.

### M5Paper v1.1 (classic ESP32, IT8951 panel)

```ini
[env:m5paper_v11]
extends = base
board = esp32dev
board_build.mcu = esp32
board_build.flash_mode = qio
; CP2104 UART bridge: 921600 drops out on macOS after the stub baud switch
upload_speed = 460800
build_unflags =
  ${base.build_unflags}
  ; classic ESP32 has UART serial, not USB CDC; Logging.h keys off these
  -DARDUINO_USB_MODE=1
  -DARDUINO_USB_CDC_ON_BOOT=1
build_flags =
  ${base.build_flags}
  -DFREEINK_DEVICE_M5PAPER=1
  ; the 63KB 540x960 framebuffer lives in PSRAM (FREEINK_FB_PSRAM auto-on)
  -DBOARD_HAS_PSRAM
  -DCROSSPOINT_VERSION=\"${crosspoint.version}-m5paper\"
  -DENABLE_SERIAL_LOG
  -DLOG_LEVEL=2
  ; touch-first device: hide front-button hint labels
  -DCROSSPOINT_SHOW_BUTTON_HINTS=0
  ; archive-scan-order workaround: without these a full relink drops Wire's i2c symbols
  -Wl,-u,i2cInit
  -Wl,-u,i2cSlaveInit
```

### LilyGo T5 S3 (ESP32-S3, controller-less panel via LovyanGFX)

```ini
[env:lilygo_t5s3]
extends = base
board = esp32-s3-devkitc1-n16r8
board_build.mcu = esp32s3
build_flags =
  ${base.build_flags}
  -DFREEINK_DEVICE_LILYGO=1
  ; board injects the parallel-bus pins + PMIC power hooks (BoardT5S3)
  -DFREEINK_LGFX_EPD_CONFIG=lilygoT5S3LgfxConfig
  -DCROSSPOINT_VERSION=\"${crosspoint.version}-lilygo\"
  -DENABLE_SERIAL_LOG
  -DLOG_LEVEL=2
  -DCROSSPOINT_SHOW_BUTTON_HINTS=0
lib_deps =
  ${base.lib_deps}
  ; LgfxEpdConfig for the T5 S3 (pins, PCA9535/TPS65185 power sequence)
  BoardT5S3=symlink://freeink-sdk/libs/hardware/BoardT5S3
  ; LovyanGFX Panel_EPD drives the controller-less ED047TC1 panel
  m5stack/M5GFX @ 0.2.20
```

Then `pio run -e m5paper_v11 -t upload` (or `-e lilygo_t5s3`). Gotchas worth knowing:

- **Flash mode matters.** The M5Paper is `qio`; the X4-family standalone envs need `dio`. A wrong flash-mode header boots into a `partition 0 invalid magic number 0xffff` loop even though esptool verified the write.
- **One `FREEINK_DEVICE_*` flag per env** selects the board profile (pins, panel, touch controller) from the SDK's `BoardConfig`. See `freeink-sdk/platformio.sample.ini` for reference envs of every supported device.
- **Serial logs:** `[base]` does not enable logging; without `-DENABLE_SERIAL_LOG` a non-default env prints nothing.
- No touch hardware on your desk? The X4 build still exercises the same code paths through buttons; touch-specific behavior (tap zones, gestures) needs a real device.
