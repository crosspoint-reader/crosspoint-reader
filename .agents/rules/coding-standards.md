## Coding Standards

### Naming Conventions

* Classes: PascalCase (e.g., EpubReaderActivity)
* Methods/Variables: camelCase (e.g., renderPage())
* Constants: UPPER_SNAKE_CASE (e.g., MAX_BUFFER_SIZE)
* Private Members: memberVariable (no prefix)
* File Names: Match Class names (e.g., EpubReaderActivity.cpp)

### Header Guards

* Use #pragma once for all header files.

### Comment Style

* Write code first. Add comments only for non-obvious constraints, mechanisms, ownership, field/parameter meaning, or necessary special cases.
* Use plain English and 1-2 short lines; use more only when a necessary contract cannot fit. Keep technical facts; cut filler and code restatements.
* Describe the current code for a future maintainer with no PR or chat context. Keep comparisons with earlier drafts, investigation logs, and change history in review notes or commit messages.
* Before handoff, read every added or changed comment without the diff. Delete or rewrite anything that will not help after merge.

### Memory Safety and RAII

* Smart Pointers: Prefer std::unique_ptr.
* RAII: Use destructors for cleanup. Call `vTaskDelete()` explicitly for deterministic task release. Do NOT call `file.close()` on local `FsFile` variables — `DESTRUCTOR_CLOSES_FILE=1` handles it at scope exit (see Critical Build Flags).

### Reuse existing interfaces

Search callers and existing helpers before adding an interface or callback
type. Reuse established APIs such as `ButtonNavigator` and `UiAppHost` when
they meet the requirement; justify a new abstraction by the contract it adds.

### ESP32-C3 Platform Pitfalls

#### `std::string_view` and Null Termination

`string_view` is *not* null-terminated. Passing `.data()` to any C-style API (`drawText`, `snprintf`, `strcmp`, SdFat file paths) is undefined behaviour when the view is a substring or a view of a non-null-terminated buffer.

**Rule**: `string_view` is safe only when passing to C++ APIs that accept `string_view`. For any C API boundary, convert explicitly:

```cpp
// WRONG - undefined behaviour if view is a substring:
renderer.drawText(font, x, y, myView.data(), true);

// CORRECT - guaranteed null-terminated:
renderer.drawText(font, x, y, std::string(myView).c_str(), true);

// CORRECT - for short strings, use a stack buffer:
char buf[64];
snprintf(buf, sizeof(buf), "%.*s", (int)myView.size(), myView.data());
```

#### `IRAM_ATTR` and Flash Cache Safety

Most application code runs from flash through the instruction cache. During
internal-flash operations such as OTA writes or NVS updates, that cache can be
disabled. A handler that must run during this window needs an entirely
IRAM-safe call path, not just an attribute on its entry point.

```cpp
// Handler registered with ESP_INTR_FLAG_IRAM:
void IRAM_ATTR gpioISR() { ... }

// Data accessed from IRAM_ATTR code: must be in DRAM, never a flash const
static DRAM_ATTR uint32_t isrEventFlags = 0;
```

**Rules**:

- For `ESP_INTR_FLAG_IRAM` handlers, verify that the handler and every callee
  reside in IRAM or ROM, and that all accessed data is in internal RAM.
- Use `DRAM_ATTR` or appropriate linker placement for constants and strings
  accessed by that path; flash and PSRAM may be unavailable with the cache off.
- Handlers not registered as IRAM-safe are deferred while the cache is disabled;
  choose the registration contract according to the required latency.
- Normal task code does **not** need `IRAM_ATTR` merely because it is firmware.

#### ISR vs Task Shared State

`xSemaphoreTake()` (mutex) **cannot** be called from ISR context — it will crash. Use the correct primitive for each communication direction:

| Direction                       | Correct primitive                                  |
| ------------------------------- | -------------------------------------------------- |
| ISR → task (data)               | `xQueueSendFromISR()` + `portYIELD_FROM_ISR()`     |
| ISR → task (signal)             | `xSemaphoreGiveFromISR()` + `portYIELD_FROM_ISR()` |
| Task → task                     | `xSemaphoreTake()` / mutex                         |
| Simple flag (single writer ISR) | `volatile bool` + `portENTER_CRITICAL_ISR()`       |

#### RISC-V Alignment

ESP32-C3 faults on unaligned multi-byte loads. Never cast a `uint8_t*` buffer to a wider pointer type and dereference it directly. Use `memcpy` for any unaligned read:

```cpp
// WRONG — faults if buf is not 4-byte aligned:
uint32_t val = *reinterpret_cast<const uint32_t*>(buf);

// CORRECT:
uint32_t val;
memcpy(&val, buf, sizeof(val));
```

This applies to all cache deserialization code and any raw buffer-to-struct casting. `__attribute__((packed))` structs have the same hazard when accessed via member reference.

#### Template and `std::function` Bloat

Template instantiations can increase binary size. `std::function` code size and
allocation depend on the implementation, callable, and compiler/linker settings;
some callables use inline storage without heap allocation. Avoid both in library
code and any path called from the render loop:

```cpp
// Avoid — may allocate and increase binary size:
std::function<void()> callback;

// Prefer — no wrapper allocation:
void (*callback)() = nullptr;

// For member function + context (common activity callback pattern):
struct Callback { void* ctx; void (*fn)(void*); };
```

When a template is necessary, limit instantiations: use explicit template instantiation in a `.cpp` file to prevent the compiler from generating duplicates across translation units.

---

### Error Handling Philosophy

**Source**: [src/main.cpp:132-143](../../src/main.cpp), [lib/GfxRenderer/GfxRenderer.cpp:10](../../lib/GfxRenderer/GfxRenderer.cpp)

**Pattern Hierarchy**:

1. **LOG_ERR + return false** (90%): `LOG_ERR("MOD", "Failed: %s", reason); return false;`
2. **LOG_ERR + fallback**: `LOG_ERR("MOD", "Unavailable"); useDefault();`
3. **assert(false)**: Only for fatal "impossible" states (framebuffer missing)
4. **ESP.restart()**: Only for recovery (OTA complete)

**Rules**: NO exceptions, NO abort(), ALWAYS log before error return

### Heap Buffer Allocation

Prefer `makeUniqueNoThrow<T>(args)` for objects and `makeUniqueNoThrow<T[]>(size)`
for buffers, from `lib/Memory/Memory.h` (`<Memory.h>`). It wraps `new (std::nothrow)`,
returns null on OOM, and uses `std::unique_ptr` to free storage on every exit path.
`malloc` also returns null on OOM. Bare `new` is forbidden: with `-fno-exceptions`, OOM calls `abort()`, not null.

Null-check every allocation; `LOG_ERR` before returning false on OOM.
Pass borrowed pointers through `.get()`; ownership stays with the `unique_ptr`.
Use raw `malloc` or `new (std::nothrow)` only when a C API takes ownership and
frees storage itself; comment why and identify the owner. Raw allocations
otherwise require manual cleanup on every return path and risk leaks.

```cpp
#include <Memory.h>
auto obj = makeUniqueNoThrow<MyClass>(args);
if (!obj) { LOG_ERR("MOD", "OOM: MyClass"); return false; }
auto buf = makeUniqueNoThrow<uint8_t[]>(size);
if (!buf) { LOG_ERR("MOD", "OOM: %d bytes", size); return false; }
someApi(buf.get(), size);  // borrowed; unique_ptr retains ownership
```

For ownership transfer, allocate with `malloc`/`new (std::nothrow)`, apply the
same OOM check, then transfer to the API that calls `free`/`delete[]`/`delete`
as appropriate.

Examples: [Memory.h](../../lib/Memory/Memory.h),
[HomeActivity.cpp](../../src/activities/home/HomeActivity.cpp), and
[GfxRenderer.cpp](../../lib/GfxRenderer/GfxRenderer.cpp).

---
