#pragma once

#include <I18n.h>

#include <functional>
#include <memory>

#include "CrossPointSettings.h"
#include "components/themes/BaseTheme.h"

class UITheme {
  // Static instance
  static UITheme instance;

 public:
  UITheme();
  static UITheme& getInstance() { return instance; }

  const ThemeMetrics& getMetrics() const { return *currentMetrics; }
  const BaseTheme& getTheme() const { return *currentTheme; }
  void reload();
  void setTheme(CrossPointSettings::UI_THEME type);
  static int getNumberOfItemsPerPage(const GfxRenderer& renderer, bool hasHeader, bool hasTabBar, bool hasButtonHints,
                                     bool hasSubtitle);
  static std::string makeSeparatorTitle(const std::string& title);
  static std::string makeSeparatorTitle(StrId labelId);
  static bool isSeparatorTitle(const std::string& title);
  static std::string stripSeparatorTitle(const std::string& title);
  // Returns a selectable predicate for use with ButtonNavigator::setSelectablePredicate().
  // Items whose title is marked as a separator (via makeSeparatorTitle) are skipped during
  // navigation. Pass the same title getter you pass to drawList so rendering and navigation
  // always agree on which items are separators.
  //
  // Preferred approach: derive from MenuListActivity (see MenuListActivity.h) which handles
  // separator skipping, navigation, and drawList automatically via SettingInfo items.
  //
  // Manual usage (for activities that don't derive from MenuListActivity).
  // Assumes `items` is a member variable (std::vector<SettingInfo> items):
  //
  //   // populate in constructor or onEnter:
  //   items.push_back(SettingInfo::Separator(StrId::STR_MY_SECTION));
  //   items.push_back(SettingInfo::Toggle(StrId::STR_MY_TOGGLE, &CrossPointSettings::myFlag));
  //
  //   // onEnter: wire navigation so separators are skipped:
  //   const auto pred = UITheme::makeSelectablePredicate(items.size(),
  //       [this](int i) { return items[i].getTitle(); });
  //   buttonNavigator.setSelectablePredicate(pred, items.size());
  //
  //   // render: pass the same getter to drawList; separator rows are drawn automatically:
  //   const auto& s = items;  // local ref for lambda capture
  //   GUI.drawList(renderer, rect, s.size(), selectedIndex,
  //       [&s](int i) { return s[i].getTitle(); },
  //       nullptr, nullptr,
  //       [&s](int i) { return s[i].getDisplayValue(); }, true);
  static std::function<bool(int)> makeSelectablePredicate(int total, std::function<std::string(int)> titleGetter);

  // Returns the drawable content Rect accounting for screen orientation and visible button hints.
  // Bottom hints occupy the physical bottom edge; side hints occupy the physical right edge.
  // The mapping to logical edges is orientation-dependent.
  static Rect getContentRect(const GfxRenderer& renderer, bool hasBottomHints, bool hasSideHints);
  static std::string getCoverThumbPath(std::string coverBmpPath, int coverHeight);
  static UIIcon getFileIcon(const std::string& filename);
  static int getStatusBarHeight();
  static int getProgressBarHeight();

 private:
  const ThemeMetrics* currentMetrics;
  std::unique_ptr<BaseTheme> currentTheme;
};

// Helper macro to access current theme
#define GUI UITheme::getInstance().getTheme()
