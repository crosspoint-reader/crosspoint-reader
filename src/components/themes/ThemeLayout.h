#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "components/themes/BaseTheme.h"

enum class ThemeLayoutAxis { Column, Row };
enum class ThemeLayoutSizeType { Flex, Fixed, Token };
enum class ThemeScreenKind { Home, FileBrowser, RecentBooks, Settings, Reader };
enum class ThemeHomeWidgetType {
  Header,
  HeaderTitle,
  Battery,
  Clock,
  Recents,
  FeaturedBookCard,
  RecentCoverGrid,
  LauncherList,
  LauncherGrid,
  ButtonHints
};
enum class ThemeHomeAction { FileBrowser, RecentBooks, OpdsBrowser, FileTransfer, Settings, RecentBook };
enum class ThemeLauncherPresentation { Menu, IconTabs };
enum class ThemeWidgetSelectionStyle { Fill, Outline, CoverFrame, None };
enum class ThemeHomeNavigationMode { Linear, SplitAxis, CarouselAxis };
enum class ThemeScreenWidgetType { List, CoverGrid };
enum class ThemeButtonHintLabel { Default, Empty, Back, Home, Select, Confirm, Open, Toggle, Up, Down, Left, Right };

constexpr size_t kMaxThemeWidgets = 12;
constexpr size_t kMaxThemeLauncherItems = 12;
constexpr size_t kMaxThemeCoverGridItems = 64;

struct ThemeLayoutNode {
  std::string id;
  ThemeLayoutAxis axis = ThemeLayoutAxis::Column;
  int gap = 0;
  ThemeLayoutSizeType sizeType = ThemeLayoutSizeType::Flex;
  int size = 0;
  int flex = 1;
  std::string sizeToken;
  std::vector<ThemeLayoutNode> children;
};

struct ThemeHomeLauncherSpec {
  std::string text;
  UIIcon icon = UIIcon::None;
  ThemeHomeAction action = ThemeHomeAction::FileBrowser;
};

struct ThemeEdgeInsets {
  int top = 0;
  int right = 0;
  int bottom = 0;
  int left = 0;
};

struct ThemeHomeLauncherWidgetSpec {
  ThemeLauncherPresentation presentation = ThemeLauncherPresentation::Menu;
  int columns = 1;
  int rows = 0;
  int gap = 0;
  int iconSize = 32;
  int selectedRadius = 6;
  std::vector<ThemeHomeLauncherSpec> items;
};

struct ThemeFeaturedBookWidgetSpec {
  int startIndex = 0;
  int coverWidth = 0;
  int coverHeight = 0;
  int coverGap = 14;
  int titleGap = 8;
  int selectedRadius = 6;
  int placeholderIconSize = 0;
};

struct ThemeCoverGridWidgetSpec {
  bool configured = false;
  int columns = 1;
  int rows = 0;
  int gap = 0;
  int rowGap = -1;
  int coverWidth = 0;
  int coverHeight = 0;
  int placeholderIconSize = 0;
  int rowHeight = 0;
  int labelHeight = 20;
  int labelGap = 2;
  int labelLines = 1;
  int startIndex = 0;
  int selectedRadius = 6;
  ThemeWidgetSelectionStyle selectionStyle = ThemeWidgetSelectionStyle::Fill;
  ThemeEdgeInsets cellInset;
  ThemeEdgeInsets labelInset;
};

struct ThemeButtonHintsWidgetSpec {
  ThemeButtonHintLabel back = ThemeButtonHintLabel::Default;
  ThemeButtonHintLabel confirm = ThemeButtonHintLabel::Default;
  ThemeButtonHintLabel previous = ThemeButtonHintLabel::Default;
  ThemeButtonHintLabel next = ThemeButtonHintLabel::Default;
};

struct ThemeHomeWidgetSpec {
  std::string slot;
  ThemeHomeWidgetType type = ThemeHomeWidgetType::LauncherList;
  int layer = 0;
  int offsetX = 0;
  int offsetY = 0;
  ThemeEdgeInsets bleed;
  ThemeEdgeInsets inset;
  ThemeHomeLauncherWidgetSpec launcher;
  ThemeFeaturedBookWidgetSpec featured;
  ThemeCoverGridWidgetSpec coverGrid;
  ThemeButtonHintsWidgetSpec buttonHints;
};

struct ThemeHomeScreenSpec {
  bool enabled = false;
  ThemeHomeNavigationMode navigation = ThemeHomeNavigationMode::Linear;
  bool hasInitialAction = false;
  ThemeHomeAction initialAction = ThemeHomeAction::FileBrowser;
  ThemeLayoutNode layout;
  std::vector<ThemeHomeWidgetSpec> widgets;
};

struct ThemeScreenSpec {
  bool enabled = false;
  ThemeLayoutNode layout;
  struct Widget {
    std::string slot;
    ThemeScreenWidgetType type = ThemeScreenWidgetType::List;
    ThemeCoverGridWidgetSpec coverGrid;
  };
  std::vector<Widget> widgets;
};

struct ThemeLayoutSlot {
  const char* id = nullptr;
  Rect rect;
};

struct ThemeLayoutSlots {
  static constexpr size_t kMaxSlots = 32;

  ThemeLayoutSlot items[kMaxSlots];
  size_t count = 0;
  bool overflow = false;

  void clear() {
    count = 0;
    overflow = false;
  }

  bool empty() const { return count == 0; }
  size_t size() const { return count; }

  void push(const char* id, Rect rect) {
    if (count >= kMaxSlots) {
      overflow = true;
      return;
    }
    items[count++] = ThemeLayoutSlot{id, rect};
  }
};

int themeLayoutTokenSize(const ThemeMetrics& metrics, const std::string& token);
void layoutThemeSlots(const ThemeLayoutNode& node, Rect rect, const ThemeMetrics& metrics, ThemeLayoutSlots& slots);
Rect findThemeSlot(const ThemeLayoutSlots& slots, const std::string& id);
Rect normalizeThemeHeaderSlot(Rect rect, const ThemeMetrics& metrics);
