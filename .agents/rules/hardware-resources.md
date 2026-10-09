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
4. String Policy: Prohibit std::string and Arduino String in hot paths. Use std::string_view for read-only access and snprintf with fixed char[] buffers for construction.
5. UI Strings: All user-facing text must use the `tr()` macro (e.g., `tr(STR_LOADING)`) for i18n support. Never hardcode UI strings directly. For the avoidance of doubt, logging messages (LOG_DBG/LOG_ERR) can be hardcoded, but user-facing text must use `tr()`.
6. `constexpr` First: Use `constexpr` for compile-time constants and lookup tables, and `static constexpr` for class-level constants. This enables constant evaluation; it does not guarantee flash placement or remove runtime storage in every use.
7. `std::vector` Pre-allocation: Always call `.reserve(N)` before any `push_back()` loop. Each growth event allocates a new block (2×), copies all elements, then frees the old one — three heap operations that fragment DRAM. When the final size is unknown, estimate conservatively.
8. SD Persistence Throttling: Settings, state, credentials, and other `PersistableStore` JSON files live on SD under `/.crosspoint/` through `HalStorage`; SPIFFS is not mounted. Guard redundant writes and debounce progress saves to avoid serialization, SD I/O, and `storageMutex` cost.
9. `new` is not nothrow on ESP32: With `-fno-exceptions`, bare `new` that fails calls `abort()` — it does NOT return `nullptr`. Always use `new (std::nothrow)` and null-check the result, or use `makeUniqueNoThrow<T>()` from `lib/Memory/Memory.h`. Never write bare `new` for any fallible allocation.

---
