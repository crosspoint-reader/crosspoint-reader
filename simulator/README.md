# CrossVi desktop simulator

This directory contains the host hardware-adaptation layer used by the
`simulator_x3` and `simulator_x4` PlatformIO environments. The application,
activities, renderer, fonts, icons, themes, image decoders, and input mapping
are compiled directly from CrossVi; only device hardware is replaced by SDL2
and host filesystem adapters.

The implementation is derived from
[`crosspoint-reader/crosspoint-simulator`](https://github.com/crosspoint-reader/crosspoint-simulator)
at commit `3c2fad0e730e37b32f571715d47851ae384584b8`, created by Julia Nguyen.
Its MIT license and copyright notice are retained in [LICENSE](LICENSE).

CrossVi-specific changes include separate X3/X4 targets, current HAL API
compatibility, a pixel-exact nearest-neighbour display path, clickable raw
hardware buttons, tear-free presentation, CrossVi virtual-SD variables,
framebuffer screenshots, scripted button input, and deterministic smoke tests.

See [the simulator guide](../docs/contributing/simulator.md) for setup and use.
