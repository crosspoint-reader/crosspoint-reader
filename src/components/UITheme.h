#pragma once

#include <EpdFontFamily.h>

#include <functional>
#include <memory>
#include <vector>

#include "CrossPointSettings.h"
#include "components/themes/BaseTheme.h"
#include "components/themes/SdCardThemeRegistry.h"

class UITheme {
  // Static instance
  static UITheme instance;

 public:
  UITheme();
  static UITheme& getInstance() { return instance; }

  const ThemeMetrics& getMetrics() const { return *currentMetrics; }
  const BaseTheme& getTheme() const { return *currentTheme; }
  int getHomeCoverThumbHeight() const;
  std::vector<int> getHomeCoverThumbHeights() const;
  int getRecentBooksCoverThumbHeight() const;
  const ThemeHomeScreenSpec* getHomeScreenSpec() const {
    return currentSdHomeScreen.enabled ? &currentSdHomeScreen : nullptr;
  }
  const ThemeScreenSpec* getScreenSpec(ThemeScreenKind screen) const;
  const ThemeReaderChromeSpec* getReaderChromeSpec() const {
    return currentSdReaderChrome.battery.enabled ? &currentSdReaderChrome : nullptr;
  }
  SdCardThemeRegistry& registry() { return themeRegistry; }
  void refreshRegistry();
  void releaseSdThemeAssetMemory();
  Rect getScreenSafeArea(const GfxRenderer& renderer, bool hasFrontButtonHints = false,
                         bool hasSideButtonHints = false);
  static void drawCenteredText(const GfxRenderer& renderer, Rect screen, int fontId, int y, const char* text,
                               bool black = true, EpdFontFamily::Style style = EpdFontFamily::REGULAR);
  void reload();
  void setTheme(CrossPointSettings::UI_THEME type);
  // Per-board UI/touch-target density scale: 1.0 on button devices, >1 on
  // high-density touch boards (wired to the board profile on touch builds).
  static float uiScale();
  static int getNumberOfItemsPerPage(const GfxRenderer& renderer, bool hasHeader, bool hasTabBar, bool hasButtonHints,
                                     bool hasSubtitle, int extraReservedHeight = 0);
  static std::string getCoverThumbPath(std::string coverBmpPath, int coverHeight);
  static UIIcon getFileIcon(const std::string& filename);
  static int getStatusBarHeight();
  static int getProgressBarHeight();

 private:
  void clearSdThemeState();

  const ThemeMetrics* currentMetrics;
  ThemeMetrics currentSdMetrics;
  ThemeHomeRecentsSpec currentSdHomeRecents;
  ThemeButtonMenuSpec currentSdButtonMenu;
  ThemeListSpec currentSdList;
  ThemeButtonHintsSpec currentSdButtonHints;
  ThemeTabBarSpec currentSdTabBar;
  ThemeHeaderSpec currentSdHeader;
  ThemeHomeScreenSpec currentSdHomeScreen;
  ThemeScreenSpec currentSdFileBrowserScreen;
  ThemeScreenSpec currentSdRecentBooksScreen;
  ThemeScreenSpec currentSdSettingsScreen;
  ThemeScreenSpec currentSdReaderScreen;
  ThemeReaderChromeSpec currentSdReaderChrome;
  std::string currentSdThemePath;
  ThemeIconMap currentSdIcons;
  std::unique_ptr<BaseTheme> currentTheme;
  SdCardThemeRegistry themeRegistry;
};

// Unified theme metric scaling (definition + field classification in UITheme.cpp).
//   res     - resolution ratio (panel pixels vs theme design resolution)
//   density - per-board UI density (UITheme::uiScale())
// Applies res to every pixel field; density additionally to non-fit-constrained
// chrome. Degrades to either factor alone when the other is 1.0.
ThemeMetrics scaleThemeMetrics(const ThemeMetrics& base, float res, float density);

// Helper macro to access current theme
#define GUI UITheme::getInstance().getTheme()
