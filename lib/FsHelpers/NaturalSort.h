#pragma once

#include <cstddef>
#include <cstdint>

// Pure (Arduino-free) natural sort primitives, host-unit-testable.
// Included by FsHelpers.h; call as FsHelpers::naturalCompare(...).
namespace FsHelpers {

/**
 * Natural case-insensitive string compare: digit runs compare by numeric value
 * (leading zeros ignored), everything else byte-wise after tolower.
 * Returns <0, 0 or >0 like strcmp.
 */
int naturalCompare(const char* s1, const char* s2);

/**
 * Write an order-preserving byte encoding of `name` into `out` (up to `cap`
 * bytes, no terminator); returns the number of bytes written. Bytewise
 * comparison of two full keys matches naturalCompare on the original names;
 * truncated keys are a consistent coarsening (equal prefixes need a
 * naturalCompare fallback). Never emits 0x00, so fixed-size keys can be
 * zero-padded. Used to build fixed-size sort keys for on-SD merge sorting.
 */
size_t naturalSortKey(const char* name, uint8_t* out, size_t cap);

}  // namespace FsHelpers
