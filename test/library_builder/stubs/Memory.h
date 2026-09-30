#pragma once

#include <memory>
#include <type_traits>
#include <utility>

#include "HalStorage.h"

template <typename T, typename... Args>
  requires(!std::is_array_v<T>)
std::unique_ptr<T> makeUniqueNoThrow(Args&&... args) {
  if (fake::fail(fake::failAlloc)) return nullptr;
  return std::make_unique<T>(std::forward<Args>(args)...);
}

template <typename T>
  requires std::is_unbounded_array_v<T>
std::unique_ptr<T> makeUniqueNoThrow(size_t count) {
  if (fake::fail(fake::failAlloc)) return nullptr;
  return std::make_unique<T>(count);
}
