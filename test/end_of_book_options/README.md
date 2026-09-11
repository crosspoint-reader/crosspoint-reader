# End-of-book menu host tests

The target compiles the complete production `EndOfBookOptions.cpp` body and the
shared `ReaderActivity` constructor/menu handler against small host UI and input
dependencies. The class declarations come from the real headers. Generation
replaces menu includes only and copies reader methods verbatim with source line
directives; there is no second implementation of the scrolling state machine.

Tests drive the public menu render/input path, advance a fake millisecond clock,
and inspect labels sent to a simulated fixed-width draw target. They cover idle
scheduling, suffix visibility, pauses/restart, UTF-8 and buffer limits, rollover,
selection changes, touch, Back and inactive menus.

Run through the usual host CMake build and
`ctest --test-dir build -R EndOfBookOptionsTest --output-on-failure`.

These tests do not measure actual fonts, panel refresh latency, ghosting, power
consumption or concurrent task execution. Physical validation remains necessary
on EPUB/TXT/XTC end screens, touch and button devices, and all four orientations.
