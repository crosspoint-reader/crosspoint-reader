# crosspoint_rs

CrossPoint's screens, written in Rust.

This is the product layer. [`cpui`](../cpui_rs/) is the framework and knows
nothing about CrossPoint; [`backend`](../backend_rs/) is the bridge to the C++
firmware; everything CrossPoint-specific lives here.

It is also the crate the firmware actually links. The other two are rlibs;
this one builds a **staticlib**, and `scripts/build_rust.py` hands the resulting
archive to the linker. Whatever the other crates contribute arrives inside it.

## What a screen looks like

```rust
use backend::tr;
use cpui::{vstack, NavigationScreen, Screen, Text, View};

pub struct ExampleScreen {
    count: i32,
}

#[derive(Clone, Copy)]
pub enum Msg {
    Bumped,
}

impl Screen for ExampleScreen {
    type Message = Msg;

    fn body(&self) -> impl View<Msg> {
        NavigationScreen::new(vstack![12;
            Text::new(tr!(STR_SOME_KEY)),
        ])
    }

    fn update(&mut self, message: Msg) {
        match message {
            Msg::Bumped => self.count += 1,
        }
    }
}

backend::register_screen!(ExampleScreen, create_example_activity);
```

Two details that are specific to this crate:

- **`tr!` for anything the user reads.** Never a bare string. Keys come from
  `lib/I18n/translations/english.yaml`, and an unknown key is a compile error.
- **`register_screen!` once per screen.** It generates the factory function the
  C++ side calls to construct this screen, named in the second argument.

On the C++ side a small `ActivityRs` subclass names that same function and
supplies the screen's translated title. Four files in total for a new screen —
the walkthrough is in
[docs/rust-ui-framework.md](../../docs/rust-ui-framework.md).

## What a finished screen consists of

Four files, two languages. For a screen called `BrightnessDemo`:

```
lib/I18n/translations/english.yaml                     the words
lib/crosspoint_rs/src/activities/settings/
    brightness_demo.rs                                 the screen
    mod.rs                                             declares it
src/activities/settings/
    BrightnessDemoActivityRs.{h,cpp}                   the C++ wrapper
    SettingsActivity.{h,cpp}                           opens it from a menu
```

The checklist, in order:

- [ ] a `STR_*` key in `english.yaml` — and **only** there
- [ ] the screen: `impl Screen`, then `register_screen!`
- [ ] `pub mod` it in the parent `mod.rs`
- [ ] an `ActivityRs` subclass naming the generated factory function
- [ ] a menu entry that constructs it
- [ ] a test on `update()`, which needs no hardware

[**Your first Rust screen**](../../docs/your-first-rust-screen.md) walks all six with real
code, then runs it in the simulator and flashes it to a device.

## Where a screen goes

The module tree mirrors `src/activities/` on the C++ side, so the same screen
sits in the same place in both languages:

```
lib/crosspoint_rs/src/            src/activities/
└── activities/                   └── ...
    ├── settings/                     ├── settings/
    │   └── about_screen.rs           │   └── AboutActivityRs.cpp
    └── util/                         └── util/
```

A file is named after the type inside it: `about_screen.rs` holds `AboutScreen`.

## What belongs here, and what does not

| You are adding | It goes in |
|---|---|
| A screen, or something only CrossPoint needs | **here** |
| A widget, a layout container, anything about drawing | [`cpui`](../cpui_rs/) |
| Access to a firmware capability, or a new C++ call | [`backend`](../backend_rs/) |

The test is whether it has to know what CrossPoint is. A stack that centres its
children does not; a panel that drives the frontlight does.

## Where next

- [`cpui/README.md`](../cpui_rs/README.md) — the framework and its widgets
- [`backend/README.md`](../backend_rs/README.md) — the C++ bridge
- [**Your first Rust screen**](../../docs/your-first-rust-screen.md) — the tutorial, start to finish
- [docs/rust-ui-framework.md](../../docs/rust-ui-framework.md) — the full reference
