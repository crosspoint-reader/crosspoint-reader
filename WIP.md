# Work in progress

This branch contains an **unfinished** refactor toward **bounded, heap-free PDF handling** (NASA-style fixed upper limits) and related **debug tooling** for crashes on ESP32-C3.

## Goals

1. **PDF path — no dynamic allocation**  
   Replace `malloc`/`free`, unbounded `std::string`/`std::vector`, and `std::unordered_map` in `lib/Pdf` with fixed-capacity types (`PdfFixedString`, `PdfFixedVector`, `PdfByteBuffer`) and explicit limits in `PdfLimits.h`.

2. **Crash diagnostics**  
   - Linker `--wrap=abort` with `__wrap_abort` in `lib/hal/HalSystem.cpp` calling `esp_backtrace_print` before libc `abort()` (e.g. `std::terminate` paths).  
   - `sdkconfig.defaults`: `CONFIG_ESP_SYSTEM_USE_EH_FRAME=y` for RISC-V backtraces.  
   - `scripts/addr2line_crash.sh` to decode PCs from serial into file:line.  
   - `[env:debug]` in `platformio.ini` with `-g3` / `CROSSPOINT_DEBUG_BUILD=1`.

## Done (this WIP)

- **`lib/Pdf/Pdf/PdfFixed.h`** — fixed-capacity string/vector and `PdfByteBuffer` for stream payloads.  
- **`lib/Pdf/Pdf/PdfLimits.h`** — caps for objects, pages, outline entries, object bodies, xref sections, inline ObjStm cache, text blocks, etc.  
- **`PdfObject`** — reads object bodies into `PdfFixedString<PDF_OBJECT_BODY_MAX>`; `getDictValue` / `getDictInt` / `getDictRef` use `std::string_view` (+ overload for legacy `const std::string&`).  
- **`XrefTable`** — `offsets_[]` fixed array; classic xref merge uses depth-indexed static update slots; xref stream / ObjStm decode use shared `g_pdfLargeWork[PDF_LARGE_WORK_BYTES]`; inline object pool instead of `unordered_map`; no `malloc` in this file.  
- **`PageTree`**, **`PdfCache`**, **`PdfOutline`**, **`PdfPage`** — migrated to fixed types; `PdfCache::configure()` + fixed paths/meta/page blobs.  
- **`Pdf` / `Pdf.cpp`** — value-type document state (no `unique_ptr<Impl>`); `bool open(const char*)`, `bool getPage(uint32_t, PdfPage&)`; `outline()` returns fixed vector.  
- **Reader** — `PdfReaderActivity` updated toward `getPage` + fixed image buffer path (see repo diff).  
- **HAL / build** — `HalSystem` abort wrapper; `platformio.ini` sdkconfig defaults + debug env; `sdkconfig.defaults` added.

## Not finished / blocked

- **`lib/Pdf/Pdf/ContentStream.cpp` (+ `ContentStream.h`)** — **not fully migrated** in this snapshot. It still uses `std::string`, `malloc` for the content buffer in `parse`/`parseBuffer`, `std::vector`/`std::unordered_map` for operator stack and font CID maps, and APIs that assumed the old `PdfObject`/`std::string` surface. **The firmware build is expected to fail** until this file (and any remaining call sites) are updated to `std::string_view` / `PdfFixedString` and the static decode buffer pattern used elsewhere.

- **Activities / tests** — `ReaderActivity`, `PdfReaderChapterSelectionActivity`, and `test/pdf/pdf_parser_host_test.cpp` may still need API updates (`Pdf::open`, `getPage`, outline types) to match the new `Pdf` API.

- **Other touched files** — `FontCacheManager`, `HalPowerManager`, `ClearCacheActivity`, `Serialization.h` may include partial or unrelated edits; verify before release.

## How to continue

1. Finish **ContentStream**: static `uint8_t` buffer sized `PDF_CONTENT_STREAM_MAX`, replace `TmpRun`/stack/CID map structures with fixed arrays or capped vectors; thread `std::string_view pageObjectBody` through parsers.  
2. **Build** with `pio run -e default` and fix remaining compile errors file by file.  
3. Run **PDF host tests** (`test/pdf/`) after API stabilizes.  
4. Remove or narrow **`PdfObject::getDictValue(..., const std::string&)`** once all callers use `string_view` / fixed strings only.

## Quick verify commands

```bash
pio run -e default
# When ContentStream is fixed:
pio run -e debug
```

Last updated: WIP commit (see git log).
