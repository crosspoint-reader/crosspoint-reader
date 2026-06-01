#pragma once

#include <string>
#include <vector>

#include "components/themes/BaseTheme.h"

struct SdThemeDeviceConstraints {
  int screenWidth = 0;
  int screenHeight = 0;
  int frontButtons = 0;
  std::string sideButtons;
};

struct SdCardThemeInfo {
  std::string id;
  std::string name;
  std::string path;
  std::string inherits;
  std::string deviceId;
  ThemeMetrics metrics = {};
  ThemeHomeRecentsSpec homeRecents;
  ThemeButtonMenuSpec buttonMenu;
  ThemeListSpec list;
  ThemeButtonHintsSpec buttonHints;
  ThemeTabBarSpec tabBar;
  ThemeHeaderSpec header;
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
  int getThemeCount() const { return static_cast<int>(themes_.size()); }
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
