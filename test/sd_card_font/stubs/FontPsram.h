#pragma once

// Host-test stub for FreeInkFont's PSRAM-preferring helpers (FontPsram.h):
// plain heap on the host, same contracts (psramNewArray is nothrow, nullptr
// on OOM). Allocation goes through nothrow operator new[] so the test's
// fail-next-allocation override (see SdCardFontTest.cpp) can inject OOM.
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
