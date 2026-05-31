#include "ThemeInstaller.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cctype>
#include <cstring>

#include "CrossPointSettings.h"

ThemeInstaller::ThemeInstaller(SdCardThemeRegistry& registry) : registry_(registry) {}

bool ThemeInstaller::isValidThemeId(const char* id) {
  if (id == nullptr || id[0] == '\0') return false;
  if (strstr(id, "..") != nullptr || strchr(id, '/') != nullptr || strchr(id, '\\') != nullptr) return false;
  for (const char* p = id; *p; ++p) {
    const char c = *p;
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') return false;
  }
  return true;
}

bool ThemeInstaller::isValidRelativePath(const char* path) {
  if (path == nullptr || path[0] == '\0' || path[0] == '/') return false;
  if (strstr(path, "..") != nullptr || strchr(path, '\\') != nullptr) return false;

  bool segmentHasChar = false;
  for (const char* p = path; *p; ++p) {
    const char c = *p;
    if (c == '/') {
      if (!segmentHasChar) return false;
      segmentHasChar = false;
      continue;
    }
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_' && c != '.') return false;
    segmentHasChar = true;
  }
  return segmentHasChar;
}

bool ThemeInstaller::ensureThemeDir(const char* themeId) {
  if (!isValidThemeId(themeId)) return false;
  const char* root = SdCardThemeRegistry::findThemeRoot(themeId);
  if (!root) root = SdCardThemeRegistry::defaultWriteRoot();

  if (!Storage.exists(root) && !Storage.mkdir(root)) {
    LOG_ERR("THEME", "Failed to create themes dir: %s", root);
    return false;
  }

  char dirPath[180];
  snprintf(dirPath, sizeof(dirPath), "%s/%s", root, themeId);
  if (!Storage.exists(dirPath) && !Storage.mkdir(dirPath)) {
    LOG_ERR("THEME", "Failed to create theme dir: %s", dirPath);
    return false;
  }
  return true;
}

bool ThemeInstaller::ensureParentDirs(const char* fullPath) {
  char dir[180];
  strncpy(dir, fullPath, sizeof(dir) - 1);
  dir[sizeof(dir) - 1] = '\0';

  char* slash = strrchr(dir, '/');
  if (!slash) return true;
  *slash = '\0';
  return Storage.ensureDirectoryExists(dir);
}

bool ThemeInstaller::validateThemeFile(const char* path) {
  HalFile file;
  if (!Storage.openFileForRead("THEME", path, file)) return false;
  const bool ok = file.fileSize() > 0;
  file.close();
  return ok;
}

void ThemeInstaller::buildThemePath(const char* themeId, const char* relativePath, char* outBuf, size_t outBufSize) {
  const char* root = SdCardThemeRegistry::findThemeRoot(themeId);
  if (!root) root = SdCardThemeRegistry::defaultWriteRoot();
  snprintf(outBuf, outBufSize, "%s/%s/%s", root, themeId, relativePath);
}

ThemeInstaller::Error ThemeInstaller::deleteTheme(const char* themeId) {
  if (!isValidThemeId(themeId)) return Error::INVALID_THEME_ID;

  const char* roots[] = {SdCardThemeRegistry::THEMES_DIR_HIDDEN, SdCardThemeRegistry::THEMES_DIR_VISIBLE};
  for (const char* root : roots) {
    char dirPath[180];
    snprintf(dirPath, sizeof(dirPath), "%s/%s", root, themeId);
    if (!Storage.exists(dirPath)) continue;
    if (!Storage.removeDir(dirPath)) {
      LOG_ERR("THEME", "Failed to remove theme dir: %s", dirPath);
      return Error::SD_WRITE_ERROR;
    }
  }

  if (strcmp(SETTINGS.sdThemeName, themeId) == 0) {
    SETTINGS.sdThemeName[0] = '\0';
    SETTINGS.uiTheme = CrossPointSettings::LYRA;
    SETTINGS.saveToFile();
  }
  return Error::OK;
}

void ThemeInstaller::refreshRegistry() { registry_.discover(); }

bool ThemeInstaller::isThemeInstalled(const char* themeId) const { return registry_.findTheme(themeId) != nullptr; }
