#pragma once

#include <string>
#include <vector>

#include "components/themes/BaseTheme.h"
#include "components/themes/ThemeLayout.h"

// Theme's declared design resolution (portrait). Used to scale the theme's pixel
// metrics to the actual device panel (see scaleThemeMetrics in UITheme.cpp).
struct SdThemeDeviceConstraints {
  int screenWidth = 0;
  int screenHeight = 0;
};

struct SdCardThemeInfo {
  std::string id;
  std::string name;
  int version = 0;
  std::string path;
  std::string inherits;
  ThemeMetrics metrics = {};
  ThemeHomeRecentsSpec homeRecents;
  ThemeButtonMenuSpec buttonMenu;
  ThemeListSpec list;
  ThemeButtonHintsSpec buttonHints;
  ThemeTabBarSpec tabBar;
  ThemeHeaderSpec header;
  ThemeHomeScreenSpec homeScreen;
  ThemeScreenSpec fileBrowserScreen;
  ThemeScreenSpec recentBooksScreen;
  ThemeScreenSpec settingsScreen;
  ThemeScreenSpec readerScreen;
  ThemeReaderChromeSpec readerChrome;
  ThemeIconMap icons;
  SdThemeDeviceConstraints constraints;
};

class SdCardThemeRegistry {
 public:
  static constexpr int MAX_SD_THEMES = 64;
  static constexpr const char* THEMES_DIR_HIDDEN = "/.themes";
  static constexpr const char* THEMES_DIR_VISIBLE = "/themes";

  bool discover();
  void clear();

  const std::vector<SdCardThemeInfo>& getThemes() const { return themes_; }
  const SdCardThemeInfo* findTheme(const std::string& id) const;
  static const char* findThemeRoot(const char* themeId);
  static const char* defaultWriteRoot();

 private:
  std::vector<SdCardThemeInfo> themes_;

  static const char* activeDeviceId();
  static bool parseThemeJson(const char* themeDirPath, SdCardThemeInfo& out);
  static bool isSafeId(const char* value);
  static bool isSafeThemeId(const char* value);
  static void scanRoot(const char* rootPath, std::vector<SdCardThemeInfo>& out);
};
