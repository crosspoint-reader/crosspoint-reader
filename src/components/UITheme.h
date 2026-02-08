#pragma once

#include <functional>
#include <vector>

#include "CrossPointSettings.h"
#include "components/themes/BaseTheme.h"

class MappedInputManager;

class UITheme {
  // Static instance
  static UITheme instance;

 public:
  UITheme();
  static UITheme& getInstance() { return instance; }

  const ThemeMetrics& getMetrics() { return *currentMetrics; }
  const BaseTheme& getTheme() { return *currentTheme; }
  void reload();
  void setTheme(CrossPointSettings::UI_THEME type);
  static int getNumberOfItemsPerPage(const GfxRenderer& renderer, bool hasHeader, bool hasTabBar, bool hasButtonHints,
                                     bool hasSubtitle);
  static std::string getCoverThumbPath(std::string coverBmpPath, int coverHeight);
  void handleListScrolling(const GfxRenderer& renderer, int listSize, int pageItems, size_t& selectorIndex,
                           const MappedInputManager& mappedInput, bool& updateRequired);

 private:
  const ThemeMetrics* currentMetrics;
  const BaseTheme* currentTheme;
};

// Helper macro to access current theme
#define GUI UITheme::getInstance().getTheme()
