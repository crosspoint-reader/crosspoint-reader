# `xpui` and FreeInkUI

Notes toward a decision: should `xpui`'s components be built on the SDK's own UI
library, and where would SD-card theming attach?

**Short answer: yes, and it is a smaller job than it looks — because it is a
change to `backend`, not to `xpui`.** The rest of this explains why.

---

## Where things actually stand

Three facts worth establishing before anything else, because they change the
question.

**FreeInkUI is already here.** `platformio.ini` symlinks
`freeink-sdk/libs/ui/FreeInkUI` as a library, the current SDK pin ships **33
components**, and **44 files in our firmware already use it**. `xpui` and FUI
have been compiled into the same binary this whole time.

**`feat-fui` is not where to work.** It is 82 commits behind `feat-touch-ui`, a
clean spike of just the FUI conversion, with no `x4pro` environment and no
simulator of its own.

**The two are near-siblings.** They were designed against the same constraints
and arrived at strikingly similar shapes.

| Concept | `xpui` | FreeInkUI |
|---|---|---|
| Geometry | `Point`, `Size`, `Rect`, `Insets` | `Point`, `Size`, `Rect`, `Insets` |
| What input an element accepts | `InputMask` (TAP, FOCUS, DRAG, LONG_PRESS, ADJUST) | `InputMask` (+ `SwipeLeft/Right`, `Prev/Next`, `Back`, `Confirm`) |
| Focus movement | `move_focus(delta)`, wraps | `moveFocus(delta)`, wraps |
| Theme geometry | `ThemeMetric` enum | `ThemeTokens` struct |
| Device capabilities | host traits | `DeviceContext{hasTouch, hasButtons}` |
| What a control reports | typed per-screen message | `ActionId` |
| Gestures | `SwipeDir` | `SwipeDir` |

FUI's `InputMask` is a superset of ours, and its `ThemeTokens` covers the same
ground as `ThemeMetric` — `minTouchSize`, `rowHeight`, `headerHeight`, a spacing
scale. This is convergent design, not coincidence.

---

## What each one is for

They are not competing, which is the important thing to say out loud.

**FUI is a component library.** The app drives it: build props, call a
component, read back an `ActionId`. It owns *how things look* — `StyleSet`,
`Paint`, `State`, and per-component style tokens.

**`xpui` is a declarative layer.** A screen describes a tree and receives typed
messages; the framework owns measurement, focus, input routing and repaint.
It owns *how a screen is written*.

FUI has no equivalent of `body()`/`update()`. `xpui` has no equivalent of 33
styled components. Each is the other's missing half.

---

## What "xpui on FUI" actually means

`xpui` reaches the firmware only through five `Host` traits. `Chrome` is already
the seam where lists, headers, dialogs and button hints are drawn **by the
firmware, not by `xpui`**. Today `backend` implements that against `UITheme`.
Pointing it at FUI instead changes:

- `lib/backend_rs/src/theme.rs` and the C++ in `src/rust_ffi/`
- **nothing in `xpui`**
- **nothing in any screen**

That is the dependency inversion paying for itself. It was built for exactly
this.

### How much drawing does `xpui` actually do?

Measured, not guessed — host-delegated calls versus raw primitives:

| Already delegates | Draws primitives itself |
|---|---|
| `Section` (4), `Stepper` (2), `Modal` (2), `ProgressBar` (2), `IconToggle` (1), `Toggle` | `Slider` (4), `Image` (3), `Text` (1), `Divider` (1) |

`Text`, `Divider` and `Image` are irreducible — draw a string, a line, blit a
bitmap — under any framework. So the geometry `xpui` genuinely owns and could
hand to FUI is, in practice, **one widget: `Slider`**.

The work is therefore not "rewrite the widgets". It is "re-point `Chrome` at
FUI, and let `Slider` use FUI's slider".

> **Correction, from doing it.** "Re-point `Chrome` at FUI in `backend`" does not
> describe real work: `Chrome` already lands on `BaseTheme`, and it is
> `BaseTheme` that migrates. What actually decides each widget is what the
> **C++ equivalent** draws through — already-FUI means `xpui` is the odd one out
> and we convert it in `src/rust_ffi/`; still hand-drawn means converting only
> Rust would split the look. `Slider` and `List` moved on that basis.
> [fui-component-coverage.md](fui-component-coverage.md) tracks the rest.

---

## Where SD-card theming attaches

Justin's plan — author themes as XML on the SD card, no firmware code — has more
groundwork already done than the conversation suggested. FUI ships
`ThemeDocument`, `ThemeTokens`, `StyleSet` and an `AssetResolver` interface.
An XML file is a way to populate a `ThemeDocument`.

The layering that follows:

- **Parsing belongs in the firmware.** Not in `xpui`: a `no_std` UI framework
  should not know what XML is, and it must not name a file format. Not in FUI
  either, most likely — FUI consumes a `ThemeDocument`; something else fills it.
- **`xpui` already has the right seam.** `ThemeMetric` and `Chrome` are exactly
  "ask the host what things look like". A theme change is the host answering
  differently. No `xpui` change is needed for themes to work.
- **If `backend` draws via FUI, Rust screens inherit themes for free.** This is
  the strongest argument for the section above: the theming work happens once,
  in FUI, and both C++ and Rust screens get it.
- **The open question is invalidation.** When a theme loads, something must
  rebuild the screen. `xpui` has `request_update()`, but nothing currently calls
  it on a theme change, and cached measurements would go stale. Worth settling
  before the XML lands rather than after.

---

## Recommendation

> **Overtaken by events.** This branch *is* `feat-fui` now — the work was
> transplanted onto it, which cost far less than feared (the framework is new
> files almost throughout) and brought its own simulator along. Points 1 and 2
> below were wrong: there was nothing to "re-point", because `Chrome` already
> lands on `BaseTheme`, and it is `BaseTheme` that migrates. What survived is
> the rule the conversions actually follow — see
> [fui-component-coverage.md](fui-component-coverage.md).

1. ~~**Do not move to `feat-fui`.**~~ Done, and it worked.
2. ~~**Re-point `Chrome` at FUI in `backend`.**~~ Not a real task. Convert per
   widget instead, deciding by what the C++ equivalent draws through.
3. **Move `Slider` onto FUI's slider** — done, along with `List` and the
   option dialog.
4. **Keep `xpui`'s declarative layer as it is.** It is what FUI lacks, it is
   why screens are testable on a laptop, and it is the part worth pitching.
5. **Settle theme invalidation** before the XML format arrives.

For the pull request Justin invited: the honest pitch is not "here is another UI
framework". It is "here is a declarative, testable way to write screens, and it
already sits on top of yours".

---

*Every figure here was read from the tree: component and file counts from the
SDK pin and `src/`, delegation counts from `lib/xpui_rs/src/widgets/`, branch
distances from `git rev-list`.*
