## UI and Orientation Guidelines

### Orientation-Aware Logic

* No Hardcoding: Never assume 800 or 480. Use renderer.getScreenWidth() and renderer.getScreenHeight().
* Viewable Area: Use renderer.getOrientedViewableTRBL() to stay within physical bezel margins.

### Logical Button Mapping

**Source**: [src/MappedInputManager.cpp:20-55](../../src/MappedInputManager.cpp)

Constraint: Physical button positions are fixed on hardware, but their logical functions change based on user settings and screen orientation.

**Button Categories**:

1. **Physical Fixed** (Up/Down side buttons):

   - `Button::Up` → Always `HalGPIO::BTN_UP`

   - `Button::Down` → Always `HalGPIO::BTN_DOWN`

2. **User Remappable** (Front buttons):

   - `Button::Back` → Maps to `SETTINGS.frontButtonBack` (hardware index)

   - `Button::Confirm` → Maps to `SETTINGS.frontButtonConfirm`

   - `Button::Left` → Maps to `SETTINGS.frontButtonLeft`

   - `Button::Right` → Maps to `SETTINGS.frontButtonRight`

3. **Reader-Specific** (Page navigation with optional swap):

   - `Button::PageBack` → Uses side button (swappable via `SETTINGS.sideButtonLayout`)

   - `Button::PageForward` → Uses side button (swappable)

**Implementation**:

- Activities use **logical buttons** (e.g., `Button::Confirm`)
- `MappedInputManager` translates to **physical hardware buttons**
- User can remap front buttons in settings
- Orientation changes handled separately by renderer coordinate transforms

**Rule**: Always use `MappedInputManager::Button::*` enums, never raw `HalGPIO::BTN_*` indices (except in ButtonRemapActivity).

### Input frames and ownership

Sources: [main.cpp](../../src/main.cpp),
[MappedInputManager](../../src/MappedInputManager.cpp), and
[ActivityManager::loop()](../../src/activities/ActivityManager.cpp).

The main loop updates `MappedInputManager` before dispatching the active activity.
Normal activity loops read this shared snapshot. A second `update()` can erase
an edge before another input owner sees it. Consume input through the existing
input and UI protocols, not another update or a delay.

| Query | Physical-button meaning | Typical use |
| --- | --- | --- |
| `wasPressed(button)` | Press edge in this frame | Immediate action |
| `wasReleased(button)` | Release edge in this frame | Short action after a complete gesture |
| `isPressed(button)` | Current held state | Hold timing and transition checks |
| `wasLongPressed(button, thresholdMs)` | One threshold event during a hold | Long action with shared release suppression |

Logical touch, Home, and shared-button actions can publish both edges in one frame.
Trace these synthesized events before treating a logical edge as a physical hold.
Keep each physical gesture with one input owner and one semantic action.
Preserve intentional navigation repeats and their existing cadence.
For short and long actions on one button, the long action must not also trigger
the short action on release.

`MappedInputManager::wasLongPressed()` already suppresses the following release.
`ActivityManager::loop()` consumes suppressed releases before activity dispatch.
Reuse that contract when it fits the caller. Check shared popup and FreeInkUI
host handling before adding a separate long-press flag or release guard.

### Activity and popup transitions

Before changing edge handling, trace the main-loop update, originating owner,
child or popup, result callback, and resumed parent. Identify all press, release,
held-state, and long-press consumers along that path.

Prefer completing the gesture in its original owner when that preserves every caller's contract.
If a transition occurs while a button is held, check whether existing handling
already prevents the next owner from acting on that hold or release.
Add a local release guard only for a demonstrated gap.

If a guard is needed, initialize it from the logical button's `isPressed()` state.
The result callback can run after the original edge's frame.
Clear the guard on release and consume that release frame too.
Keep independent touch input usable. Do not block the whole input loop for an
inherited physical hold when the shared UI can route unrelated touch actions.

### Input verification matrix

Use these cases for changed input behavior. Run applicable host checks and give
the human device steps with expected results. Include button remapping and
relevant orientations. Host checks do not establish hardware behavior.

| Scenario | Expected result |
| --- | --- |
| Press opens a child or popup | The inherited hold and release cause no unintended action there |
| Release opens a child or popup | The new owner waits for a new gesture |
| Child closes on press | The resumed parent does not act on that gesture's release |
| Long press fires | The long action runs once and release does not run the short action |
| Held navigation repeats | Repeats keep their intended cadence without an extra release action |
| Logical buttons are remapped or orientation changes | Each gesture still reaches the intended logical action |
| Touch activates the same UI | A physical-button guard does not block independent touch actions |

### UITheme (The GUI Macro)

* Use the shared FreeInkUI hosts for controls and interaction; see Shared UI and input below.
* Use `GUI` (UITheme) for theme metrics and shared chrome, and `GfxRenderer` for drawing and oriented geometry.
* Derive fonts, colors, and layout from those contracts rather than hardcoding them.

---

## Common Patterns

### Singleton Access

**Available Singletons**:

```cpp
#define SETTINGS CrossPointSettings::getInstance()  // User settings
#define APP_STATE CrossPointState::getInstance()    // Runtime state
#define GUI UITheme::getInstance()                   // Current theme
#define Storage HalStorage::getInstance()            // SD card I/O
#define I18N I18n::getInstance()                     // Internationalization
```

### Activity Lifecycle and Memory Management

**Source**: [ActivityManager.cpp](../../src/activities/ActivityManager.cpp),
[ActivityManager.h](../../src/activities/ActivityManager.h).

Activities are heap-allocated and owned by `ActivityManager` through
`std::unique_ptr`. Replace/pop calls `onExit()` before destruction. Push retains
the parent on the activity stack, so navigation does not always free the old
screen; include stacked parents in the memory budget.

Use the manager's navigation methods rather than assigning or deleting activity
pointers yourself. Destruction is serialized with rendering through `RenderLock`;
`onEnter()` runs after releasing that lock. The loop acquires the lock only where
shared render state requires it. `requestUpdateAndWait()` must not be called
while holding it.

**Memory Implications**:

- Replace/pop releases the exited activity; push keeps its parent alive
- Any memory allocated in `onEnter()` MUST be freed in `onExit()`
- FreeRTOS tasks MUST be deleted in `onExit()` before activity destruction
- Member `HalFile` handles MUST be closed in `onExit()` (local `HalFile` variables auto-close via destructor)

**Activity Pattern**:

```cpp
void onEnter()  { Activity::onEnter(); /* alloc: buffer, tasks */ render(); }
void loop()     { /* handle the input snapshot supplied by the main loop */ }
void onExit()   { /* free: vTaskDelete, free buffer, close member FsFiles */ Activity::onExit(); }
```

**Critical**: Free resources in reverse order. Delete tasks BEFORE activity destruction.

### FreeRTOS Task Guidelines

**Source**: [ActivityManager::begin()](../../src/activities/ActivityManager.cpp).
The manager owns a shared render task; do not add a per-screen render task.
The guidelines below apply when an activity needs a separate worker.

**Pattern**: See Activity Lifecycle above. `xTaskCreate(&taskTrampoline, "Name", stackSize, this, 1, &handle)`

**Stack Sizing** (in BYTES, not words):

- Size activity worker stacks from their call chains and measured high-water
  marks; 2048/4096 bytes are examples for simple/network work, not render defaults.
- The shared render task and vector-font loop have larger conditional stacks;
  check `ActivityManager::begin()` and `SET_LOOP_TASK_STACK_SIZE` in `main.cpp`.
- Monitor: `uxTaskGetStackHighWaterMark()` if crashes

**Rules**: Always `vTaskDelete()` in `onExit()` before destruction. Use mutex if shared state.

Network activities also release their WiFi/power-lock ownership on exit.
Trace the existing `HalPowerManager` lock and low-power transition protocol
before changing network, sleep, or logging paths. For dictionary memory and
`.dict.dz` behavior on boards without PSRAM, read
[docs/dictionary.md](../../docs/dictionary.md).

### Global Font Loading

**Source**: [src/main.cpp](../../src/main.cpp),
[lib/EpdFont/builtinFonts/](../../lib/EpdFont/builtinFonts/).

Built-in fonts are registered at firmware startup. The original baseline includes:

- Noto Serif: 12, 14, 16, 18pt (4 styles each: regular, bold, italic, bold-italic)
- Noto Sans: 12, 14, 16, 18pt (4 styles each)
- Ubuntu UI fonts: 10, 12pt (2 styles)

Inspect `builtinFonts/all.h` and the registration code for the current font set;
SD and vector fonts have runtime ownership, separate from these static objects.

**Compilation Flag**:

```cpp
#ifndef OMIT_FONTS
  // Most fonts loaded here
#endif
```

**Implications**:

- Fonts stored in **Flash** (marked as `static const` in `lib/EpdFont/builtinFonts/`)
- Font rendering data cached in **DRAM** when first used
- `OMIT_FONTS` can reduce binary size for minimal builds
- Font IDs defined in [src/fontIds.h](../../src/fontIds.h)

**Usage**:

```cpp
#include "fontIds.h"

renderer.insertFont(FONT_UI_MEDIUM, ui12FontFamily);
renderer.drawText(FONT_UI_MEDIUM, x, y, "Hello", true);
```

---

### Shared UI and input

Read [docs/contributing/touch-and-ui.md](../../docs/contributing/touch-and-ui.md)
when adding or changing a screen. Use `UiListActivity` for lists,
`UiTabListActivity` for tabbed lists, and `UiAppHost` for custom FreeInkUI layouts.
Use the existing popup/dialog hosts for modal controls. Keep the shared
interaction-table publication and `uiReady` handshake in `UiAppHost`; route
logical buttons and touch through the existing stack.

- `fui::ListNav` separates selection from viewport scrolling. Swipes scroll the
  viewport; buttons move selection and bring it into view. The tabbed selection
  ring reserves position 0 for the tab bar, while row indices remain zero-based.
- Navigation uses `ButtonNavigator` press/hold callbacks; Confirm and Back still
  have release/long-press semantics in their callers. Preserve those distinctions
  when moving input handling, and trace the main-loop snapshot lifecycle first.
- For large lists, use `fui::ListProps::rowProvider` as in
  `FileBrowserActivity::provideRow` instead of materializing every row. Small
  bounded lists may use fixed arrays; other row storage belongs to the activity
  and is reused across renders. Keep steady-state repaint allocation-free.
- Global Home/Back/control-center gestures and header Back taps have shared
  routing. Reader overlays and end-of-book menus must consume their input before
  page turns; do not add competing per-screen gesture or coordinate handlers.
- Cover-grid themes and runtime vector fonts are gated on PSRAM capability;
  other boards retain supported list themes and `.cpfont` fonts. Check the
  existing gates before exposing settings or allocating their working sets.

### Runtime fonts and cache ownership

Read [docs/sd-card-fonts.md](../../docs/sd-card-fonts.md) for discovery, family
precedence, styles, sizes, and UI fallback. `.cpfont` works on all devices;
TTF/OTF/TTC rendering is compiled behind `CROSSPOINT_VECTOR_FONTS` from
`VectorFontSupport.h` and requires enabled PSRAM. Font upload accepts TTF/OTF on
PSRAM devices as well as `.cpfont`; check `FontInstaller` and the web handler
because older user documentation may describe only `.cpfont` upload.

Use `FontCacheManager::PrewarmScope` for the scan/prewarm/render lifecycle.
A screen can mix built-in, SD, and vector fonts and resolved styles; retain
that distinction when changing scanners. SD UI fallback must match the UI size.
`SdCardFont` keeps reusable mini arenas and persistent advances across renders;
`clearCache()` is not a promise that all resident memory was freed. Use
`FontCacheManager::releaseSdFontCaches()` at existing heap-critical transitions
when all rebuildable SD/vector caches must be released while fonts stay loaded.
Inspect the implementation for kerning/ligature view lifetimes before changing
cache release or reuse.
