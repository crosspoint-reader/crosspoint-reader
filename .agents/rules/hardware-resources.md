## Platform and Hardware Constraints

### Hardware Specs

* MCUs: ESP32-C3 (X3/X4, single-core RISC-V @ 160MHz), ESP32-S3
  (including Sticky, X4 Pro, X4 Classic, Paper Mono, and Metalio E-Ink 4,
  dual-core Xtensa LX7), and classic ESP32 on SDK boards such as M5Paper v1.1
* RAM: ~380KB usable on ESP32-C3 (VERY LIMITED - primary project constraint)
  * **NO PSRAM on C3**.
  * **Single Buffer Mode**: One framebuffer sized for the runtime panel
* X4 baseline flash: 16MB (Instruction storage and static data)
* X4 baseline display: 800x480 E-Ink (Slow refresh, monochrome, 1-2s full update)
  * Framebuffer: 48,000 bytes (800 × 480 ÷ 8)
* X3 display: 792x528; framebuffer: 52,272 bytes (792 × 528 ÷ 8)
* Storage: SD Card (Used for books and aggressive caching)

Check `platformio.ini`, the selected SDK `BoardConfig`, and `lib/hal/` for the
actual display size, touch/buttons, frontlight, grayscale, storage bus, USB-MSC,
and PSRAM capabilities. S3 does not by itself guarantee usable PSRAM or touch;
X4 Classic is a button-only board. Keep C3 operation viable when adding a
capability available only on richer boards.

CrossPoint's checked-in build and release targets are `default` (X3/X4),
`sticky`, `x4pro`, `x4c`, `papermono`, and `metalio_eink4`. Inspect the current
workflow matrices for coverage. Additional SDK profiles are not automatically
CrossPoint release targets or evidence of device verification.

Budget internal RAM and PSRAM separately with `HalMemory::getInternalHeap()`
and `getPsramHeap()`, including `largestBlockBytes`; a combined free-heap
reading can hide internal-heap pressure. `HalMemory::allocatePsram()` returns
null rather than falling back to internal RAM. Preserve that failure contract.

### The Resource Protocol

1. Stack Safety: Limit local function variables to < 256 bytes. The ESP32-C3 default stack is small; use std::unique_ptr or static pools for larger buffers.
2. Heap Fragmentation: Avoid repeated new/delete in loops. Allocate buffers once during onEnter() and reuse them.
3. Flash Persistence: Give large immutable tables static storage and const qualification so the linker can place them in flash. Check the build map for material memory claims; data needed while the flash cache is disabled must be in accessible internal RAM.
4. String Policy: std::string and Arduino String are acceptable in cold file-I/O/setup paths. Hot/render paths use std::string_view for read-only access and snprintf with fixed char[] buffers; an unavoidable std::string must reserve storage first.
5. User-facing strings follow the [translation workflow](generated-files-cache.md#translation-workflow).
6. `constexpr` First: Use `constexpr` for compile-time constants and lookup tables, and `static constexpr` for class-level constants. This enables constant evaluation; it does not guarantee flash placement or remove runtime storage in every use.
7. `std::vector` Pre-allocation: Always call `.reserve(N)` before any `push_back()` loop. Each growth event allocates a new block (2×), copies all elements, then frees the old one — three heap operations that fragment DRAM. When the final size is unknown, estimate conservatively.
8. SD Persistence Throttling: Settings, state, credentials, and other `PersistableStore` JSON files live on SD under `/.crosspoint/` through `HalStorage`; SPIFFS is not mounted. Guard redundant writes and debounce progress saves to avoid serialization, SD I/O, and `storageMutex` cost.
9. Fallible storage follows the [checked-allocation contract](coding-standards.md#heap-buffer-allocation); budget peak coexistence during container growth, not just final size.

### Flash size: unused features must cost nothing

Flash is limited, and every release target pays for shared code. A new optional
feature must not grow builds, screens or call sites that do not use it. Before
adding one to shared code, check each of these, and prefer a design that avoids
the cost over one that needs measuring:

* **Templates** (FreeInkUI's `list()` and `Frame<N>`, and anything else
  instantiated per caller): code added inside a template is compiled into every
  instantiation. Put anything that does not depend on the template parameter in
  a non-template function and call it. A 10-line branch added to `list()` cost
  about 250 bytes per instantiation and grew every target by 3.2-3.6 KB.
* **Per-item and per-call structs** (`ListItem`, `ListProps`, `SettingInfo` and
  similar): every field is initialized at every construction site and enlarges
  every array, vector and stack frame that holds one. Do not add a by-value
  sub-struct for an optional feature. Use a pointer that defaults to `nullptr`,
  or reuse an existing field.
* **Header-only `inline` functions** with more than a few lines that many
  callers use: mark them `__attribute__((noinline))` or move the body to a
  `.cpp` file so there is one copy.
* **Device-specific code**: `BoardConfig::ACTIVE` is selected at runtime, so a
  `BoardConfig::isX4Pro()`-style check does not remove code from other
  targets. Put anything sizeable behind the device's or capability's build
  flag (`FREEINK_DEVICE_*`, `FREEINK_CAP_*`); a runtime check is fine for a
  line or two.
* **SDK bumps**: a submodule bump that grows every target has pulled in a
  change of the kinds above. Find it and fix it in the SDK instead of
  accepting the growth here.

CI posts a per-target size comparison on each pull request. If a feature still
adds code for targets or callers that do not use it, state the number in the PR
description with the reason; "the feature needs it" is not one when a pointer,
a shared function or a build flag would avoid it. To measure before pushing,
compile one translation unit that uses the component with the target compiler
at `-Os`, before and after the change, and compare with `size` and
`nm -S --size-sort`.

---
