# backend

The bridge between Rust and the C++ firmware. Everything that crosses that line
crosses it here.

[`xpui`](../xpui_rs/) is a generic UI framework that cannot draw a pixel.
CrossPoint is a C++ firmware that can. This crate connects them, and it holds
very nearly all the `unsafe` in the three crates — every call across the C
boundary is here.

## Why it is separate

The dependency points **inward**:

```
   crosspoint_rs  ──▶  backend  ──▶  xpui
     screens            bridge       framework
```

`xpui` declares what it needs — paint this, measure that — and `backend`
implements it against the real firmware. `xpui` never names a CrossPoint symbol,
so the compiler enforces the boundary rather than a review comment. That is what
lets `xpui` have zero dependencies, and what keeps product detail from leaking
into layout code.

Practically: if you find yourself wanting to `use backend::` from inside `xpui`,
something is in the wrong crate.

## Following a value across the boundary

Take the brightness screen from [`xpui`'s README](../xpui_rs/README.md). A
finger lands on the track, and 60% becomes 72%. This is the whole journey:

```
  1. finger on the glass
  2. InputSource::touch_held()      backend  ->  the firmware's input manager
  3. the Stepper's declared track   xpui     ->  a position becomes a value
  4. Msg::Set(72)                   xpui     ->  your screen's update()
  5. Frontlight::set_brightness(72) backend  ->  a safe wrapper
  6. cpp_frontlight_set_brightness  the C boundary
  7. the light actually changes     firmware
```

Steps 2 and 6 are this crate. Everything between them is `xpui`, which never
learns that a frontlight exists — it only knows a control reported a number.

Reading goes the other way, and is simpler. The screen asks once when it opens:

```rust
BrightnessDemo { level: Frontlight::brightness() }
```

which is a safe wrapper over one `extern "C"` call. No `unsafe` reaches the
screen; that is this crate's job.

## Adding a C++ function needs three edits

This is the thing to remember. Miss the third and host tests stop linking, often
long after you made the change.

| Where | What |
|---|---|
| [`src/raw.rs`](src/raw.rs) | The `extern "C"` declaration |
| `src/rust_ffi/*.cpp` (in the firmware) | The C++ implementation |
| [`src/testing.rs`](src/testing.rs) | A double, so tests link without the firmware |

Then wrap it in something safe. Callers should never see `unsafe`:

```rust
pub fn brightness() -> i32 {
    unsafe { raw::cpp_frontlight_brightness() }
}
```

## What it provides

**The host implementation** — `Canvas`, `TextMetrics`, `Chrome`, `InputSource`
and `Clock` over the firmware's renderer, theme and input manager. Split by
concern across [`renderer.rs`](src/renderer.rs), [`font.rs`](src/font.rs),
[`theme.rs`](src/theme.rs), [`input.rs`](src/input.rs) and
[`firmware.rs`](src/firmware.rs).

**Firmware capabilities** — battery and device identity
([`device.rs`](src/device.rs)), frontlight and screen inversion
([`frontlight.rs`](src/frontlight.rs)), icons by role
([`icon.rs`](src/icon.rs)).

**Translations** — the `tr!` macro and the `STR_*` keys
([`i18n.rs`](src/i18n.rs)).

**The lifecycle** — [`lifecycle.rs`](src/lifecycle.rs) holds the
`rust_activity_*` entry points that C++ calls, and the `register_screen!` macro
that gives a screen a factory function for the C++ side to construct.

**The runtime** — [`runtime.rs`](src/runtime.rs) routes Rust allocation through
the firmware's own heap and turns a Rust panic into a firmware log rather than a
silent reset.

## Two traps

**The `STR_*` keys are generated, not written.** [`build.rs`](build.rs) reads
`lib/I18n/translations/english.yaml` and emits one constant per key into cargo's
`OUT_DIR`. Nothing lands in the source tree, so a fresh clone builds without
PlatformIO having run first. Add a key to the YAML, use it, build.

**A screen crate must anchor this one.** `backend` is an rlib, and an rlib that
nothing references is never linked in — taking the `#[panic_handler]` and the C
entry points with it. So the staticlib crate keeps:

```rust
extern crate backend as _;
```

Remove it and a device build fails with `` `#[panic_handler]` function required,
but not found``. The simulator does **not** catch this, because the host
supplies its own panic machinery.

## Where next

- [`xpui/README.md`](../xpui_rs/README.md) — the framework this serves
- [`xpui/docs/host.md`](../xpui_rs/docs/host.md) — the contract implemented here
- [**Your first Rust screen**](../../docs/your-first-rust-screen.md) — the end-to-end tutorial
- [`crosspoint_rs`](../crosspoint_rs/) — where the screens themselves live
- [`docs/rust-ui-framework.md`](../../docs/rust-ui-framework.md) — the full reference
