# Swipe hints

`src/util/EdgeSwipe.h` owns the recognizer and tuning constants. Distances use
millimeters and `MappedInputManager::touchDpi()` converts them using the board's
panel density. The current density map covers the 220 ppi X4 family and the
235 ppi Sticky/Paper Mono panels; check and extend that map when enabling a
new panel.

`MappedInputManager` maps the left edge to Back, bottom to Home, and top to the
frontlight panel when available or the reader menu. `EdgeSwipeIndicator` selects
the matching icons, generated from `edgeSwipeIcons.manifest`. Bottom is now Home on
Home-key boards too; old bottom-swipe reader-menu settings fall back to Tap.
The configured capacitive Home-key actions remain available.
Inward swipes from the right edge use ordinary activity input, such as reader
page turns, and show no swipe hint.

`src/components/EdgeSwipeIndicator.cpp` owns the overlay. It retains a 1 KiB
nothrow heap allocation on touch boards so the snapshot stays off the render
stack and gestures never allocate. Each draw snapshots a bounded, byte-aligned
region, paints through the theme, sends the existing B/W fast refresh, and
restores the write framebuffer immediately. Cancel sends the clean framebuffer
through the same refresh path on B/W screens. Over grayscale, the active screen
redraws its content to restore the gray pixels. A committed action that does not
navigate gets cleanup after 200 ms.

The renderer tracks normal display updates so a page or toolbar repaint can
replace a hint without an extra cleanup refresh. Before drawing, it finishes
pending B/W async work and restores the differential baseline if needed.
Restoring the tab's pixels in memory does not reseed that baseline: it must
continue to describe the visible hint until the next refresh erases it.
Activity transitions and controller read errors cancel the active edge contact.

The main loop recognizes input while the existing render task services the
latest requested stage. Input and display changes wake the task; timed waits
cover refresh spacing and delayed cleanup. Refreshes are synchronous and spaced at
least 150 ms after completion. A contact gets at most three hint updates and
one final cleanup. Repeated reversals that exhaust this budget erase the tab
and hide it for the rest of that contact; recognition and cancellation continue.
This resolves the conflict between unlimited threshold reversals and a fixed
refresh budget.

Tabs sit flush with the screen edge, with a rounded inner curve and small dark
24 px icons with a white outline on a light gray dithered fill. They extend
3.5 mm into the screen and use one fixed size throughout the swipe.
Dithering keeps the refresh in B/W mode without a grayscale underlay.

The hint appears after 2 mm of inward travel. Releasing after 7 mm commits the
action even after a slow swipe or a pause. A fast inward flick can commit after
2 mm. The recognizer calls these thresholds Peek and Armed; they share one
visual appearance. Crossing between them does not cause another refresh.

Settings > Controls > Show swipe hints defaults to On. Disabling it changes
only the visuals. Settings > Display > Show checkbox also defaults to On. Turning
it off replaces settings-list checkbox graphics with translated On/Off values.

## Display support

Hints use `GfxRenderer::displayBuffer(FAST_REFRESH)` and the existing display
pipeline. They do not require a controller-specific window API or display-driver
changes. The SDK touch-input changes are still required for live contacts and
cancellation.

A small tab change sends the ordinary full framebuffer. The selected driver
owns its waveform, inversion, power, and previous-frame handling. A Fast request
may be promoted to a clearing refresh by the normal display policy; window-only
refreshes and the absence of whole-screen flashes are not guaranteed.

Tabs can appear over B/W pages, antialiased text, and grayscale images. Drawing
waits until the framebuffer is available and the screen's render has finished;
it never interrupts grayscale plane composition or a framebuffer loan.

The hint uses the screen's B/W framebuffer. Depending on the panel's waveform,
this can change gray pixels while the hint is visible. When the hint is cleared,
the active screen redraws its content to restore grayscale, including when a
swipe reverses while the finger stays down. A normal screen transition can
replace the hint without a separate restoration. Inverted B/W pages use the
usual B/W cleanup. The simulator cannot characterize e-ink waveforms.

Controller calibration maps touch coordinates through the full raw range, but
source inspection cannot prove that fast swipes report the outermost pixels.
There is no speculative extrapolation: an in-screen swipe must remain an
in-screen swipe. Record first samples on hardware before enabling extrapolation
or changing the 3.5 mm zone.

## Verification

Host recognizer check:

```sh
g++ -std=c++20 -Wall -Wextra -Werror -Isrc test/edge_swipe/EdgeSwipeTest.cpp -o /tmp/edge-swipe-test
/tmp/edge-swipe-test
```

Host indicator and renderer checks:

```sh
python3 test/edge_swipe/run_indicator.py
```

Builds: `pio run -e x4pro`, `pio run -e default`, and
`pio run -e simulator_x4_pro`.

On a touch device with a B/W page:

1. In all four orientations, swipe bottom at both horizontal extremes, and
   the left edge at its vertical extremes. Check the clamped, fixed anchor,
   Home/Back icons, and icon visibility under a real finger.
2. Move through 2 mm and 7 mm, reverse through both thresholds, and release.
   A slow release below 7 mm must cancel; release at 7 mm must commit. The hint
   must keep one size, and reversing below 2 mm must erase it.
3. Flick at least 2 mm, then compare with a swipe that pauses for over 100 ms
   before release. Only the moving flick should commit below 7 mm.
4. Add another finger during Peek/Armed; change orientation; and exercise
   controller cancellation. None may commit an old edge gesture.
5. Check edge-parallel scrolling, interior swipes, page turns, taps, long
   presses, header Back, and physical buttons. Toggle both new settings, save,
   restart, and confirm persistence and checkbox On/Off values.
6. Monitor `EDGE` debug logs. Each contact must show at most three
   hint updates and one cleanup. Check that page navigation absorbs cleanup.
   Inspect the glass for ghosting and whole-screen flashes; logs alone cannot
   establish either behavior. Repeat on inverted B/W output.
   Swipe inward from the right edge: no Back action or hint should appear;
   reader page turns and ordinary scrolling should follow their settings.
7. Enable text anti-aliasing and check all three hints in the reader. Cancel
   by reversing below 2 mm while still holding, then by releasing. The hint
   must disappear and AA must return without a page turn. Repeat over EPUB
   images, grayscale XTC pages, and the image viewer; inspect gray pixels
   beneath and outside the tab. Compare with AA off, Settings, and File Browser.
8. Capture touch-down coordinates at each edge, including fast swipes. If the
   controller misses the 3.5 mm zone, measure its sample delay before deciding
   how to recover those gestures without stealing interior swipes.
9. Monitor free heap across repeated contacts. The indicator allocates once;
   heap must remain stable and above the project's 50 KiB device threshold.
10. While holding an armed edge swipe, navigate with a physical button or Home
    key, or enter sleep. The new screen must appear promptly; lifting the finger
    must not trigger another action. Swipe after opening the reader toolbar and
    check that hints appear without disturbing it.
