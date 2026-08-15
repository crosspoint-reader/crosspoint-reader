#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace TableLayout {

constexpr size_t MAX_COLUMNS = 8;

struct ColumnMetrics {
  uint16_t minimum = 0;
  uint16_t preferred = 0;
};

bool allocateColumnWidths(const std::array<ColumnMetrics, MAX_COLUMNS>& metrics, size_t columnCount,
                          uint16_t availableWidth, uint16_t columnGap,
                          std::array<uint16_t, MAX_COLUMNS>& widths);

void applySpanRequirement(std::array<ColumnMetrics, MAX_COLUMNS>& metrics, size_t columnCount, size_t firstColumn,
                          size_t span, uint16_t minimum, uint16_t preferred, uint16_t columnGap);

}  // namespace TableLayout
