#pragma once

#include "../../../lib/Memory/Memory.h"

namespace AllocationTest {
inline size_t failBytes = 0;
inline unsigned failures = 0;

template <typename T, typename... Args>
  requires(!std::is_array_v<T>)
std::unique_ptr<T> allocate(Args&&... args) {
  return makeUniqueNoThrow<T>(std::forward<Args>(args)...);
}

template <typename T>
  requires std::is_unbounded_array_v<T>
std::unique_ptr<T> allocate(size_t count) {
  if (failBytes && count * sizeof(std::remove_extent_t<T>) == failBytes) {
    ++failures;
    return nullptr;
  }
  return makeUniqueNoThrow<T>(count);
}
}  // namespace AllocationTest

#define makeUniqueNoThrow AllocationTest::allocate
