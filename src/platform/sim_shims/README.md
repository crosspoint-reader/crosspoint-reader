Host stand-ins for ESP-only headers, on the simulator's include path only.

They exist so `src/main.cpp` compiles unmodified for the host. Every `#ifdef
SIMULATOR` we would otherwise add to a file we do not own is a hunk that has to
be re-merged on every upstream pull; a shim here costs nothing.
