#pragma once

// Host-test stub for FreeInkFont's PSRAM-preferring helpers (FontPsram.h):
// plain heap on the host, same contracts (psramNewArray is nothrow, nullptr
// on OOM). Allocation goes through nothrow operator new[] so the test's
// fail-next-allocation override (see SdCardFontTest.cpp) can inject OOM.
#include <cstdlib>
#include <new>
#include <vector>

// Fails the font allocation after this many more succeed; -1 never fails.
inline int sdFontPsramFailAfter = -1;
// How many consecutive allocations fail once sdFontPsramFailAfter reaches zero.
inline int sdFontPsramFailCount = 1;
inline size_t sdFontPsramAllocations = 0;
inline size_t sdFontPsramFrees = 0;

inline bool sdFontPsramFails() {
  if (sdFontPsramFailAfter < 0) return false;
  if (sdFontPsramFailAfter > 0) {
    --sdFontPsramFailAfter;
    return false;
  }
  if (--sdFontPsramFailCount > 0) return true;
  sdFontPsramFailAfter = -1;
  sdFontPsramFailCount = 1;
  return true;
}

namespace freeink {
namespace font {

template <typename T>
using PsramVector = std::vector<T>;

template <typename T>
T* psramNewArray(std::size_t n) {
  if (sdFontPsramFails()) return nullptr;
  T* result = new (std::nothrow) T[n ? n : 1];
  if (result) ++sdFontPsramAllocations;
  return result;
}

template <typename T>
void psramDeleteArray(T* p) {
  if (p) ++sdFontPsramFrees;
  delete[] p;
}

}  // namespace font
}  // namespace freeink
