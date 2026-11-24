#pragma once

#include <new>
#include <vector>

namespace freeink {
namespace font {

template <typename T>
using PsramVector = std::vector<T>;

template <typename T>
T* psramNewArray(std::size_t n) {
  return new (std::nothrow) T[n ? n : 1];
}

template <typename T>
void psramDeleteArray(T* p) {
  delete[] p;
}

}  // namespace font
}  // namespace freeink
