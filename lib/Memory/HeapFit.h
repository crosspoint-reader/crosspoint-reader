#pragma once

#include <esp_heap_caps.h>

#include <cstddef>

// Whether the heap holds one free block of at least `bytes`.
//
// For the growth of std containers (vector::resize/reserve, string::reserve), which cannot fail
// gracefully on the device: it is built with -fno-exceptions, so a failed operator new aborts and
// reboots it. Where the size comes from data -- a count read from a cache file -- and the caller
// has something sensible to do without the allocation, ask first. Found the hard way on the X3
// (2026-10-01): a 4 KB string reserve on a heap with 14.7 KB free but no block over 4.3 KB.
//
// The answer can go stale if another task allocates in between; the margin covers the allocator's
// own block header and a little of that, not a concurrent burst.
inline bool heapHasBlockFor(size_t bytes) {
  constexpr size_t ALLOCATOR_SLACK = 64;
  return heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT) >= bytes + ALLOCATOR_SLACK;
}

// Makes room in the std::vector or std::string `c` for `extra` more elements, so the appends that
// follow cannot reallocate. Grows it as it would grow itself, to at least twice its capacity, but
// only when the heap holds a block for that: the container would ask through the throwing
// operator new. False: nothing changed, and the caller does without the elements.
//
// For tables that grow with the document and whose entries the caller can drop (a chapter's
// printed-page labels, its resident anchors). A table that must not lose an entry belongs in a
// ChunkedVector instead.
template <typename C>
bool heapReserveFor(C& c, const size_t extra) {
  const size_t needed = c.size() + extra;
  if (needed <= c.capacity()) return true;
  const size_t grown = needed > 2 * c.capacity() ? needed : 2 * c.capacity();
  // One element more than `grown`: a string's terminator.
  if (!heapHasBlockFor((grown + 1) * sizeof(typename C::value_type))) return false;
  c.reserve(grown);
  return true;
}
