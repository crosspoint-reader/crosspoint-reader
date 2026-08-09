# Rust UI Framework

Screens can be written in Rust against a declarative, SwiftUI-shaped API that
renders through the existing C++ `GfxRenderer` and `UITheme`. Rust and C++
screens coexist — nothing about the C++ UI changed to accommodate this — and the
Rust code compiles into the firmware for every device, not just the simulator.

- [Crates](#crates)
- [Adding a screen, end to end](#adding-a-screen-end-to-end)
- [API reference](#api-reference)
- [Rules](#rules)
- [Building and running](#building-and-running)
- [Testing](#testing)
- [Extending the FFI](#extending-the-ffi)

## Crates

| Crate | Contains | Output |
|---|---|---|
| `lib/cpui` | Generic framework: views, layout, widgets, FFI. **No product-specific code** — CI enforces this. | `rlib` |
| `lib/crosspoint_rs` | CrossPoint's screens, mirroring `src/activities/`. | `staticlib` + `rlib` |

`crosspoint_rs` statically contains `cpui`, so one archive is linked.
Both are members of the Cargo workspace at the repository root.

```
lib/cpui_rs/src/        lib/crosspoint_rs/src/
├── lib.rs      re-exports      ├── lib.rs        tr! macro
├── runtime.rs  heap, panic     ├── strings.rs    GENERATED keys
├── geometry.rs Point/Size/Rect └── activities/   mirrors src/activities/
├── view.rs     the View trait      └── settings/about.rs
├── activity.rs lifecycle
├── testing.rs  test doubles
├── ffi/        raw + renderer, font, input, theme, device, i18n, activity
├── layout/     stack, spacer, padding, macros
├── widgets/    text, divider, list, progress
└── screen/     navigation
```

---

## Adding a screen, end to end

We'll add a fictional **Storage** screen under Settings. Seven steps; the
existing About screen is a working copy of every one of them.

### 1. Add the translation keys

Edit `lib/I18n/translations/english.yaml` only:

```yaml
STR_STORAGE: "Storage"
STR_CARD_CAPACITY: "Card Capacity"
```

Other languages need no edit — a missing key falls back to English
automatically. See [i18n.md](./i18n.md).

Regenerate (the build does this too, but doing it now gives you the Rust
constants immediately):

```bash
python3 scripts/gen_i18n.py lib/I18n/translations lib/I18n/
```

This writes `lib/crosspoint_rs/src/strings.rs` with a constant per key. Never
edit that file.

### 2. Write the screen in Rust

`lib/crosspoint_rs/src/activities/settings/storage.rs`:

```rust
use alloc::format;

use cpui::{register_screen, vstack, Font, List, ListRow, NavigationScreen, Screen, Text, View};

use crate::tr;

/// Everything this screen can be told. One enum per screen, matched
/// exhaustively in `update` — so a control that sends something the screen
/// forgot to handle is a compile error, not a silent no-op.
#[derive(Clone, Copy)]
pub enum Msg {
    Refresh,
}

#[derive(Default)]
pub struct StorageScreen {
    capacity_gb: u32,
}

impl StorageScreen {
    pub fn new() -> Self {
        StorageScreen::default()
    }
}

impl Screen for StorageScreen {
    type Message = Msg;

    fn body(&self) -> impl View<Msg> {
        NavigationScreen::new(vstack![20;
            Text::new(tr!(STR_CARD_CAPACITY)).font(Font::ui_small()),
            Text::new(format!("{} GB", self.capacity_gb)).bold(),
            List::new().push(ListRow::new(tr!(STR_REFRESH)).on_tap(Msg::Refresh)),
        ])
    }

    fn update(&mut self, message: Msg) {
        match message {
            Msg::Refresh => self.capacity_gb = read_capacity(),
        }
    }
}

register_screen!(StorageScreen, create_storage_activity);
```

That is the whole screen. There is no hit-testing, no selected index, no button
polling, and no `request_update()` — the runtime does all of it.

**`body` is a pure function of the screen's state**, called once per paint and
once per frame carrying input. Built fresh rather than stored, because `loop`
and `render` run on different FreeRTOS tasks with no lock between them: a stored
tree is one task walking what the other is replacing. Two screens froze that way
before this rule existed.

**`update` is the only place state changes.** The runtime repaints afterwards,
so a screen can never forget to.

Override only what you need:

```rust
impl Screen for StorageScreen {
    // ...

    /// A key the runtime has not claimed, offered *before* it applies its own
    /// meaning — so a screen wanting Up/Down for something other than moving
    /// focus simply says so. Auto-repeat applies to whatever you claim.
    fn on_key(&self, key: Button) -> Option<Msg> {
        match key {
            Button::Left => Some(Msg::Previous),
            _ => None,
        }
    }

    fn is_overlay(&self) -> bool { true }   // paint over what is already there
    fn on_enter(&mut self) { /* acquire */ }
    fn on_exit(&mut self) { /* release */ }
}
```

`register_screen!` exports the C factory. Use it once per screen; the shared
lifecycle entry points live in the framework.

### 3. Declare the module

`lib/crosspoint_rs/src/activities/settings/mod.rs`:

```rust
pub mod about;
pub mod storage;

pub use about::AboutActivity;
pub use storage::StorageActivity;
```

### 4. Add the C++ wrapper

`src/activities/settings/StorageActivityRs.h`:

```cpp
#pragma once

#include "activities/ActivityRs.h"

extern "C" {
void* create_storage_activity();   // must match register_screen!
}

class StorageActivityRs final : public ActivityRs {
 public:
  explicit StorageActivityRs(GfxRenderer& renderer, MappedInputManager& mappedInput);

  const char* getTitle() const override;
};
```

`src/activities/settings/StorageActivityRs.cpp`:

```cpp
#include "StorageActivityRs.h"

#include <I18n.h>

#include "I18nKeys.h"

StorageActivityRs::StorageActivityRs(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : ActivityRs("StorageActivityRs", renderer, mappedInput, create_storage_activity) {}

// Read fresh on every render, so a language change is picked up immediately.
const char* StorageActivityRs::getTitle() const { return tr(STR_STORAGE); }
```

The title lives on the C++ side so it goes through `tr()` without the Rust
screen having to know about it. `NavigationScreen` draws it in the header band.

### 5. Wire it into the menu

In `src/activities/settings/SettingsActivity.h`, add to the `SettingAction`
enum:

```cpp
Storage,
```

In `src/activities/settings/SettingsActivity.cpp`, include the header and
handle the action the same way `About` is handled:

```cpp
#include "StorageActivityRs.h"
...
case SettingAction::Storage:
  startActivityForResult(std::make_unique<StorageActivityRs>(renderer, mappedInput),
                         resultHandler);
  break;
```

Then add the row to the settings list (see how `STR_ABOUT` is registered in
`src/SettingsList.h`).

### 6. Add a test

`lib/crosspoint_rs/tests/storage_screen.rs`:

```rust
use crosspoint_rs::activities::settings::StorageScreen;
use cpui::testing::{self, CONTENT_BOTTOM, CONTENT_TOP, SCREEN_HEIGHT, SCREEN_WIDTH};
use cpui::{Interactions, Point, Screen, Size, View};

#[test]
fn storage_draws_inside_the_content_band() {
    let screen = StorageScreen::new();
    let mut root = screen.body();

    testing::reset();
    root.measure(Size::new(SCREEN_WIDTH, SCREEN_HEIGHT));
    root.render(Point::ORIGIN);

    for (_, y, text, font_id, _) in testing::drawn_text() {
        assert!(y >= CONTENT_TOP, "{text:?} overlaps the header");
        assert!(y + testing::line_height(font_id) <= CONTENT_BOTTOM,
                "{text:?} runs under the button hints");
    }
}

/// Controls can be tested without touching the runtime: collect the
/// interactions and ask what each would send.
#[test]
fn the_refresh_row_sends_refresh() {
    let screen = StorageScreen::new();
    let mut root = screen.body();
    root.measure(Size::new(SCREEN_WIDTH, SCREEN_HEIGHT));

    let mut found = Interactions::new(0);
    root.interactions(Point::ORIGIN, &mut found);

    let item = &found.items()[0];
    assert!(matches!(item.trigger.resolve(item.rect, 0), Msg::Refresh));
}
```

No firmware and no simulator needed — the framework's test doubles stand in for
every `cpp_*` symbol.

### 7. Build and check

```bash
./build-and-test.sh check     # gates: fmt, clippy, tests, cpui_rs grep, C++ format
./build-and-test.sh           # then build and run the simulator
./build-and-test.sh all       # and before you push: every gate, every CI target
```

---

## API reference

### Layout

Containers take views **by value** — you never write `Box::new` to build a tree.

```rust
VStack::new(20)                    // vertical, 20px between children
    .push(Text::new("Title"))
    .push_if(show_detail, Text::new(detail))   // conditional
    .push_some(optional_view)                  // Option<impl View>
    .extend(rows)                              // an iterator of views
```

Or the macro form, which expands to exactly those calls:

```rust
vstack![20;
    Text::new("Title").bold(),
    Spacer::new(),
    Text::new("Footer"),
]

hstack![8; Text::new("Battery"), Spacer::new(), Text::new("72%")]
```

| Type | Purpose |
|---|---|
| `VStack` / `HStack` | Stack children along one axis. Two-pass: fixed children measure first, flexible ones split what is left. |
| `Spacer` | Absorbs leftover space, pushing what follows to the far end. |
| `Padding::all(child, 12)` | Insets a child. Also `symmetric(child, h, v)` and `new(child, Insets)`. |

Stacks align children at the leading cross edge by default. A row mixing a 32px
icon with a line of text wants `.align(Alignment::Center)`, or the text hangs off
the top.

Any view can be modified in place, chainably:

| Modifier | Effect |
|---|---|
| `.on_tap(ACTION)` | Reports a touch as `ACTION`. See [Touch](#touch). |
| `.on_tap_value(ACTION, v)` | Same, carrying `v` in the hit — how `-`/`+` share one id. |
| `.flexible()` | Absorbs the space fixed siblings leave, like a `Spacer` does. |
| `.frame(w, h)` | Fixes the size and centres the view in it. Either axis may be `0` for natural. |

To collect views of different types, box them — `Box<T>` is itself a `View`:

```rust
let rows: Vec<Box<dyn View>> = vec![
    Box::new(Text::new("a")),
    Box::new(Divider::new()),
];
VStack::new(4).extend(rows)
```

### Widgets

| Widget | Notes |
|---|---|
| `Text::new(s)` | `.font(f)`, `.bold()`, `.italic()`. Measured with real font metrics. |
| `Divider::new()` | A one-pixel rule across the available width. |
| `List` / `ListRow` | Themed, selectable list. `ListRow::new(t).subtitle(s).value(v)`. |
| `ListRow::toggle(t, on, on_label, off_label)` | A boolean setting. See below. |
| `ProgressBar::new(cur, total)` | Or `ProgressBar::percent(72)`. |
| `Slider::new(value, max)` | A bare track. `.on_change(Msg::V)` to report drags. |
| `Stepper::new(value)` | `-` / slider / `+` as one control. `.on_change`, `.on_step`. |
| `Toggle::new(label, on, on_label, off_label)` | A boolean row. `.on_change(Msg::V)` gets the **next** state. |
| `Modal::new(title, options)` | A centred option dialog. |
| `Image::new(data, w, h)` | A 1-bpp bitmap you supply. |
| `Icon::new(IconRole::Sun)` | A host asset by **role**. `.filled(bool)`, `.size(px)`. |

**A toggle is a row, not a switch.** No C++ screen in this firmware draws a
switch graphic, so `Toggle` renders through the theme's list and looks identical
standing alone or inside one. It hands `update` the state it is *moving to*, so
a screen never writes `!self.something`:

```rust
Toggle::new(tr!(STR_HYPHENATION), self.on, tr!(STR_STATE_ON), tr!(STR_STATE_OFF))
    .on_change(Msg::Hyphenation)   // Msg::Hyphenation(true) when currently off
```

**Interactive widgets are stateless.** `Slider` and `Modal` draw the value they
are given and never change it; the screen owns the state and adjusts it in
`loop_`, exactly as `IntervalSelectionActivity` does in C++. Helpers turn input
into values without duplicating geometry:

```rust
// Drag: same rounding the C++ slider screens use. The rect comes from the hit.
let value = Slider::value_at(hit.rect, touch.x, max);

// Tap: hit rects come from C++, which already owns the dialog math.
if let Some(index) = modal.option_at(touch) { ... }
```

### Interaction

A screen never computes a rect, never hit-tests, and never polls a button. It
tags controls with **its own messages**, and the runtime delivers them:

```rust
#[derive(Clone, Copy)]
enum Msg {
    Brightness(i32),      // a new absolute value
    Step(i32),            // a relative nudge
    ToggleLight,
}

fn body(&self) -> impl View<Msg> {
    vstack![gap;
        Icon::new(IconRole::Sun).filled(self.on).on_tap(Msg::ToggleLight),
        Stepper::new(self.brightness)
            .on_change(Msg::Brightness)   // dragged or tapped on the track
            .on_step(Msg::Step),          // -1 / +1 from the end glyphs
    ]
}

fn update(&mut self, message: Msg) {
    match message {
        Msg::Brightness(v) => self.set_brightness(v),
        Msg::Step(d)       => self.set_brightness(self.brightness + d),
        Msg::ToggleLight   => self.toggle(),
    }
}
```

`Msg::Brightness` in `.on_change(Msg::Brightness)` is the variant *constructor*,
`fn(i32) -> Msg`. The framework converts the touch position into a value and
calls it, so slider geometry never reaches a screen.

**Messages, not closures.** A closure mutating screen state from inside a tree
the screen also owns needs interior mutability, and a `RefCell` borrow failure
panics — which aborts on device. A message is a plain value; nothing borrows.

| Modifier | Effect |
|---|---|
| `.on_tap(msg)` | Touch, and Confirm when focused |
| `.on_touch(msg)` | Touch only — stays out of the focus order |
| `.on_long_press(msg)` | Adds the held-press threshold |
| `.flexible()` | Absorbs leftover space, like a `Spacer` |
| `.frame(w, h)` | Fixes the size and centres the view in it |
| `.map(Msg::Variant)` | Folds a sub-component's messages into this screen's |

**Focus is the framework's.** Up/Down move it through the interactive controls
in tree order, wrapping at both ends, and never reach `update`. Confirm fires
the focused control's message — **the identical message a tap produces**, so
touch and buttons can never drift apart. A `List` highlights whichever row holds
focus with no screen code at all.

**Left/Right nudge whatever holds focus.** A control that opts into adjustment
(`Stepper` does) receives `-1` / `+1` there, so one pair of keys drives every
adjustable setting on a screen rather than the screen wiring keys to one of
them. Confirm on such a control does nothing — there is no absolute value to
commit.

**A composite is one focus stop.** A `Stepper` offers three touch targets (`-`,
the track, `+`) but a single stop for buttons, so Up/Down move between settings
rather than through glyphs. Use `.on_touch(msg)` instead of `.on_tap(msg)` for
anything that should take a finger without joining the focus order.

**Held frames only reach drag controls.** Each interaction declares an
`InputMask`; only `DRAG` (sliders) sees frames while the finger is down.
Everything else acts once, on release. Without this, a finger resting on a
button re-fires it every tick — a bug this framework shipped once.

**A control smaller than a fingertip is widened automatically** to the theme's
minimum touch target, centred on what was drawn. The message still reports the
drawn control, not the widened area.

**Auto-repeat is free.** A key claimed by `on_key` fires on press, then repeats
after 500ms at 500ms intervals — matching `ButtonNavigator` in C++.

### Reusable components

A component owns its state, declares its own message type, and the parent folds
it in with `.map()`:

```rust
struct UnitToggle { units: Units }

enum UnitMsg { Cycle }

impl UnitToggle {
    fn view(&self, bytes: i32) -> impl View<UnitMsg> { .. }
    fn update(&mut self, m: UnitMsg) { .. }
}

// in the screen
fn body(&self) -> impl View<Msg> {
    vstack![ self.units.view(heap.free).map(Msg::Units) ]
}

fn update(&mut self, m: Msg) {
    match m { Msg::Units(inner) => self.units.update(inner) }
}
```

A plain `fn thing(..) -> impl View<M>` is a first-class component too — that is
all `field()` in the About screen is.

### Screen roots

`NavigationScreen::new(content)` is the root for anything pushed onto the
activity stack. The theme draws the title band and button hints; content is laid
out between them.

```rust
NavigationScreen::new(content)
    .title("Custom")                       // else the activity's tr() title
    .hints(Hint::Standard, Hint::text("Save"), Hint::None, Hint::None)
```

`Hint::Standard` uses the firmware's own translated label for that slot;
`Hint::None` leaves it blank. Labels are given by meaning — the firmware
reorders them for the user's front-button layout.

`OverlayPanel::new(content)` is the root for a drop-down over whatever is
already on screen. It sizes itself to its content, paints only its own band, and
rules its bottom edge; the screen underneath survives as a scrim. Pair it with
`fn is_overlay(&self) -> bool { true }` so the runtime does not clear.

`.on_scrim_tap(Msg::Dismiss)` makes a touch below the panel dismiss it. That is
declared as an ordinary interaction, so the screen never compares a touch
against the panel's own height.

`.scrim(Scrim::Dim)` darkens that same strip, so the panel reads as the
foreground without its context being hidden. Both the dimming and the dismiss
target come from one rect, so what looks tappable is what is tappable.

Dimming adds ink on one checkerboard parity rather than filling, so roughly half
the pixels behind survive and the region reads as grey. `fill_rect_dither`
cannot do this — it clears the interior before applying its pattern, which
destroys the very content an overlay exists to preserve.

### Fonts

```rust
Font::ui()                                  // 12pt interface face
Font::ui_small()                            // 10pt
Font::reader()                              // the user's reading font
Font::new(FontFamily::NotoSans, 16)
Font::ui().bold()
```

Sizes are not interchangeable between families: Serif and Sans ship 12/14/16/18,
Ui ships 10/12, Small has one size. An unavailable combination resolves to
`Font::UNAVAILABLE`, which measures zero and draws nothing.

### Input, device, theme

```rust
Input::was_pressed(InputButton::Back)       // one frame, on press
Input::is_pressed(InputButton::Up)          // while held
Input::tap()                                // Option<Point>
Input::swipe()                              // SwipeDir
Input::was_back_gesture()

device::firmware_version()
device::name()
device::battery_percent()

Theme::content_area()                       // Rect between header and hints
Theme::metric(ThemeMetric::ListRowHeight)
request_update()                            // repaint after state changes
finish_activity()                           // pop this screen
```

All 15 logical buttons are available, named by meaning rather than position.

---

## Rules

**Never hardcode values the firmware owns.** Font ids are content hashes that
change with the assets; resolve family+size instead. The version is a
compile-time macro Rust cannot see; ask `device::firmware_version()`.

**Never estimate text size.** `Text` measures through the firmware's font
metrics. Guessing is what previously pushed content past the bottom of the
screen.

**Never hardcode `StrId` numbers.** They are positional and renumber whenever a
key is inserted. Use `tr!(STR_KEY)`.

**Ask the theme for geometry.** `Theme::content_area()` and `ThemeMetric`
replace pixel offsets, so a new panel size needs no screen changes.

**Watch the heap.** Widgets convert strings once at construction. Keep
`format!` off render paths — it pulls in `core::fmt`.

Memory is measured, not assumed. Every build prints a per-environment report and
fails if it exceeds `memory-budget.json`, in this shape:

```
[memory] default
  flash                   5,732,511 B   limit  6,100,000 B     +367,489 B  OK
  static RAM                 50,788 B   limit     65,536 B      +14,748 B  OK
    of which Rust            26,019 B   limit     98,304 B      +72,285 B  OK
```

Those figures are an illustration, not a current reading — they move with every
commit. The live numbers for a build are written to
`.pio/build/<env>/memory.json`; the `_measured_*` notes in `memory-budget.json`
record only what the figures were when a limit was last reviewed.

Note what that does **not** cover: static RAM is `.data/.bss/.noinit`, and Rust
allocates through the firmware heap, so it contributes ~0 bytes there no matter
how much it uses. Heap cost shows up in the `ActivityRs` log instead, which
reports free heap and largest block across a screen's lifetime — a screen that
does not return to its entry figure is leaking. `device::heap()` exposes the
same figures to Rust.

**`loop_` and `render` run on different FreeRTOS tasks.** Keep state in `self`
and let `render` only read it.

**`no_std` on device.** Use `alloc::` types explicitly; `std` is host-only.

---

## Building and running

PlatformIO compiles Rust through `scripts/build_rust.py`, which picks the target
from the environment's MCU. There is no separate cargo step.

| Environment | MCU | Triple | Toolchain |
|---|---|---|---|
| `simulator*` | host | host triple | stable, `std` |
| `default`, `gh_release*`, `slim` | ESP32-C3 | `riscv32imc-unknown-none-elf` | stable, `no_std` |
| `x4pro`, `sticky` | ESP32-S3 | `xtensa-esp32s3-none-elf` | `esp`, `no_std`, `build-std` |

One-time setup for the Xtensa targets (tier 3, no prebuilt `core`/`alloc`):

```bash
cargo install espup --locked
espup install                       # installs the `esp` toolchain + rust-src
rustup target add riscv32imc-unknown-none-elf
```

Then:

```bash
./build-and-test.sh               # gates, build, run simulator   (inner loop)
./build-and-test.sh check         # the gates only, no build      (seconds)
./build-and-test.sh all           # every gate + every CI target  (before you push)
./build-and-test.sh run           # run the existing simulator binary
./build-and-test.sh device        # build the x4pro firmware
pio run -e x4pro -t upload        # flash it
```

**Simulator controls:** arrows navigate, Enter confirms, Escape is Back, Q
quits. See [SIMULATOR.md](../SIMULATOR.md).

> Homebrew's `pio` may lack the `littlefs` module the espressif32 builder
> imports. If a device build dies with `ModuleNotFoundError: No module named
> 'littlefs'`, use `~/.platformio/penv/bin/pio` — `build-and-test.sh` prefers it
> automatically.

---

## Testing

```bash
cargo fmt --check
cargo clippy --workspace --all-targets --features cpui-rs/testing -- -D warnings
cargo test --workspace --features cpui-rs/testing
```

`cpui::testing` implements every `cpp_*` symbol with deterministic
stand-ins and records draw calls, so layout is verified on a desktop. It reports
a 480x800 portrait panel with a header band and button hints, exposed as
`SCREEN_WIDTH`, `CONTENT_TOP`, `CONTENT_BOTTOM`, `SIDE_PADDING` and friends.

```rust
testing::reset();
screen.render();
for (x, y, text, font_id, style) in testing::drawn_text() { ... }
```

What the doubles **cannot** prove: they echo any translation key straight back,
so a key missing from `english.yaml` looks identical to a working one. That half
is enforced by `gen_i18n.py`, which fails the build for a key used in source but
absent from the YAML.

CI runs the same gates plus a grep asserting the framework contains no
product-specific code.

---

## Extending the FFI

Adding a C++ capability means three edits, in this order:

1. **`lib/backend_rs/src/raw.rs`** — the `extern "C"` declaration.
2. **`src/activities/RustActivityStubs.cpp`** — the implementation.
3. **`lib/cpui_rs/src/testing.rs`** — a double, or the test binary will
   not link.

Then wrap the raw call in the matching `ffi/` module. Nothing outside `ffi`
should touch `raw`.

The globals `g_rustRendererPtr`, `g_rustInputPtr` and `g_rustActivityPtr` are
bound for the whole activity lifetime in `ActivityRs::onEnter()` — not
per-callback, because `render()` runs on the render task while `loop()` runs on
the main task. Binding them per-callback leaves them null for the other one.
