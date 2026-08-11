# How a frame runs

The [README](../README.md) describes three conversations. This is what actually
happens between them, in order. It is short because the design is small.

## The cast

| | |
|---|---|
| **Your screen** | A struct with `body()` and `update()`. Holds state; touches nothing else. |
| **The runtime** | [`src/screen/mod.rs`](../src/screen/mod.rs). Drives one screen. |
| **The view tree** | Whatever `body()` returned. Thrown away after each use. |
| **The host** | The firmware. Paints, measures, reads input. |

The runtime is the only part that talks to everyone. Your screen and the
firmware never meet.

## A frame with a touch in it

The firmware calls into the runtime once per loop:

1. **Read input.** The runtime asks the host what happened — a tap, a drag, a
   button, nothing.
2. **Build the tree.** It calls your `body()`. You return a fresh description of
   the screen; nothing is kept between frames.
3. **Measure it.** Each view is offered a size and reports what it actually
   wants. Stacks divide space between children; a `Spacer` claims what is left.
4. **Collect what is touchable.** Each view *declares* rectangles it responds to,
   along with the message to send. A slider declares its track, a row declares
   itself, a label declares nothing.
5. **Resolve.** [`src/screen/routing.rs`](../src/screen/routing.rs) finds which
   declaration the touch landed in, scanning backwards so the innermost control
   wins.
6. **Deliver.** The runtime calls `update()` with that message. You change your
   state. The runtime asks the firmware to repaint.

Painting is the same steps 2–4 followed by a `render` pass, because the tree no
longer exists — it was dropped at the end of the last frame.

## Why rebuild the tree every time

Because it removes a whole category of bug. There is no stored tree that can
disagree with your state, no "I changed the value but the screen still shows the
old one", no invalidation to get wrong. `body()` is a pure function of your
struct, so what you see is always what you hold.

The cost is real: `body()` runs on every repaint and on every frame of a drag,
and it allocates. That is the trade, made deliberately. Keep expensive work in
`update()`, where it happens once per event, rather than in `body()`.

## Messages, not callbacks

A widget does not run your code. It carries a value you gave it, and hands that
value back:

```rust
Stepper::new(self.level).on_change(Msg::Set)
```

`Msg::Set` here is not a call — it is the constructor of an enum variant, used
as a function. When the track is dragged, the runtime builds `Msg::Set(72)` and
passes it to `update()`.

This is why `update()` is the only place your state changes, and why you can
test a screen by calling `update()` directly with no UI at all.

## Declaring instead of hit-testing

A view says *what* it responds to, never *whether it was hit*:

```rust
out.declare(rect, InputMask::TAP, Trigger::Message(msg));
```

The mask matters. A control that only accepts `TAP` never sees the frames while
a finger is held down, so it cannot be dragged by accident; a slider asks for
`DRAG` and does. The runtime handles focus, auto-repeat on a held button and the
minimum touch target, because every control declares the same way.

## Where the firmware fits

The runtime never calls the firmware directly. It goes through five traits in
[`src/host/`](../src/host/) — `Canvas`, `TextMetrics`, `Chrome`, `InputSource`,
`Clock` — reached through small façades like `Renderer::fill_rect(..)`.

Two consequences worth stating:

- `cpui` compiles with **no dependencies** and cannot name a firmware symbol.
- Tests install a fake host, so layout, input routing and widget behaviour are
  all testable on a laptop. That is how there are 58 tests and no simulator.

See [host.md](host.md) to implement one.

## The one piece of global state

The installed host. `install()` is called once at startup and only read
afterwards. It exists so a widget can write `Renderer::fill_rect(..)` instead of
threading a context parameter through every `measure`, `render` and
`interactions` call in the tree.

On the device the two callers run on different tasks, so the runtime installs it
from both entry points rather than assuming which arrives first.
