#include "TableLayout.h"

#include <algorithm>
#include <cstdint>

namespace TableLayout {
namespace {

uint32_t sumWidths(const std::array<uint16_t, MAX_COLUMNS>& widths, const size_t first, const size_t count) {
  uint32_t result = 0;
  for (size_t i = first; i < first + count; ++i) {
    result += widths[i];
  }
  return result;
}

}  // namespace

void applySpanRequirement(std::array<ColumnMetrics, MAX_COLUMNS>& metrics, const size_t columnCount,
                          const size_t firstColumn, const size_t span, const uint16_t minimum,
                          const uint16_t preferred, const uint16_t columnGap) {
  if (span == 0 || firstColumn >= columnCount || span > columnCount - firstColumn) return;

  const uint32_t internalGaps = static_cast<uint32_t>(span - 1) * columnGap;
  uint32_t currentMinimum = internalGaps;
  uint32_t currentPreferred = internalGaps;
  for (size_t i = firstColumn; i < firstColumn + span; ++i) {
    currentMinimum += metrics[i].minimum;
    currentPreferred += std::max(metrics[i].minimum, metrics[i].preferred);
  }

  uint32_t minimumDeficit = minimum > currentMinimum ? minimum - currentMinimum : 0;
  uint32_t preferredDeficit = preferred > currentPreferred ? preferred - currentPreferred : 0;
  for (size_t i = firstColumn; i < firstColumn + span; ++i) {
    const size_t columnsLeft = firstColumn + span - i;
    const uint16_t minimumAddition = static_cast<uint16_t>((minimumDeficit + columnsLeft - 1) / columnsLeft);
    metrics[i].minimum = static_cast<uint16_t>(metrics[i].minimum + minimumAddition);
    minimumDeficit -= std::min<uint32_t>(minimumDeficit, minimumAddition);

    const uint16_t preferredAddition = static_cast<uint16_t>((preferredDeficit + columnsLeft - 1) / columnsLeft);
    metrics[i].preferred = static_cast<uint16_t>(metrics[i].preferred + preferredAddition);
    preferredDeficit -= std::min<uint32_t>(preferredDeficit, preferredAddition);
    metrics[i].preferred = std::max(metrics[i].preferred, metrics[i].minimum);
  }
}

bool allocateColumnWidths(const std::array<ColumnMetrics, MAX_COLUMNS>& metrics, const size_t columnCount,
                          const uint16_t availableWidth, const uint16_t columnGap,
                          std::array<uint16_t, MAX_COLUMNS>& widths) {
  widths.fill(0);
  if (columnCount == 0 || columnCount > MAX_COLUMNS) return false;

  const uint32_t totalGaps = static_cast<uint32_t>(columnCount - 1) * columnGap;
  if (totalGaps >= availableWidth) return false;

  uint32_t minimumTotal = 0;
  for (size_t i = 0; i < columnCount; ++i) {
    widths[i] = metrics[i].minimum;
    minimumTotal += widths[i];
  }
  if (minimumTotal + totalGaps > availableWidth) return false;

  uint32_t remaining = availableWidth - totalGaps - minimumTotal;
  while (remaining > 0) {
    uint32_t unmetTotal = 0;
    for (size_t i = 0; i < columnCount; ++i) {
      unmetTotal += std::max(metrics[i].minimum, metrics[i].preferred) - widths[i];
    }
    if (unmetTotal == 0) break;

    const uint32_t passBudget = remaining;
    uint32_t distributed = 0;
    for (size_t i = 0; i < columnCount && distributed < passBudget; ++i) {
      const uint32_t target = std::max(metrics[i].minimum, metrics[i].preferred);
      const uint32_t unmet = target - widths[i];
      if (unmet == 0) continue;
      uint32_t addition = static_cast<uint32_t>((static_cast<uint64_t>(passBudget) * unmet) / unmetTotal);
      if (addition == 0) addition = 1;
      addition = std::min({addition, unmet, passBudget - distributed});
      widths[i] = static_cast<uint16_t>(widths[i] + addition);
      distributed += addition;
    }
    if (distributed == 0) break;
    remaining -= distributed;
  }

  while (remaining > 0) {
    uint32_t weightTotal = 0;
    for (size_t i = 0; i < columnCount; ++i) {
      weightTotal += std::max<uint16_t>(1, metrics[i].preferred);
    }
    const uint32_t passBudget = remaining;
    uint32_t distributed = 0;
    for (size_t i = 0; i < columnCount && distributed < passBudget; ++i) {
      const uint32_t weight = std::max<uint16_t>(1, metrics[i].preferred);
      uint32_t addition = static_cast<uint32_t>((static_cast<uint64_t>(passBudget) * weight) / weightTotal);
      if (addition == 0) addition = 1;
      addition = std::min(addition, passBudget - distributed);
      widths[i] = static_cast<uint16_t>(widths[i] + addition);
      distributed += addition;
    }
    remaining -= distributed;
  }

  return sumWidths(widths, 0, columnCount) + totalGaps == availableWidth;
}

}  // namespace TableLayout
