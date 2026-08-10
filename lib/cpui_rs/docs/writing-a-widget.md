# Writing a widget

A widget is any type that implements [`View`](../src/view/mod.rs). There are
three required methods and a few optional ones with sensible defaults.

## The three you must write

```rust
impl<M> View<M> for Underline {
    fn measure(&mut self, available: Size) {
        // How big do you want to be, given this much room?
        self.measured = Size::new(available.width, 2);
    }

    fn size(&self) -> Size {
        // What you decided last time you were measured.
        self.measured
    }

    fn render(&self, origin: Point) {
        // Paint yourself with your top-left corner here.
        Renderer::fill_rect(Rect { origin, size: self.measured }, true);
    }
}
```

`measure` and `size` are separate because a parent needs to ask twice: once to
find out what you want, then again after it has decided where you go.

**Never measure text by guessing.** Ask the firmware:

```rust
let width = font.text_width(&self.content);
```

Estimating character widths is what used to push content off the bottom of the
screen.

## Responding to touch

Add `interactions` and declare a rectangle:

```rust
fn interactions(&mut self, origin: Point, out: &mut Interactions<M>) {
    let Some(message) = self.message.clone() else { return };
    out.declare(
        Rect { origin, size: self.size() },
        InputMask::TAP,
        Trigger::Message(message),
    );
}
```

The mask is the important choice:

| Mask | Means |
|---|---|
| `TAP` | A completed tap. Held frames never arrive. |
| `DRAG` | Every frame while a finger is down — sliders want this. |
| `FOCUS` | Joins the Up/Down focus order for hardware buttons. |
| `LONG_PRESS` | A press held past the threshold. |
| `DEFAULT` | `TAP` plus `FOCUS`: what most controls want. |

If your control is small, you do not need to grow it for fingers — `Tappable`
already widens an undersized hit area to the theme's minimum. Wrap it rather
than duplicating that logic.

## Reporting a value

For anything continuous, do not send one message per pixel. Declare a `Trigger`
that carries the arithmetic instead:

```rust
Trigger::Value { make: Msg::Set, max: 100 }   // absolute, from a position
Trigger::Step { make: Msg::Nudge }            // relative, -1 / +1
```

The runtime turns a touch position into a value and calls `make`. This is how
`Slider` and `Stepper` work.

## The optional methods

- `is_flexible()` — return `true` to absorb leftover space along the stacking
  axis. `Spacer` and `Flexible` do.
- `contributes_cross_size()` — return `false` if your measured size should not
  make the parent grow *across* the stacking axis. Only `Spacer` says no, and
  the reason is written down at the trait: a spacer records the full extent it
  was offered, so counting it made rows as tall as the screen.

Most widgets need neither.

## Compose before you implement

Many widgets are arrangements of existing ones and need no `View` impl of their
own. [`Toggle`](../src/widgets/toggle.rs) is a `ListRow`.
[`IconToggle`](../src/widgets/icon_toggle.rs) is an `Icon` in a `Frame`. Reach
for a new `View` implementation only when you genuinely need to control
measurement or painting.

## Testing it

Install the fake host and assert on geometry — no hardware, no simulator:

```rust
let mut widget = Underline::new();
widget.measure(Size::new(200, 60));
assert_eq!(View::<()>::size(&widget).height, 2);
```

The fake also records what was drawn, so you can assert a slider painted a
dithered track before a solid fill. See
[`tests/widgets.rs`](../tests/widgets.rs) for the existing ones.

**Check your test can fail.** Break the widget on purpose and confirm the test
notices. A test that passes against a broken implementation is worse than no
test, because it is believed.
