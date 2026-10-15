#pragma once

namespace ot {

// Stable insertion sort. Everything the shaper sorts is short (a syllable,
// a cluster's marks, a plan's features or one stage's lookups), and this
// costs a few dozen bytes per element type where std::stable_sort's merge
// machinery costs over a kilobyte.
template <typename T, typename Less>
void stableSort(T* first, T* last, Less less) {
  if (last - first < 2) return;
  for (T* i = first + 1; i != last; ++i) {
    const T value = *i;
    T* j = i;
    for (; j != first && less(value, *(j - 1)); --j) *j = *(j - 1);
    *j = value;
  }
}

}  // namespace ot
