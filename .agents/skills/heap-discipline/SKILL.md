---
name: heap-discipline
description: ESP32-C3 allocation discipline. Use when changing allocation size/failure handling, buffer or cache lifetime, or hot-path container growth.
---

# Heap Discipline (ESP32-C3)

The hardware and coding rule files state the allocation rules. This is the
procedure you run while writing the code and the gate before handoff.

The C3 baseline: ~380KB RAM, no PSRAM, one panel-sized framebuffer (48,000
bytes on X4; 52,272 on X3).
**Fragmentation, not total usage, is what kills this device.**
Free-heap can read fine while the largest free block is too small for the next
allocation. Optimize for not leaving holes, not just for using fewer bytes.

## Allocation decision procedure

Ask in order; stop at the first yes.

1. **Stack?** Local, bounded, under ~256 bytes total: plain array/struct. No
   heap, no fragmentation. Keep frames lean; the task stack is small.
2. **Compile-time constant?** Use `static constexpr` for constant evaluation;
   check the build map when its storage placement matters. Data used while the
   flash cache is disabled must remain in accessible internal RAM.
3. **Allocated once and reused for an activity's lifetime?** Allocate in
   `onEnter`, hold in a member, release in `onExit`. Never per-frame, never
   per-iteration.
4. **Dynamic and fallible?** `makeUniqueNoThrow<T>(...)` /
   `makeUniqueNoThrow<T[]>(n)` from `lib/Memory/Memory.h`. Null-check, `LOG_ERR`
   with the size, return false. It frees on every exit path.
5. **A C/SDK API takes ownership and frees it itself?** Only then raw
   `new (std::nothrow)` / `malloc`, with a comment naming who frees it.

Bare `new` / `new[]` is never correct here: under `-fno-exceptions` it calls
`abort()` on OOM instead of returning null.

## Fragmentation rules

- `std::vector`: `reserve(n)` before any `push_back` loop. Each growth is
  alloc-copy-free (three heap ops) and leaves a hole. Unknown n: estimate high.
- No repeated `new`/`delete` or growing containers inside a loop or render path.
  Hoist the allocation out of the loop.
- Large contiguous blocks fragment worst. Full-screen-class buffers use the
  chunked `storeBwBuffer` / `restoreBwBuffer` path in `GfxRenderer` so they
  avoid a contiguous panel-sized backup allocation. Reuse that path. Do not
  malloc a second full-screen buffer.
- During chapter builds, reuse `GfxRenderer::FrameBufferLoan` and the exclusive
  `buildscratch` claim/release protocol. No drawing is allowed during the loan;
  restoration requires a redraw. For PSRAM-only working sets, use
  `HalMemory::allocatePsram` and preserve its no-internal-RAM-fallback contract.
- Check internal heap, PSRAM, and largest blocks separately. Reusable SD-font
  arenas may remain resident after `clearCache`; use the established full-cache
  release path for heap-critical transitions rather than reallocating each page.
- `std::string` / Arduino `String`: acceptable on cold paths (file I/O, one-shot
  setup). Banned on hot/render paths. Build text with a stack `char[]` +
  `snprintf`; if a `std::string` is unavoidable, `reserve` it first.

## Justify every allocation

Per the root evidence rule: when you add a heap allocation, state in one line
why stack/static/reuse was rejected and the worst-case size. If you cannot name
the size, you cannot budget it, and you should not allocate it.

Reuse existing buffers before adding working storage. Simplifying ownership
must preserve allocation-failure handling, cleanup on every exit path, and the
required internal-RAM or PSRAM placement.

## Self-review before handoff

- [ ] No bare `new`/`new[]`. Every fallible alloc is `makeUniqueNoThrow`, or a
      raw alloc with an explicit owner comment.
- [ ] Every allocation is null-checked with `LOG_ERR` before the error return.
- [ ] No allocation inside a loop or render path that could be hoisted.
- [ ] Every `push_back` loop has a preceding `reserve`.
- [ ] Anything allocated in `onEnter` is released in `onExit`; member `HalFile`
      closed there too.
- [ ] No second full-screen buffer; grayscale uses store/restoreBwBuffer.
- [ ] Each new allocation carries a one-line size + why-not-stack/static note.
