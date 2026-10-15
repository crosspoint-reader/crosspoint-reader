#pragma once

#include <cstdint>

// Read-only copies of .cpfont layout fonts in internal flash, memory-mapped
// for the shaper so a complex-script face costs no heap for its tables.
//
// No-PSRAM boards only: there an Indic layout font (5-80 KB) would otherwise
// sit in the same internal DRAM the reader lives in. The "spiffs" data
// partition, which nothing else uses, is split into as many MAX_BLOB_BYTES
// slots as it holds (up to 8; the 1 MB partition of a stock Xteink layout
// holds 7) behind a one-sector directory. A blob is written once, keyed by its content hash, and
// re-verified against that hash every time it is mapped; the oldest-written
// idle slot is recycled first. Writes happen only when a font is used for the first
// time (or after its slot was recycled), so flash wear is negligible.
//
// Everything here is best-effort: acquire() returns nullptr whenever the
// partition is missing or too small, the blob is too large, or flash access
// fails, and the caller then decides whether the blob may stay in RAM.
namespace FlashBlobCache {

// Largest blob a slot holds: Noto Devanagari's layout font is ~77 KB.
constexpr uint32_t MAX_BLOB_BYTES = 128 * 1024;

// Fills buf with `length` bytes of the blob starting at `offset`.
using Reader = bool (*)(void* ctx, uint32_t offset, uint8_t* buf, uint32_t length);

// Maps the blob whose content hashes to `key` (FNV-1a | 1), copying it in
// through `read` first when flash does not already hold it.
const uint8_t* acquire(uint32_t key, uint32_t length, Reader read, void* ctx);

// Unmaps a pointer returned by acquire(). Signature matches
// ComplexShaper::Blob::release.
void release(void* data);

// Whether acquire() could ever map this blob: the partition is usable, the
// blob fits a slot and its copy has not failed. When true, a failed
// acquire() may succeed later (a copy buffer, a mapped slot freeing up).
bool canHold(uint32_t key, uint32_t length);

}  // namespace FlashBlobCache
