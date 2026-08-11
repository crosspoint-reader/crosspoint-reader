# FreeInkUI coverage

What `xpui` draws through, what FreeInkUI ships, and what is left to do.

**The intent is reuse.** Where FUI has a component and CrossPoint's C++ already
draws with it, `xpui` renders through the same component rather than keeping its
own version — that is what makes a Rust screen and a C++ screen the same pixels
by construction, and what will let SD/XML themes reach Rust screens for free.
Where FUI has nothing, we build it in `xpui`, and those may later be worth
contributing upstream.

## The rule

Decide per widget by what the **C++ equivalent** draws through:

| C++ equivalent uses… | Converting `xpui`… | Whose change |
|---|---|---|
| the FUI component already | *removes* a divergence — `xpui` is the odd one out | ours, in `src/rust_ffi/` |
| a hand-drawn `BaseTheme` method | *creates* one — Rust would stop matching C++ | `BaseTheme`, upstream |
| nothing (irreducible primitive) | nothing to convert | — |

Converting in `src/rust_ffi/` touches only our own files: no `BaseTheme` edit,
no effect on any C++ activity.

## Where `xpui` stands

| `xpui` widget | Draws through | FUI component | Status |
|---|---|---|---|
| `NavigationScreen` header | `BaseTheme::drawHeader` | `header` + `batteryIndicator` | **on FUI** — inherited free; the one method already converted |
| `Slider` | `cpp_theme_draw_slider` | `slider` | **on FUI** — C++ used it in 3 activities; `xpui` held the last hand-drawn one |
| `List` | `cpp_theme_draw_list` | `list` (via `Screen::list`) | **on FUI** — the same path 36 files under `src/activities/` use |
| `Modal` | `BaseTheme::drawOptionPopup` | `option-dialog` | **blocked** — the C++ `OptionPopup` is hand-drawn too; converting only Rust splits the look |
| `ProgressBar` | `BaseTheme::drawProgressBar` | `progress-bar` | **mixed** — one C++ activity uses the component, the theme method is hand-drawn |
| `Section` | `BaseTheme::drawSubHeader` | — | **blocked** — hand-drawn, no FUI equivalent |
| button hints | `BaseTheme::drawButtonHints` | — | **blocked** — hand-drawn, no FUI equivalent |
| `Toggle` | list row value text | `toggle`, `toggle-row` | **not a port** — CrossPoint shows a word, not a switch; adopting one is a design change |
| `Text` | `draw_text` | — | irreducible |
| `Divider` | `draw_line` | — | irreducible |
| `Image` / `Icon` | `draw_image`, `draw_icon` | — | irreducible |

`Stepper`, `IconToggle` and `ListRow` compose the rows and sub-headers above and
have no separate drawing of their own.

## FUI components CrossPoint uses

`header`, `battery-indicator` (both inside `BaseTheme::drawHeader`), `list` and
`keyboard` (through `FreeInkApp::Screen`), `slider`, `tab-bar`, `button`,
`progress-bar`.

## FUI components nothing uses yet

Neither C++ nor Rust draws these. Adopting one means a screen wants it — they
are not work in themselves, but they are what `xpui` would gain by sitting on
FUI rather than beside it.

- **Overlays**: `option-dialog`, `context-menu`, `popup`, `message-panel`, `toast`
- **Lists**: `setting-row`, `toggle-row`, `stepper-row`, `radio-group`, `dropdown`, `table`
- **Controls**: `checkbox`, `text-field`, `text-area`
- **Bars**: `status-bar`, `gesture-bar`, `tap-zones`, `reader-chrome`
- **Library**: `book-card`, `cover-carousel`, `cover-grid`, `metric-card`

`option-dialog` is the notable one: it already covers what `Modal` needs
(`dimBackground` matches our `Scrim`, `verticalOptions` matches our row layout),
and it is called from nowhere in the firmware.

## What `xpui` has that FUI does not

`Text`, `Divider` and `Image`/`Icon` are irreducible primitives, but the
declarative layer around them is not: the `View` trait, the layout containers,
`Interactions`/`Trigger` routing, focus and the screen runtime have no FUI
counterpart. FUI has no `body()`/`update()`; `xpui` has no styled component
library. Each is the other's missing half.

---

*Every figure here was read from the tree: components from the SDK pin under
`freeink-sdk/libs/ui/FreeInkUI/include/components/`, call sites from `src/`.*
