## Coding Standards

### Naming Conventions

Classes/files use PascalCase, methods/variables camelCase, constants
UPPER_SNAKE_CASE. Private members have no prefix. Match surrounding code.
File names match their class names.

### Header Guards

Use `#pragma once`.

### Comment Style

Write code first. Comment only non-obvious constraints, mechanisms, ownership,
field meaning, or necessary exceptions. Use plain English and usually 1-2 lines;
longer text must express a necessary contract. Describe current code for a future
maintainer, not draft comparisons or investigation history. Read changed comments
without the diff before handoff; remove narration and restatements.

### Memory Safety and RAII

Prefer `std::unique_ptr` and destructor cleanup. File lifetime follows the
[HAL contract](architecture-hal.md#file-handle-lifetime); task/activity cleanup
follows [lifecycle ownership](ui-activities.md#activity-lifecycle-and-memory-management).

### Reuse existing interfaces

Trace callers and existing helpers before adding interfaces or callbacks.
Reuse `ButtonNavigator` / `UiAppHost` when appropriate; a new abstraction must
own a real contract or hide a demonstrated implementation choice.

### ESP32-C3 Platform Pitfalls

#### `std::string_view` and Null Termination

A view need not be null-terminated. Pass it directly only to APIs accepting
`string_view`; C-string APIs need an explicit terminated string or bounded
`snprintf` buffer. A substring's `.data()` is not a safe C string.

#### `IRAM_ATTR` and Flash Cache Safety

OTA/NVS flash operations can disable the instruction cache. For
`ESP_INTR_FLAG_IRAM`, verify the entire call path is IRAM/ROM and all data is
internal RAM (`DRAM_ATTR`/appropriate placement); flash and PSRAM may be unavailable.
An entry-point attribute alone is insufficient. Non-IRAM-safe handlers are
deferred; choose registration from required latency. Ordinary task code does
not need `IRAM_ATTR` merely because it is firmware.

#### ISR vs Task Shared State

No mutex `xSemaphoreTake()` in an ISR.
Use `xQueueSendFromISR()` for ISR data or `xSemaphoreGiveFromISR()` for signals,
with `portYIELD_FROM_ISR()`; task-to-task state uses mutexes.
A single-writer ISR flag follows the existing `volatile`/critical-section protocol,
using `portENTER_CRITICAL_ISR()` on the ISR side.

#### RISC-V Alignment

Use `memcpy` for wider reads from potentially unaligned byte buffers instead
of dereferencing cast pointers. This applies to cache deserialization and
references to packed-struct members.

#### Template and `std::function` Bloat

Avoid templates and `std::function` in library code or paths called from the
render loop; use existing callback APIs, function pointers, or a context/function
pair.
Their footprint depends on instantiations, callables, and toolchain settings;
some `std::function` callables have inline storage. When a template is necessary,
limit instantiations through explicit instantiation in a `.cpp` file.

### Error Handling Philosophy

Log with `LOG_ERR` before an error return or fallback. No exceptions or explicit
`abort()`. Assertions are reserved for fatal impossible states; restart is for
explicit recovery such as OTA completion.

### Heap Buffer Allocation

Use `makeUniqueNoThrow<T>()` / `makeUniqueNoThrow<T[]>(n)` from
[Memory.h](../../lib/Memory/Memory.h), then null-check and log the requested byte
count with `LOG_ERR` before returning failure. Bare `new`/`new[]` is forbidden:
OOM can abort instead of returning null with exceptions disabled.
Borrow through `.get()`; keep ownership with the `unique_ptr`.

Raw `malloc`/`new (std::nothrow)` is allowed only for a C/SDK API taking ownership.
Check failure, name that owner in a comment, and match its `free`/`delete`/`delete[]`
contract. Resource budgets and hot-path containers follow
[hardware rules](hardware-resources.md#the-resource-protocol).
