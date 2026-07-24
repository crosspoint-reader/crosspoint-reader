#pragma once

#include <algorithm>

namespace HomeMenuLayout {

struct Fit {
  int rows = 0;
  int rowHeight = 0;
  int rowGap = 0;

  constexpr int yOffset(const int itemIndex, const int columns) const {
    return columns > 0 ? (itemIndex / columns) * (rowHeight + rowGap) : 0;
  }

  constexpr int totalHeight() const {
    return rows > 0 ? rows * rowHeight + (rows - 1) * rowGap : 0;
  }
};

// Preserve each theme's preferred rhythm when it fits. If the menu grows,
// tighten gaps and row height just enough to keep every item in bounds.
constexpr Fit fit(const int availableHeight, const int itemCount, const int columns, const int preferredRowHeight,
                  const int preferredGap, const int minimumRowHeight) {
  if (availableHeight <= 0 || itemCount <= 0 || columns <= 0) return {};

  const int rows = (itemCount + columns - 1) / columns;
  const int safePreferredHeight = std::max(1, preferredRowHeight);
  const int safePreferredGap = std::max(0, preferredGap);
  const int safeMinimumHeight = std::max(1, std::min(minimumRowHeight, safePreferredHeight));
  const int preferredTotal = rows * safePreferredHeight + (rows - 1) * safePreferredGap;
  if (preferredTotal <= availableHeight) return {rows, safePreferredHeight, safePreferredGap};

  const int gap = rows > 1
                      ? std::min(safePreferredGap,
                                 std::max(0, (availableHeight - rows * safeMinimumHeight) / (rows - 1)))
                      : 0;
  const int rowHeight = std::max(1, std::min(safePreferredHeight, (availableHeight - (rows - 1) * gap) / rows));
  return {rows, rowHeight, gap};
}

}  // namespace HomeMenuLayout
