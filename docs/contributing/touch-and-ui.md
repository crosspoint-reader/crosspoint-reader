# Touch and UI Development

CrossPoint runs on touch devices (Seeed Sticky, M5Paper, M5Stack PaperMono, LilyGo T5, Xteink X4 Pro) alongside the button-only Xteink X3/X4. Every screen must work with both input styles.

**There is one supported way to build a new screen: FreeInkUI, hosted through the firmware base classes below.** Touch hit-testing, tap highlighting, long-press, swipe scrolling, and button focus navigation all come from the shared stack; you never hand-roll coordinate math.

The old bridge helpers (`rowTouch`, `colTouch`, `wasTapInRect`, manual rect `contains()` checks) are legacy. They survive only for the two remaining hand-rolled surfaces (the theme-driven home screen and the reader page) and must not appear in new code. PRs that add new uses will be asked to convert.

---

## Picking the right host

| Your screen is... | Use | In-tree reference |
|---|---|---|
| A single list of rows | subclass `UiListActivity` | [`LanguageSelectActivity`](../../src/activities/settings/LanguageSelectActivity.cpp) (minimal), [`FileBrowserActivity`](../../src/activities/home/FileBrowserActivity.cpp) (long-press and lazy rows) |
| Tabbed lists | subclass `UiTabListActivity` | [`SettingsActivity`](../../src/activities/settings/SettingsActivity.cpp) |
| Custom FUI layout (sliders, prompts, state machines) | inherit `UiAppHost` directly | [`EpubReaderPercentSelectionActivity`](../../src/activities/reader/EpubReaderPercentSelectionActivity.cpp), [`WifiSelectionActivity`](../../src/activities/network/WifiSelectionActivity.cpp) |
| A modal picker or confirm inside a legacy activity | `OptionPopup` (or push `ConfirmationActivity`) | [`OtaUpdateActivity`](../../src/activities/settings/OtaUpdateActivity.cpp) |
| A Yes/No prompt inside a FUI screen | build `fui::optionDialog` into the screen | `WifiSelectionActivity::buildPromptDialog` |

All of these paths route input through the same SDK interaction table, so touch and physical buttons fire the same actions with no per-screen coordinate code.

## The hosting stack

`UiAppHost` ([src/components/UiAppHost.h](../../src/components/UiAppHost.h)) owns what every FUI screen shares: the font-bound render target, the `FreeInkApp`, and the `uiReady` handshake that lets the loop task route touch snapshots against the interaction table the render task rebuilds. Never re-implement that handshake; it is a cross-task protocol and lives in exactly one place.

`UiListActivity` ([src/activities/UiListActivity.h](../../src/activities/UiListActivity.h)) layers the list protocol on top: swipes scroll the viewport without moving the selection, buttons move the selection and pull the viewport along (`fui::ListNav`), plus the render skeleton (header chrome, app, footer hints). `UiTabListActivity` is the tab-bar sibling; its selection ring puts the tab bar at position 0 and the rows after it.

### New list screen skeleton

Subclasses supply the data; the base owns the loop. The whole contract:

```cpp
class MyListActivity final : public UiListActivity {
 public:
  MyListActivity(GfxRenderer& r, MappedInputManager& in) : UiListActivity("MyList", r, in) {}

 private:
  std::vector<Entry> entries;       // the activity's data, loaded in onEnter
  std::vector<fui::ListItem> rows;  // activity-owned row cache: rebuilt only when entries changes, reused every render

  int listCount() const override { return entries.size(); }
  const char* headerTitle() const override { return tr(STR_MY_TITLE); }

  void onEnter() override {
    UiListActivity::onEnter();
    entries = /* ... load from wherever ... */;
    rebuildRows();
  }

  // Called whenever entries changes (here, only onEnter; a mutable list would
  // also call this after any add/remove). NOT called from buildScreen().
  void rebuildRows() {
    rows.clear();
    rows.reserve(entries.size());
    // ... one fui::ListItem per entry (label, actionValue = index) ...
  }

  void buildScreen(UiScreen& screen) override {
    // set content margin from the theme safe area, then:
    fui::ListProps props;
    props.items = rows.data();
    props.count = rows.size();
    props.action = ACTION_ROW;
    props.inputMask = fui::InputTouch;  // physical buttons stay in the base loop
    syncListViewport(screen, props);    // selection/viewport handoff, always right before list()
    screen.list(props);
  }

  void activateIndex(int index) override {
    app.clearTapFlash();  // leaving the screen: a lingering flash would gray the next render
    // ... open the thing ...
  }
};
```

For small bounded lists, [`LanguageSelectActivity`](../../src/activities/settings/LanguageSelectActivity.cpp)
builds a fixed row array in `onEnter()` and reuses it in `buildScreen()`. For
large lists, follow [`FileBrowserActivity::provideRow()`](../../src/activities/home/FileBrowserActivity.cpp)
and `fui::ListProps::rowProvider`: format rows on demand into reusable scratch
storage instead of materializing the whole row list. Optional overrides include
`onRowLongPress(index)`, `drawFooter()`, and `handleButtons()` for extra physical
input. Use `ACTION_USER`-and-up action ids for non-row elements and register
handlers in `onEnter()` after the base's.

### Rules that apply to every FUI screen

- Register actions with `app.on(...)` in `onEnter` (after `resetUi()` for direct `UiAppHost` users). Handlers receive a `void* user` you cast back to the activity.
- Handlers that leave the current screen call `app.clearTapFlash()` first.
- Theme tokens are shared and bound by `resetUi()`; never call `app.setTheme` yourself. Metrics flow from the active UITheme through [`UIThemeTokens.h`](../../src/components/UIThemeTokens.h), including the per-board bezel insets that keep scrollbars visible.
- `TextStyle.maxLines` defaults to 1 and truncates with an ellipsis. Set `maxLines` explicitly on any dialog headline or message that can wrap.
- Keep steady-state repaint allocation-free. A local `std::vector` in `buildScreen()` allocates and frees on every repaint even with `reserve()`. For bounded lists, reuse activity-owned row storage, reserved or fixed-size when data loads. For large lists, use `rowProvider` with reusable scratch storage as above. Keep the backing storage alive for rendering; do not retain FUI `props` across renders.

### Component inventory

All under `freeink-sdk/libs/ui/FreeInkUI/include/components/`:

| Category | Components |
|---|---|
| Controls | `button`, `checkbox`, `slider`, `progress-bar`, `header` |
| Lists | `list` (virtualized), `table`, `dropdown`, `radio-group`, `setting-row`, `toggle-row`, `stepper-row` |
| Keyboard | `keyboard` (Latin, Cyrillic and Hebrew layouts), `key-grid` |
| Overlays | `popup`, `option-dialog`, `context-menu`, `message-panel`, `toast` |
| Bars | `status-bar`, `tab-bar`, `reader-chrome`, `battery-indicator`, `gesture-bar`, `tap-zones` |
| Media | `book-card`, `cover-grid`, `cover-carousel`, `metric-card` |
| Text | `text-field`, `text-area` |

One exception to "always go through a host": `KeyboardEntryActivity` drives the keyboard component with a raw `fui::Frame` and `TouchHoldRouter` because per-key hold repeat needs its own routing. If you think your screen needs that, raise it in the PR first.

---

## Global gestures: do not reimplement these

Three gestures are handled once, for every screen. Activities must not add their own edge-swipe handling:

| Gesture | Trigger | Where it is handled |
|---|---|---|
| Back | Right-swipe starting in the left 25% of the screen | Folded into `Button::Back`, so the existing `wasPressed(Button::Back)` in your activity already fires |
| Home | Up-swipe starting in the bottom 14% | `ActivityManager::loop()`; pops to Home (activities can override via `handleHomeGesture()`) |
| Menu | Down-swipe starting in the top 14% | Activities that have a menu check `wasMenuGesture()` themselves (the reader does this) |

Because the back gesture arrives as `Button::Back`, most button-era activities gain back-swipe support with zero changes.

---

## Legacy bridge helpers (do not use in new code)

`MappedInputManager` still exposes raw touch accessors. The FUI stack consumes them internally (via `touchSnapshotFrom` in [`UiAppHelpers.h`](../../src/components/UiAppHelpers.h)); activities should not.

| Helper | Status |
|---|---|
| `wasScreenTapped` / `wasScreenTouchDown` / `isScreenTouchHeld` | Consumed by the FUI snapshot builder. Direct use only in the two legacy surfaces |
| `wasTapInRect(x, y, w, h)` | Legacy one-off hit test |
| `rowTouch` / `colTouch` | Legacy row/column band math. Sole remaining user: `HomeActivity` (theme-driven layout) |
| `wasSwipe()` | Raw swipe direction, for behavior beyond the global gestures (reader page turns) |
| `hasTouch()` | Still fine anywhere: gate touch-only chrome (on-screen Cancel/OK pairs) on it |

The `wasListItemTapped` / `wasListItemTouchedDown` helpers from the button-era bridge have been removed; every themed list is a `UiListActivity` now. If you are porting a branch that still calls them, convert the screen to `UiListActivity` rather than resurrecting the helpers.

As with all input: never call the SDK `InputManager` or read GPIO directly. The HAL rule from the main guide applies to touch too.

---

## Building and testing on other devices

Select a checked-in environment from `platformio.ini` before creating a local
profile. CrossPoint's CI and release workflows currently cover:

| Device | Development environment | MCU |
| --- | --- | --- |
| Xteink X3 / X4 | `default` | ESP32-C3 |
| Seeed reTerminal Sticky | `sticky` | ESP32-S3 |
| Xteink X4 Pro | `x4pro` | ESP32-S3 |
| Xteink X4 Classic | `x4c` | ESP32-S3 |
| M5Stack Paper Mono | `papermono` | ESP32-S3 |
| Metalio E-Ink 4 | `metalio_eink4` | ESP32-S3 |

Build with `pio run -e <environment>`. X3 and X4 share a binary with runtime
board selection; different MCU families require separate binaries. Check the
current workflow matrices before claiming build coverage or published assets.

The [FreeInk SDK profiles](../../freeink-sdk/libs/hardware/BoardConfig/include/BoardConfig.h)
cover additional hardware. An SDK profile does not by itself establish
CrossPoint release support or hardware verification. The M5Paper v1.1 and
LilyGo T5 S3 examples below are local profiles outside the current CI matrix.

Put those profiles in **`platformio.local.ini`**, a gitignored file merged over
`platformio.ini` (see `extra_configs`). Personal ports and debug flags belong
there too. If overriding `build_flags`, preserve the selected environment's
device and capability flags as well as `${base.build_flags}`; a local value
replaces its whole flag list. Follow [getting started](./getting-started.md)
for the pinned pioarduino setup.

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
- **Device flags select board profiles.** The local examples use one `FREEINK_DEVICE_*` flag; the C3 X3/X4 profile deliberately enables both. See `freeink-sdk/platformio.sample.ini` for SDK reference environments and `BoardConfig` for capability defaults.
- **Serial logs:** `[base]` does not enable logging; without `-DENABLE_SERIAL_LOG` a non-default env prints nothing.
- No touch hardware on your desk? The X4 build still exercises the same code paths through buttons; touch-specific behavior (tap zones, gestures) needs a real device.
