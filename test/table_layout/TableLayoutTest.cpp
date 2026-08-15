#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>

#include "Epub/TableLayout.h"

namespace {

uint32_t occupiedWidth(const std::array<uint16_t, TableLayout::MAX_COLUMNS>& widths, const size_t count,
                       const uint16_t gap) {
  uint32_t total = count > 0 ? static_cast<uint32_t>(count - 1) * gap : 0;
  for (size_t i = 0; i < count; ++i) total += widths[i];
  return total;
}

TEST(TableLayoutTest, GivesProseMostSpaceAndKeepsNumericColumnNarrow) {
  std::array<TableLayout::ColumnMetrics, TableLayout::MAX_COLUMNS> metrics = {};
  metrics[0] = {.minimum = 100, .preferred = 300};
  metrics[1] = {.minimum = 10, .preferred = 10};
  std::array<uint16_t, TableLayout::MAX_COLUMNS> widths = {};

  ASSERT_TRUE(TableLayout::allocateColumnWidths(metrics, 2, 320, 8, widths));
  EXPECT_EQ(occupiedWidth(widths, 2, 8), 320u);
  EXPECT_GT(widths[0], 290);
  EXPECT_LT(widths[1], 20);
}

TEST(TableLayoutTest, SimilarColumnsReceiveSimilarWidths) {
  std::array<TableLayout::ColumnMetrics, TableLayout::MAX_COLUMNS> metrics = {};
  metrics[0] = {.minimum = 30, .preferred = 90};
  metrics[1] = {.minimum = 32, .preferred = 92};
  metrics[2] = {.minimum = 28, .preferred = 88};
  std::array<uint16_t, TableLayout::MAX_COLUMNS> widths = {};

  ASSERT_TRUE(TableLayout::allocateColumnWidths(metrics, 3, 300, 6, widths));
  EXPECT_EQ(occupiedWidth(widths, 3, 6), 300u);
  const uint16_t smallest = std::min({widths[0], widths[1], widths[2]});
  const uint16_t largest = std::max({widths[0], widths[1], widths[2]});
  EXPECT_LE(largest - smallest, 5);
}

TEST(TableLayoutTest, RejectsColumnsWhoseMinimumsDoNotFit) {
  std::array<TableLayout::ColumnMetrics, TableLayout::MAX_COLUMNS> metrics = {};
  metrics[0] = {.minimum = 180, .preferred = 200};
  metrics[1] = {.minimum = 150, .preferred = 180};
  std::array<uint16_t, TableLayout::MAX_COLUMNS> widths = {};

  EXPECT_FALSE(TableLayout::allocateColumnWidths(metrics, 2, 320, 8, widths));
}

TEST(TableLayoutTest, ColspanRequirementExpandsCoveredColumns) {
  std::array<TableLayout::ColumnMetrics, TableLayout::MAX_COLUMNS> metrics = {};
  metrics[0] = {.minimum = 20, .preferred = 30};
  metrics[1] = {.minimum = 20, .preferred = 30};

  TableLayout::applySpanRequirement(metrics, 2, 0, 2, 100, 180, 8);

  EXPECT_GE(metrics[0].minimum + metrics[1].minimum + 8, 100);
  EXPECT_GE(metrics[0].preferred + metrics[1].preferred + 8, 180);
}

}  // namespace
