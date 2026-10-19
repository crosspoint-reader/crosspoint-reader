# X3 software USB DFU

Software GPIO18/19 full-speed transport from the local Escape Hatch updater.
The vendored TinyUSB subset (common/device core, DFU class and OS-none headers) is unmodified upstream commit 840ffe3ac152394347d27a0d4841f4c3c49ac944 (MIT license alongside source).
The software transport is C3-only; the DFU session is also shared with X4 Pro. No eFuse writes.
Protocol X3DFU001, 4096-byte blocks, device version 0x0700. Compression is deliberately disabled.
