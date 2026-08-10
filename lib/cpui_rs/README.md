# CrossPoint UI Framework

A small declarative UI framework for CrossPoint firmware: you describe what the
screen should look like, and `cpui` works out the rest.

It was written for CrossPoint and the e-ink readers its firmware supports —
1-bit panels, around 380 KB of RAM, no GPU and no room for waste. Those
constraints shaped every decision in here.

What it deliberately holds *no* trace of is CrossPoint itself: no screen, no
asset name, no setting, no string. It reaches the firmware through a handful of
traits that [`backend`](../backend_rs/) implements. That separation is not a
promise it will drop into some unrelated project — it is what keeps product
detail out of layout code, and what lets all 41 tests run on a laptop instead of
a device.

## At a glance

- **No dependencies.** Not one. ~4,300 lines of Rust, and that is the whole of it.
- **`no_std`.** Runs on bare metal; runs on your laptop for tests.
- **Nothing to draw with.** `cpui` cannot paint a pixel by itself — the firmware
  supplies that, through five small traits.
- **41 tests**, none of which need hardware or a simulator.

## Getting started

A screen is a struct that says what it looks like and how it changes. That's it:

```rust
use cpui::{vstack, NavigationScreen, Screen, Stepper, Text, View};

struct Brightness {
    level: i32,
}

/// Everything this screen can be told.
#[derive(Clone, Copy)]
enum Msg {
    Set(i32),   // an absolute value, from dragging the track
    Step(i32),  // a nudge of -1 or +1, from the end glyphs
}

impl Screen for Brightness {
    type Message = Msg;

    fn body(&self) -> impl View<Msg> {
        NavigationScreen::new(vstack![12;
            Text::new(format!("Brightness  {}%", self.level)),
            Stepper::new(self.level)
                .on_change(Msg::Set)
                .on_step(Msg::Step),
        ])
    }

    fn update(&mut self, message: Msg) {
        match message {
            Msg::Set(level) => self.level = level.clamp(0, 100),
            Msg::Step(delta) => self.level = (self.level + delta).clamp(0, 100),
        }
    }
}
```

That is a working screen. It draws a header, a label and a stepper; it responds
to a finger on the track, to the `−` and `+` glyphs, and to the hardware buttons
— and you did not write a single coordinate, hit-test or redraw call.

It cannot reach a device yet, though: `cpui` has no idea one exists.
[`backend`](../backend_rs/) is what connects it, and
[**Your first Rust screen**](../../docs/your-first-rust-screen.md) walks the whole path
from here to a screen running on real hardware.

## How the pieces talk to each other

This is the part worth understanding, and it is simpler than it looks. There are
**three conversations**, and each one only goes one way.

```
        your screen                cpui                    the firmware
   ┌───────────────────┐   ┌──────────────────┐   ┌──────────────────────┐
   │                   │   │                  │   │                      │
   │  body()   ────────┼──▶│   view tree      ├──▶│  Canvas       paint  │
   │                   │   │   measure        │   │  TextMetrics  sizes  │
   │                   │   │   render         │   │  Chrome       theme  │
   │  update(msg) ◀────┼───┤   routing        │◀──┤  InputSource  touch  │
   │                   │   │                  │   │  Clock        time   │
   └───────────────────┘   └──────────────────┘   └──────────────────────┘
          messages              the View trait          the Host traits
```

**1. Your screen and `cpui` talk in messages.** `body()` hands over a description
of the screen. When something happens, `cpui` hands back a message and calls
`update()`. Your screen never reads a touch, never asks where anything is on
screen, and never asks for a repaint — it only ever receives a message and
changes its own state.

**2. `cpui` and the view tree talk through the `View` trait.** Every widget
answers four questions: how big are you, where do you sit, what do you draw, and
what can be touched. Widgets *declare* their touchable regions; nothing polls
for input.

**3. `cpui` and the firmware talk through the `Host` traits.** `cpui` cannot draw,
measure text or read a button. It says what it needs and the firmware provides
it. This is why the crate has no dependencies, and why nothing inside it names
a product, a screen or an asset.

The whole frame is: build the tree, measure it, collect what is touchable, find
the message for whatever the user did, apply it, paint. If that leaves you
wanting the detail, it is in [docs/architecture.md](docs/architecture.md).

## How it reaches the firmware

`cpui` needs somewhere to draw. The firmware implements five traits — paint,
measure text, draw themed furniture, read input, tell the time — and installs
that implementation once:

```rust
// Once, before anything is measured or drawn.
unsafe { cpui::host::install(&MY_FIRMWARE) };
```

The traits live in [`src/host/`](src/host/) and are deliberately small. Nothing
else in `cpui` knows the firmware exists, which is why a fake host makes the whole
framework testable. See [docs/host.md](docs/host.md) for what each trait must
do, and [`backend`](../backend_rs/) for the real one, which talks to C++.

## What you get

**Widgets** — [`src/widgets/`](src/widgets/)

| | |
|---|---|
| [`Text`](src/widgets/text.rs) | One line, measured with the firmware's real font metrics |
| [`Icon`](src/widgets/image.rs) | A firmware asset chosen by *role*, not filename |
| [`IconToggle`](src/widgets/icon_toggle.rs) | An icon that shows a boolean and flips it |
| [`Image`](src/widgets/image.rs) | A 1-bit bitmap you supply |
| [`List` / `ListRow`](src/widgets/list/) | Rows drawn by the firmware's own theme |
| [`Section`](src/widgets/section.rs) | A titled group of anything |
| [`Toggle`](src/widgets/toggle.rs) | A boolean row reading On / Off |
| [`Slider`](src/widgets/slider.rs) | A bare track |
| [`Stepper`](src/widgets/stepper.rs) | `−`, track and `+` as one control |
| [`ProgressBar`](src/widgets/progress.rs) | Determinate progress |
| [`Divider`](src/widgets/divider.rs) | A one-pixel rule |
| [`Modal`](src/widgets/modal.rs) | A centred option dialog |

**Layout** — [`src/layout/`](src/layout/)

`vstack!` and `hstack!` stack things with a gap. `Spacer` eats whatever space is
left, so a footer sits at the bottom without arithmetic. `Padding`, `Frame`,
`Flexible` and `Tappable` are chainable modifiers: `Text::new("−").frame(44, 44)`.

**Screen roots** — [`src/screen/`](src/screen/)

`NavigationScreen` is an ordinary page with a header and button hints.
`OverlayPanel` is a drop-down that leaves the screen beneath it intact, and can
dim it with `Scrim::Dim`.

## Worth knowing before you start

- **This is pre-1.0 and the API will move.** CrossPoint is its only consumer,
  and it changes to suit CrossPoint.
- **`body()` runs often** — on every repaint and while a finger is dragging. It
  allocates, so keep heavy work out of it. This is a real cost, not a hypothetical.
- **Text is a single line.** There is no wrapping widget yet.
- **Nothing scrolls.** Lists draw what fits; paging is the screen's business.
- **One thing is global**: the installed host. It is set once at startup and
  only read afterwards.

## Where next

- [docs/architecture.md](docs/architecture.md) — how a frame actually runs
- [docs/writing-a-widget.md](docs/writing-a-widget.md) — adding to the framework
- [docs/host.md](docs/host.md) — the contract the firmware implements
- [`backend`](../backend_rs/) — the C++ bridge · [`crosspoint_rs`](../crosspoint_rs/) — screens built on `cpui`
