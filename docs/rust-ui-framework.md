# Rust UI Framework

Screens can be written in Rust against a declarative, SwiftUI-shaped API that
renders through the existing C++ `GfxRenderer` and `UITheme`. Rust and C++
screens coexist — nothing about the C++ UI changed to accommodate this — and the
Rust code compiles into the firmware for every device, not just the simulator.

- [Crates](#crates)
- [Adding a screen](#adding-a-screen)
- [API reference](#api-reference)
- [Rules](#rules)
- [Building and running](#building-and-running)
- [Testing](#testing)
- [Extending the FFI](#extending-the-ffi)

## Crates

| Crate | Contains | Output |
|---|---|---|
| `lib/cpui_rs` | The framework: views, layout, widgets, screen roots, and the `Host` traits it needs from a firmware. **No product-specific code** — CI enforces this. | `rlib` |
| `lib/backend_rs` | The C++ bridge: the `Host` implementation, every `extern "C"` declaration, `tr!`, and the activity lifecycle. Effectively all the `unsafe`. | `rlib` |
| `lib/crosspoint_rs` | CrossPoint's screens, mirroring `src/activities/`. | `staticlib` + `rlib` |

The dependency points **inward** — `crosspoint_rs` → `backend` → `cpui` — so the
framework never names a firmware symbol and the compiler enforces the boundary.
`crosspoint_rs` statically contains the other two, so the firmware links one
archive. All three are members of the Cargo workspace at the repository root.

Each has its own README: [`cpui`](../lib/cpui_rs/README.md) ·
[`backend`](../lib/backend_rs/README.md) ·
[`crosspoint_rs`](../lib/crosspoint_rs/README.md).

```
lib/cpui_rs/src/                      the framework
├── host/          the five traits it needs, and the façades widgets call
├── view/          the View trait and the interaction model
├── layout/        stacks, spacer, padding, scroll view, chainable modifiers
├── widgets/       text, list, slider, stepper, icons, modal, toggles
├── screen/        the Screen contract, its runtime, and the root views
├── geometry.rs    Point, Size, Rect, Insets
└── testing.rs     a fake host, so tests need neither device nor simulator

lib/backend_rs/src/                   the C++ bridge
├── raw.rs         every extern "C" declaration
├── firmware.rs    the type implementing cpui's Host
├── renderer.rs · font.rs · theme.rs · input.rs    the trait impls
├── device.rs · frontlight.rs · icon.rs · i18n.rs  firmware capabilities
├── lifecycle.rs   the rust_activity_* entry points, and register_screen!
├── runtime.rs     global allocator and panic handler
└── testing.rs     host doubles for the C symbols, so test binaries link

lib/crosspoint_rs/src/                the screens
├── lib.rs         anchors backend, so the linker keeps its entry points
└── activities/    mirrors src/activities/ on the C++ side
```

---

## Adding a screen

The step-by-step walkthrough now lives in
**[Your first Rust screen](your-first-rust-screen.md)** — it goes from an empty file to a
screen running on real hardware, including the simulator and flashing, which
this document never covered.

Each crate's README explains its own layer, and they read in order:

| | |
|---|---|
| [`cpui`](../lib/cpui_rs/README.md) | the UI: views, layout, widgets |
| [`backend`](../lib/backend_rs/README.md) | the bridge to C++ |
| [`crosspoint_rs`](../lib/crosspoint_rs/README.md) | where screens live |

The rest of this document is the reference behind them: the full API, the rules
that keep the firmware within its memory budget, and how to extend the FFI.

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
| `ScrollView::new(child)` | Shows a window onto content taller than itself. The runtime scrolls to keep focus visible; content is clipped, so overflow never reaches the chrome. One per screen. |

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
| `Section::new(title, content)` | A titled group of anything, ruled beneath the title. |
| `List` / `ListRow` | Themed, selectable list. `ListRow::new(t).subtitle(s).value(v)`. |
| `ListRow::toggle(t, on, on_label, off_label)` | A boolean setting. See below. |
| `ProgressBar::new(cur, total)` | Or `ProgressBar::percent(72)`. |
| `Slider::new(value, max)` | A bare track. `.on_change(Msg::V)` to report drags. |
| `Stepper::new(value)` | `-` / slider / `+` as one control. `.on_change`, `.on_step`. |
| `Toggle::new(label, on, on_label, off_label)` | A boolean row. `.on_change(Msg::V)` gets the **next** state. |
| `Modal::picker(title, options)` | A centred option dialog. `.selected(i)`, `.on_select(Msg::V)`, `.scrim(Scrim::Dim)`. Captures input while present. Also `Modal::confirm`. |
| `Image::new(data, w, h)` | A 1-bpp bitmap you supply. |
| `Icon::new(IconRole::Sun)` | A host asset by **role**. `.filled(bool)`, `.size(px)`. |
| `IconToggle::new(role, on)` | An icon that shows and flips a boolean. `.on_change(Msg::V)` gets the **next** state. |

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
`update`. Neither hit-tests: they declare their regions and the runtime converts
a touch into a value or an index, so no screen ever sees geometry.

```rust
Slider::new(self.level, 100).on_change(Msg::Level)   // drag or tap -> a value
Modal::picker(title, LABELS)
    .selected(self.current)
    .on_select(Msg::Chose)                           // touch or Confirm -> an index
```

A dialog **captures input**: while one is in the tree nothing behind it can be
reached, focus opens on the value already chosen, and the side buttons walk its
options rather than the list underneath. Dismissing returns focus to the row
that opened it. A screen decides only whether the dialog is in `body()`.

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
        IconToggle::new(IconRole::Sun, self.on).on_change(Msg::Light),
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

**A vertical swipe moves focus**, which is what the C++ home screen does, so a
touch panel and a button one navigate the same list the same way. A screen that
wants the gesture for itself claims it with `fn on_swipe(&self, dir)`. Which way
it walks is the host's preference (`InputSource::swipe_moves_selection`): by
default the swipe drags the *content*, so swiping up moves focus down.

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

`.overlay(view)` / `.overlay_if(cond, view)` puts a view over the content —
a dialog, typically. It is measured against the whole screen rather than the
content band, drawn last, and sits outside any `ScrollView`, so it is neither
clipped nor scrolled away.

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
2. **`src/rust_ffi/*.cpp`** — the implementation, one file per concern.
3. **`lib/cpui_rs/src/testing.rs`** — a double, or the test binary will
   not link.

Then wrap the raw call in the matching `ffi/` module. Nothing outside `ffi`
should touch `raw`.

The globals `g_rustRendererPtr`, `g_rustInputPtr` and `g_rustActivityPtr` are
bound for the whole activity lifetime in `ActivityRs::onEnter()` — not
per-callback, because `render()` runs on the render task while `loop()` runs on
the main task. Binding them per-callback leaves them null for the other one.
