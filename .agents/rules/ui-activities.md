## UI and Orientation Guidelines

### Orientation-Aware Logic

Use `renderer.getScreenWidth()`, `getScreenHeight()`, and
`getOrientedViewableTRBL()`; never assume panel dimensions or bezel margins.
Shared UI hosts own controls; `GUI` supplies theme metrics/chrome and
`GfxRenderer` supplies drawing and oriented geometry.
Derive fonts, colors, and layout from those contracts rather than hardcoding them.

### Shared UI and input

Use `UiListActivity` for lists, `UiTabListActivity` for tabbed lists,
`UiAppHost` for custom FreeInkUI layouts, and existing popup/dialog hosts.
Keep interaction-table publication and the `uiReady` handshake in `UiAppHost`.
Consult the relevant section of [touch/UI guidance](../../docs/contributing/touch-and-ui.md)
when changing a screen.

- `fui::ListNav` separates selection from viewport scrolling. Swipes scroll;
  buttons select and bring rows into view. The tabbed ring reserves position 0
  for the tab bar; row indices remain zero-based.
- `ButtonNavigator` owns navigation press/hold callbacks. Confirm/Back callers
  retain release/long-press semantics.
- Large lists use `fui::ListProps::rowProvider`, as in
  `FileBrowserActivity::provideRow`. Keep bounded row storage with the activity
  and reuse it; steady-state repaint must remain allocation-free.
- Global Home/Back/control-center gestures and header Back taps have shared
  routing. Reader overlays/end-of-book menus consume input before page turns.
  Reuse these owners instead of per-screen gesture or coordinate handlers.
- Cover-grid themes and vector fonts require PSRAM capability; other boards
  retain supported list themes and `.cpfont` fonts.

### Logical Button Mapping

Activities use `MappedInputManager::Button`, not raw `HalGPIO::BTN_*` indices
except in `ButtonRemapActivity`. Front buttons are user-remappable; page buttons
follow `SETTINGS.sideButtonLayout`. Inspect
[MappedInputManager](../../src/MappedInputManager.cpp) for the mapping.

### Input frames and ownership

The main loop updates `MappedInputManager` before
[ActivityManager::loop()](../../src/activities/ActivityManager.cpp).
Activities read that shared snapshot; a second update can erase an edge.

| Query | Physical-button meaning |
| --- | --- |
| `wasPressed()` | Press edge this frame |
| `wasReleased()` | Release edge this frame |
| `isPressed()` | Current held state |
| `wasLongPressed()` | One threshold event, with following-release suppression |

Touch, Home, and shared-button actions may synthesize both edges in one frame.
Trace them before assuming a physical hold. Keep one gesture with one semantic
owner, preserve navigation-repeat cadence, and prevent a long action also firing
the short release action. `wasLongPressed()` already suppresses that release;
`ActivityManager::loop()` consumes it before activity dispatch.

### Activity and popup transitions

Trace the update, originating owner, child/popup, callback, and resumed parent,
including press, release, held-state, and long-press consumers. Prefer completing
the gesture in its original owner when callers permit it. Add a local release
guard only for a demonstrated gap in shared handling.

Initialize a needed guard from the logical button's `isPressed()` state: a
callback may run after the originating edge's frame. Clear it on release and
consume that release frame. Keep independent touch usable.

### Input verification matrix

The main agent runs applicable host checks; give the human device steps with
button remapping and relevant orientations. Verify these changed paths:

| Scenario | Expected result |
| --- | --- |
| Press opens child/popup | Inherited hold/release causes no unintended action |
| Release opens child/popup | New owner waits for a new gesture |
| Child closes on press | Parent ignores that gesture's release |
| Long press | One long action; no short release action |
| Held navigation | Intended repeats; no extra release action |
| Remapping/orientation | Gesture reaches the intended logical action |
| Touch | Physical-button guards leave independent touch usable |

### Activity Lifecycle and Memory Management

`ActivityManager` owns heap-allocated activities through `std::unique_ptr`.
Replace/pop calls `onExit()` before destruction; push retains the parent, which
must remain in the peak memory budget. Use manager navigation methods.

Destruction is serialized with rendering through `RenderLock`; `onEnter()`
runs after releasing it. Never call `requestUpdateAndWait()` while holding it.
Free resources in reverse order: stop/delete workers before destruction, release
`onEnter()` allocations in `onExit()`, and close member `HalFile` handles there.
Local file lifetime follows the [HAL contract](architecture-hal.md#file-handle-lifetime).

### FreeRTOS Task Guidelines

`ActivityManager::begin()` owns the shared render task; do not add per-screen
render tasks. Worker stacks are sized in bytes, using call chains and measured
`uxTaskGetStackHighWaterMark()`. Check the existing render stack and
`SET_LOOP_TASK_STACK_SIZE` rather than copying example sizes.
Workers require shared-state synchronization and `vTaskDelete()` in `onExit()`
before activity destruction.

Network flows release WiFi and power locks when they end. Trace the existing
`HalPowerManager` low-power protocol before changing network, sleep, or logging.
For non-PSRAM dictionary memory and `.dict.dz`, consult [dictionary.md](../../docs/dictionary.md).

### Global Font Loading

Built-ins are registered in `src/main.cpp`; inspect
`lib/EpdFont/builtinFonts/all.h` and `src/fontIds.h` for the current set.
Built-in glyph data uses static flash storage; rendering caches consume DRAM.
SD/vector fonts have separate runtime ownership. Check `OMIT_FONTS` when changing
registration or build footprint.

### Runtime fonts and cache ownership

[SD-font documentation](../../docs/sd-card-fonts.md) defines discovery, precedence,
styles, sizes, and fallback. `.cpfont` works on all devices; vector TTF/OTF/TTC
requires `CROSSPOINT_VECTOR_FONTS` and enabled PSRAM. Inspect `FontInstaller` and
the web handler for current upload support.

Use `FontCacheManager::PrewarmScope` for scan/prewarm/render. Mixed built-in,
SD, and vector fonts/styles must remain distinct; SD UI fallback matches UI size.
`SdCardFont` retains mini arenas and advances after `clearCache()`.
Use `releaseSdFontCaches()` at established heap-critical transitions to release
rebuildable caches while keeping fonts loaded. Check kerning/ligature view
lifetimes before changing release or reuse.
