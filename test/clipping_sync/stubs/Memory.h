#pragma once
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

#include "HalMemory.h"
#include "HalStorage.h"

template <typename T, typename... Args>
  requires(!std::is_array_v<T>)
std::unique_ptr<T> makeUniqueNoThrow(Args&&... args) {
  if (fake::fail(fake::failAlloc)) return nullptr;
  return std::unique_ptr<T>(new (std::nothrow) T(std::forward<Args>(args)...));
}
template <typename T>
  requires std::is_unbounded_array_v<T>
std::unique_ptr<T> makeUniqueNoThrow(size_t count) {
  if (fake::fail(fake::failAlloc)) return nullptr;
  if (count == 32768) fake::syncBufferAllocated = true;
  return std::unique_ptr<T>(new (std::nothrow) std::remove_extent_t<T>[count]);
}
