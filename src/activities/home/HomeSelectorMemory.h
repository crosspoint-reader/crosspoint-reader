#pragma once

#include <algorithm>

// Stores and restores the last selected home item while the app is running.
class HomeSelectorMemory {
 public:
  int getStoredIndex() const { return std::max(0, lastSelectorIndex); }

  int restore(int menuItemCount) const {
    if (menuItemCount <= 0) {
      return 0;
    }

    return std::clamp(lastSelectorIndex, 0, menuItemCount - 1);
  }

  void store(int selectorIndex, int menuItemCount) {
    if (menuItemCount <= 0) {
      lastSelectorIndex = 0;
      return;
    }

    lastSelectorIndex = std::clamp(selectorIndex, 0, menuItemCount - 1);
  }

 private:
  int lastSelectorIndex = 0;
};
