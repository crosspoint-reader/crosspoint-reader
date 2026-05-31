#pragma once

#include <cstddef>

#include "components/themes/SdCardThemeRegistry.h"

class ThemeInstaller {
 public:
  enum class Error {
    OK,
    INVALID_THEME_ID,
    INVALID_FILE,
    SD_WRITE_ERROR,
  };

  explicit ThemeInstaller(SdCardThemeRegistry& registry);

  static bool isValidThemeId(const char* id);
  static bool isValidRelativePath(const char* path);
  bool ensureThemeDir(const char* themeId);
  bool ensureParentDirs(const char* fullPath);
  bool validateThemeFile(const char* path);
  static void buildThemePath(const char* themeId, const char* relativePath, char* outBuf, size_t outBufSize);
  Error deleteTheme(const char* themeId);
  void refreshRegistry();
  bool isThemeInstalled(const char* themeId) const;

 private:
  SdCardThemeRegistry& registry_;
};
