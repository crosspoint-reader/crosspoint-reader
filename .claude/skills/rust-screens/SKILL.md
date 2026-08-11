---
name: rust-screens
description: Building a screen or feature in Rust and wiring it to the C++ firmware. Use when adding or editing a Rust activity, working in lib/crosspoint_rs or lib/xpui_rs, adding a UI widget or layout container, exposing a new C++ capability to Rust over FFI, or touching scripts/build_rust.py, RustActivityStubs.cpp, or ActivityRs. Covers the exact files a new screen needs, the traps that produce silent breakage (dead input, stripped translations, device-only link failures), and the gates a Rust change must pass.
---

# Rust Screens

Full reference: `docs/rust-ui-framework.md`. This is the decision layer — what
to create, and the failures that do not announce themselves.

## A new screen is six files

Nothing else. Copy the About screen (`lib/crosspoint_rs/src/activities/settings/about.rs`
and `src/activities/settings/AboutActivityRs.*`); it is a working instance of
every step.

**1. Key(s) in `lib/I18n/translations/english.yaml`** — that file only. Other
languages fall back to English automatically; adding the key to all 30 files
destroys the "untranslated" signal. Regenerate with
`python3 scripts/gen_i18n.py lib/I18n/translations lib/I18n/` to get the Rust
constants.

**2. The screen** in `lib/crosspoint_rs/src/activities/<group>/<name>.rs`, where
`<group>` mirrors `src/activities/` on the C++ side:

```rust
use alloc::format;

use xpui::{register_screen, vstack, Font, NavigationScreen, Screen, Text, View};
use crate::tr;

/// One enum per screen. `update` matches it exhaustively, so a control that
/// sends something unhandled is a compile error.
#[derive(Clone, Copy)]
pub enum Msg { Refresh }

#[derive(Default)]
pub struct FooScreen { value: u32 }

impl FooScreen {
    pub fn new() -> Self { FooScreen::default() }
}

impl Screen for FooScreen {
    type Message = Msg;

    fn body(&self) -> impl View<Msg> {
        NavigationScreen::new(vstack![20;
            Text::new(tr!(STR_FOO_LABEL)).font(Font::ui_small()),
            Text::new(format!("{}", self.value)).bold(),
        ])
    }

    fn update(&mut self, message: Msg) {
        match message { Msg::Refresh => self.value += 1 }
    }
}

register_screen!(FooScreen, create_foo_activity);
```

`body` and `update` are the only required methods. The runtime owns
hit-testing, focus, input priority, auto-repeat and repainting — a screen never
calls `Input::`, never tracks a selected index, and never calls
`request_update()`.

**The screen holds state, never a view tree.** `body` is called fresh every
paint and every input frame. Storing the tree and mutating it from the input
path is a cross-task data race against `render` — it has frozen two screens.

**3. `pub mod` + `pub use`** in the group's `mod.rs`.

**4. The C++ wrapper**, `src/activities/<group>/FooActivityRs.{h,cpp}`: a
`final : public ActivityRs` whose constructor passes `create_foo_activity`, and
whose `getTitle()` returns `tr(STR_FOO)`. The title lives in C++ so it is
translated without the Rust screen knowing.

**5. Menu wiring**: a `SettingAction` enumerator, a `case` doing
`startActivityForResult(std::make_unique<FooActivityRs>(renderer, mappedInput), resultHandler)`,
and the row in `src/SettingsList.h`.

**6. A test** in `lib/crosspoint_rs/tests/` asserting every draw lands between
`CONTENT_TOP` and `CONTENT_BOTTOM`. Runs on the desktop against
`xpui::testing`; no simulator, no hardware.

## Traps

These all produce a screen that looks fine in code and fails in ways that do not
point at the cause.

- **Never hardcode a font id, a `StrId` number, or a pixel offset.** Font ids
  are content hashes regenerated with the assets; `StrId`s are positional and
  renumber when any key is inserted; offsets break on a new panel. Use
  `Font::new(family, size)`, `tr!(STR_KEY)`, and `Theme::metric` /
  `Theme::content_area()`.
- **Never estimate text size.** `Text` measures through the firmware's font
  metrics. An estimate drifts from what is painted and pushes content off the
  panel — the renderer then logs one error per pixel.
- **A font may not exist in this build.** `slim` sets `OMIT_FONTS`. An
  unavailable font resolves to `Font::UNAVAILABLE`, measures zero, draws
  nothing. Do not assume a family/size pair resolves.
- **`loop_` and `render` run on different FreeRTOS tasks.** Keep state in
  `self`; let `render` only read it. Anything reached from both is cross-task.
- **The FFI globals are bound for the activity lifetime**, in
  `ActivityRs::onEnter()`, not per callback. Binding them inside `render()`
  leaves them null during `loop()`, which silently kills all input.
- **A key used only from Rust must still be seen by the generator.** It scans
  `.rs` as well as C/C++ and the build strips unreferenced keys, so the key name
  must appear literally at the call site — which `tr!(STR_KEY)` guarantees and a
  renamed alias does not.
- **A toggle is not a switch.** No C++ screen draws one. Use `Toggle`, which
  renders through the theme's list and reports the state it is moving to.
- **Never compute a rect, and never poll input.** Tag controls with
  `.on_tap(Msg::X)` / `.on_change(Msg::X)` and match in `update`. Hand-computed
  rects are a second copy of the layout that drifts from what is drawn — which
  is how the first frontlight panel became 200 lines of arithmetic.
- **Claim keys with `on_key` sparingly.** Up/Down move focus and Left/Right
  nudge whatever holds it. `on_key` is consulted *first*, so claiming a key
  takes it away from navigation entirely — which is how the frontlight panel
  first shipped with dead Up/Down. Claim only keys with a screen-wide meaning.
- **A composite should be one focus stop.** Use `.on_touch(msg)` for the touch
  targets inside it and declare a single `FOCUS | ADJUST` interaction for the
  whole control, as `Stepper` does. Otherwise buttons walk through its parts.
- **Icons are roles, not assets.** `Icon::new(IconRole::Sun).filled(on)`. Adding
  one means a variant in `IconRole` and an arm in the C++ `resolveIcon` switch.
- **`Image` and `Icon` are not interchangeable.** Icon assets are stored
  pre-rotated; a bitmap fed to the wrong one renders rotated 90 degrees. And in
  the `Image` format **bit 0 is ink**, inverted from the usual convention.
- **Interactive widgets are stateless.** `Slider` and `Stepper` draw the value
  they are given; the screen owns it and changes it in `update`. The framework
  converts a touch position into a value, so geometry never reaches a screen.
- **A container's `interactions` must walk exactly as its `render` does.** They
  are separate code paths with nothing enforcing agreement; if they drift,
  touches land on the neighbouring control. `lib/xpui_rs/tests/interactions.rs`
  catches that — extend it when adding a container.
- **`register_screen!`, not `register_activity!`.** Once per screen; the shared
  lifecycle symbols live in the framework.

## Adding a C++ capability

Three edits, or the build breaks in a confusing place:

1. `lib/backend_rs/src/raw.rs` — the `extern "C"` declaration.
2. `src/activities/RustActivityStubs.cpp` — the implementation.
3. `lib/xpui_rs/src/testing.rs` — a double, or the **test binary will
   not link** even though the firmware does.

Wrap the raw call in the matching `ffi/` module. Nothing outside `ffi` touches
`raw`.

## Keep the framework generic

`lib/xpui_rs` must contain no product name, screen, or feature — CI
greps for it. A screen belongs in `lib/crosspoint_rs`; a reusable widget or
container belongs in the framework. If a "generic" addition only makes sense for
one screen, it is not generic.

Do not add framework API speculatively. Unused surface is what produced the
several hundred lines of dead code this crate was already carrying.

## Self-review before handing back

- [ ] `./build-and-test.sh check` clean — fmt, clippy, tests, the `xpui_rs`
      product-name grep, and C++ formatting.
- [ ] **Device environments build, not just the simulator.** Rust links into
      every firmware target, so a Rust error breaks real hardware builds, not
      just the host. `./build-and-test.sh all` builds them all in one `pio`
      invocation. (Homebrew's `pio` may lack `littlefs`; use
      `~/.platformio/penv/bin/pio`, as `build-and-test.sh` does.)
- [ ] No `Box::new` in the view tree — containers take views by value.
- [ ] Every user-visible string goes through `tr!`; none hardcoded.
- [ ] New FFI function has all three edits, including the test double.
- [ ] Nothing product-specific landed in `lib/xpui_rs`.
- [ ] `lib/crosspoint_rs/src/strings.rs` was not hand-edited — it is generated.
- [ ] Layout is asserted by a test, not by eyeballing the simulator. The
      doubles record text *and* rectangles, so widget geometry is testable.
- [ ] The memory report stayed within `memory-budget.json`. It prints per
      environment on every build; raising a limit needs a reason in the commit.
- [ ] Heap returns to its entry figure across the screen — check the
      `[ACTRS] <name> heap:` line. Static RAM cannot show a Rust leak.
