#include <gtest/gtest.h>

#include "components/themes/ThemeLayout.h"

namespace {

ThemeMetrics testMetrics() {
  ThemeMetrics metrics{};
  metrics.headerHeight = 48;
  metrics.buttonHintsHeight = 40;
  metrics.tabBarHeight = 36;
  metrics.listRowHeight = 42;
  metrics.listWithSubtitleRowHeight = 58;
  metrics.menuRowHeight = 42;
  metrics.homeCoverTileHeight = 0;
  metrics.homeCoverHeight = 0;
  metrics.verticalSpacing = 8;
  metrics.progressBarHeight = 4;
  return metrics;
}

ThemeLayoutNode slot(const char* id, ThemeLayoutSizeType type, int value) {
  ThemeLayoutNode node;
  node.id = id;
  node.sizeType = type;
  if (type == ThemeLayoutSizeType::Fixed) {
    node.size = value;
  } else {
    node.flex = value;
  }
  return node;
}

}  // namespace

TEST(ThemeLayoutTest, EmitsSuperMinimalHomeSlots) {
  ThemeLayoutNode root;
  root.id = "root";
  root.axis = ThemeLayoutAxis::Column;
  root.children.push_back(slot("header", ThemeLayoutSizeType::Fixed, 48));
  root.children.push_back(slot("menu", ThemeLayoutSizeType::Flex, 1));
  root.children.push_back(slot("buttons", ThemeLayoutSizeType::Fixed, 40));

  ThemeLayoutSlots slots;
  layoutThemeSlots(root, Rect{0, 0, 528, 792}, testMetrics(), slots);

  ASSERT_EQ(slots.size(), 3u);
  EXPECT_STREQ(slots.items[0].id, "header");
  EXPECT_EQ(slots.items[0].rect.x, 0);
  EXPECT_EQ(slots.items[0].rect.y, 0);
  EXPECT_EQ(slots.items[0].rect.width, 528);
  EXPECT_EQ(slots.items[0].rect.height, 48);

  EXPECT_STREQ(slots.items[1].id, "menu");
  EXPECT_EQ(slots.items[1].rect.x, 0);
  EXPECT_EQ(slots.items[1].rect.y, 48);
  EXPECT_EQ(slots.items[1].rect.width, 528);
  EXPECT_EQ(slots.items[1].rect.height, 704);

  EXPECT_STREQ(slots.items[2].id, "buttons");
  EXPECT_EQ(slots.items[2].rect.x, 0);
  EXPECT_EQ(slots.items[2].rect.y, 752);
  EXPECT_EQ(slots.items[2].rect.width, 528);
  EXPECT_EQ(slots.items[2].rect.height, 40);
}

TEST(ThemeLayoutTest, EmitsFileBrowserSlotsWithPath) {
  ThemeLayoutNode root;
  root.id = "root";
  root.axis = ThemeLayoutAxis::Column;
  root.gap = 8;
  root.children.push_back(slot("header", ThemeLayoutSizeType::Fixed, 48));
  root.children.push_back(slot("list", ThemeLayoutSizeType::Flex, 1));
  root.children.push_back(slot("path", ThemeLayoutSizeType::Fixed, 14));
  root.children.push_back(slot("buttons", ThemeLayoutSizeType::Fixed, 40));

  ThemeLayoutSlots slots;
  layoutThemeSlots(root, Rect{0, 0, 528, 792}, testMetrics(), slots);

  ASSERT_EQ(slots.size(), 4u);
  EXPECT_EQ(findThemeSlot(slots, "header").height, 48);
  EXPECT_EQ(findThemeSlot(slots, "list").height, 666);
  EXPECT_EQ(findThemeSlot(slots, "path").height, 14);
  EXPECT_EQ(findThemeSlot(slots, "buttons").height, 40);
}
