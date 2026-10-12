---
name: heap-discipline
description: ESP32-C3 allocation discipline. Use when changing allocation size/failure handling, buffer or cache lifetime, or hot-path container growth.
---

# Heap Discipline

Use the selected board's [resource budget](../../rules/hardware-resources.md)
and the [checked-allocation contract](../../rules/coding-standards.md#heap-buffer-allocation).
Budget peak coexistence and largest required blocks, not just final free bytes.

## Allocation decision procedure

Choose the first option meeting size, ownership, and lifetime requirements:

1. Local bounded storage within the stack budget: use a plain array/struct.
2. Immutable data: use static const/constexpr storage; verify placement when
   material. Cache-disabled paths need accessible internal RAM.
3. Repeated activity work: allocate once, retain and reuse through its lifetime;
   cleanup follows [activity ownership](../../rules/ui-activities.md#activity-lifecycle-and-memory-management).
4. Other fallible storage: use checked `makeUniqueNoThrow` ownership.
5. C/SDK ownership transfer: raw allocation only under the checked-allocation
   contract, with the receiving owner and matching deallocator identified.

## Check the complete working set

Apply the [resource protocol](../../rules/hardware-resources.md#the-resource-protocol)
to container growth, strings, stack, and persistence.
Reuse existing buffers before adding storage. Full-screen work follows
[framebuffer ownership](../../rules/architecture-hal.md#framebuffer-ownership);
SD/vector arenas follow [font-cache ownership](../../rules/ui-activities.md#runtime-fonts-and-cache-ownership).
Preserve `HalMemory::allocatePsram()`'s no-internal-RAM-fallback contract.

For each new allocation, state worst-case bytes, peak coexistence, lifetime,
placement, and why stack/static/reuse cannot meet the requirement. Verify failure
propagation and cleanup, including retained parent activities and cache arenas.

**Done when:** every allocation has a bounded budget and owner, checked failure
handling, matching cleanup, and no avoidable repeated allocation in a hot path.
