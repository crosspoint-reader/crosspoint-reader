# The host contract

`cpui` draws nothing by itself. It describes what it needs through five traits in
[`src/host/`](../src/host/); you implement them and install the result once.

[`backend`](../../backend_rs/) implements these against CrossPoint's C++
firmware. This page describes what each trait owes `cpui` — worth reading if you
are changing that implementation, or standing up a new CrossPoint-based one.

## The five traits

| Trait | You provide | File |
|---|---|---|
| `Canvas` | Fill and stroke rectangles, draw text, lines, bitmaps, icons | [canvas.rs](../src/host/canvas.rs) |
| `TextMetrics` | Width and line height for a string in a font | [metrics.rs](../src/host/metrics.rs) |
| `Chrome` | Header, button hints, list rows, dialogs — your theme | [chrome.rs](../src/host/chrome.rs) |
| `InputSource` | Buttons, taps, drags, gestures for one frame | [input.rs](../src/host/input.rs) |
| `Clock` | Milliseconds since boot | [clock.rs](../src/host/clock.rs) |

Implement all five on one type and it satisfies `Host` automatically:

```rust
pub struct MyFirmware;

impl Canvas for MyFirmware { /* ... */ }
impl TextMetrics for MyFirmware { /* ... */ }
impl Chrome for MyFirmware { /* ... */ }
impl InputSource for MyFirmware { /* ... */ }
impl Clock for MyFirmware { /* ... */ }

static FIRMWARE: MyFirmware = MyFirmware;

// `install` is unsafe: it must run before the first measure, render or
// interactions pass, and never alongside one.
unsafe { cpui::host::install(&FIRMWARE) };
```

A compile-time assertion is worth adding so a missing trait is caught at the
definition rather than at the install site:

```rust
const _: fn() = || {
    fn assert_host<T: cpui::host::Host>() {}
    assert_host::<MyFirmware>();
};
```

## Things that are easy to get wrong

**Install before anything runs.** That is the safety contract on `install`, not
a style note. Rendering and input run on different tasks on the device, so the
lifecycle installs from both entry points rather than assuming which wakes first.

**`Chrome` is your theme, not a default one.** `cpui` deliberately has no opinion
about what a list row looks like. It asks for one and you draw it however the
firmware's own C++ screens draw one, so Rust and native screens match.

**Ask for metrics honestly.** `TextMetrics` must reflect the font you will
actually paint with. If it does not, everything measures correctly and draws
wrongly.

**Icons are roles, not files.** `Canvas::draw_icon` receives an opaque number
meaning "the thing you use for *sun*", and you choose the asset. That keeps
asset names out of the framework.

**Report zero rather than guessing.** If a font or icon is missing from a build,
return `0` for its size. `cpui` then draws nothing, rather than painting garbage
at an arbitrary size.

## Testing without hardware

`cpui` ships a fake host behind the `testing` feature:

```toml
[dev-dependencies]
cpui = { path = "../cpui_rs", features = ["testing"] }
```

```rust
cpui::testing::install();
```

It reports fixed screen and theme dimensions and records everything drawn, so
you can assert on layout and touch behaviour in an ordinary `cargo test`. The
framework's own 41 tests use nothing else.

## A worked example

[`backend`](../../backend_rs/) implements all five over a C++ firmware, across
an FFI boundary. It is about 1,200 lines including the C declarations, and it
holds every unsafe call across that boundary — `cpui`'s own `unsafe` is confined
to the four lines that read the installed host.
