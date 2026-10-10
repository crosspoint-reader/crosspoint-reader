#pragma once

#include <memory>
#include <type_traits>
#include <utility>

#include "HalStorage.h"

template <typename T, typename... Args>
  requires(!std::is_array_v<T>)
std::unique_ptr<T> makeUniqueNoThrow(Args&&... args) {
  fake::allocationSizes.push_back(sizeof(T));
  if (fake::failAllocationBytes == sizeof(T) || fake::fail(fake::failAlloc)) return nullptr;
  return std::make_unique<T>(std::forward<Args>(args)...);
}

template <typename T>
  requires std::is_unbounded_array_v<T>
std::unique_ptr<T> makeUniqueNoThrow(size_t count) {
  const size_t bytes = count * sizeof(std::remove_extent_t<T>);
  fake::allocationSizes.push_back(bytes);
  if (fake::failAllocationBytes == bytes || fake::fail(fake::failAlloc)) return nullptr;
  return std::make_unique<T>(count);
}
