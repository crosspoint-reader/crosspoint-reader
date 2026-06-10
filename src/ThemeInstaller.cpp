#include "ThemeInstaller.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cctype>
#include <cstdio>
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
  const int written = snprintf(dirPath, sizeof(dirPath), "%s/%s", root, themeId);
  if (written < 0 || static_cast<size_t>(written) >= sizeof(dirPath)) {
    LOG_ERR("THEME", "Theme dir path too long: %s", themeId);
    return false;
  }
  if (!Storage.exists(dirPath) && !Storage.mkdir(dirPath)) {
    LOG_ERR("THEME", "Failed to create theme dir: %s", dirPath);
    return false;
  }
  return true;
}

bool ThemeInstaller::ensureParentDirs(const char* fullPath) {
  if (!fullPath) return false;
  char dir[180];
  const int written = snprintf(dir, sizeof(dir), "%s", fullPath);
  if (written < 0 || static_cast<size_t>(written) >= sizeof(dir)) {
    LOG_ERR("THEME", "Theme parent path too long");
    return false;
  }

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

bool ThemeInstaller::buildThemePath(const char* themeId, const char* relativePath, char* outBuf, size_t outBufSize) {
  if (!themeId || !relativePath || !outBuf || outBufSize == 0) return false;
  const char* root = SdCardThemeRegistry::findThemeRoot(themeId);
  if (!root) root = SdCardThemeRegistry::defaultWriteRoot();
  const int written = snprintf(outBuf, outBufSize, "%s/%s/%s", root, themeId, relativePath);
  if (written < 0 || static_cast<size_t>(written) >= outBufSize) {
    LOG_ERR("THEME", "Theme file path too long: %s/%s", themeId, relativePath);
    return false;
  }
  return true;
}

ThemeInstaller::Error ThemeInstaller::deleteTheme(const char* themeId) {
  if (!isValidThemeId(themeId)) return Error::INVALID_THEME_ID;

  const char* roots[] = {SdCardThemeRegistry::THEMES_DIR_HIDDEN, SdCardThemeRegistry::THEMES_DIR_VISIBLE};
  for (const char* root : roots) {
    char dirPath[180];
    const int written = snprintf(dirPath, sizeof(dirPath), "%s/%s", root, themeId);
    if (written < 0 || static_cast<size_t>(written) >= sizeof(dirPath)) {
      LOG_ERR("THEME", "Theme dir path too long: %s", themeId);
      return Error::INVALID_THEME_ID;
    }
    if (!Storage.exists(dirPath)) continue;
    if (!Storage.removeDir(dirPath)) {
      LOG_ERR("THEME", "Failed to remove theme dir: %s", dirPath);
      return Error::SD_WRITE_ERROR;
    }
  }

  if (strcmp(SETTINGS.sdThemeName, themeId) == 0) {
    SETTINGS.sdThemeName[0] = '\0';
    SETTINGS.saveToFile();
  }
  return Error::OK;
}

void ThemeInstaller::refreshRegistry() { registry_.discover(); }

bool ThemeInstaller::isThemeInstalled(const char* themeId) const { return registry_.findTheme(themeId) != nullptr; }
