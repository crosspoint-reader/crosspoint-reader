#pragma once

#include <HalGPIO.h>

#include <functional>
#include <vector>

#include "CrossPointSettings.h"
#include "components/themes/BaseTheme.h"

class UITheme {
  // Static instance
  static UITheme instance;

 public:
  UITheme();
  explicit UITheme(HalGPIO& gpioRef);
  static UITheme& getInstance() { return instance; }

  const ThemeMetrics& getMetrics() { return *currentMetrics; }
  const BaseTheme& getTheme() { return *currentTheme; }
  void reload();
  void setTheme(CrossPointSettings::UI_THEME type);
  static int getNumberOfItemsPerPage(const GfxRenderer& renderer, bool hasHeader, bool hasTabBar, bool hasButtonHints,
                                     bool hasSubtitle);
  static std::string getCoverThumbPath(std::string coverBmpPath, int coverHeight);

  HalGPIO* gpio = nullptr;

 private:
  const ThemeMetrics* currentMetrics;
  const BaseTheme* currentTheme;
};

// Helper macro to access current theme
#define GUI UITheme::getInstance().getTheme()
