#pragma once

#include <cstdint>
#include <string>

// Shared string-pool primitive. Stores many short strings contiguously in one
// std::string buffer; callers keep {offset, length} references instead of owning
// a std::string each — collapsing N small heap allocations into ~1 growing
// buffer (less fragmentation on the constrained ESP32-C3 heap).
//
// Each appended string is null-terminated, so `pool.data() + offset` is a valid
// C string for any C API (drawText, snprintf, ...). The pool itself may contain
// embedded nulls (one per entry); index entries by their stored offset.
namespace TextPool {

// Append a copy of s[0..len) plus a '\0' to `pool`; return the byte offset of the
// copy. Manual +256 linear growth avoids std::string's 2x doubling (which would
// over-reserve and re-fragment on a small heap).
inline uint16_t append(std::string& pool, const char* s, size_t len) {
  const uint16_t offset = static_cast<uint16_t>(pool.size());
  if (pool.size() + len + 1 > pool.capacity()) pool.reserve(pool.capacity() + 256);
  pool.append(s, len);
  pool.push_back('\0');
  return offset;
}

}  // namespace TextPool
