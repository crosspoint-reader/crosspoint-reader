#include "FsHelpers.h"

#include <SD.h>

#include <vector>

bool FsHelpers::openFileForRead(const char* moduleName, const char* path, File& file) {
  if (!SD.exists(path)) {
    Serial.printf("[%lu] [%s] File does not exist: %s\n", millis(), moduleName, path);
    return false;
  }

  file = SD.open(path, FILE_READ);
  if (!file) {
    Serial.printf("[%lu] [%s] Failed to open file for reading: %s\n", millis(), moduleName, path);
    return false;
  }
  return true;
}

bool FsHelpers::openFileForRead(const char* moduleName, const std::string& path, File& file) {
  return openFileForRead(moduleName, path.c_str(), file);
}

bool FsHelpers::openFileForRead(const char* moduleName, const String& path, File& file) {
  return openFileForRead(moduleName, path.c_str(), file);
}

bool FsHelpers::openFileForWrite(const char* moduleName, const char* path, File& file) {
  file = SD.open(path, FILE_WRITE, true);
  if (!file) {
    Serial.printf("[%lu] [%s] Failed to open file for writing: %s\n", millis(), moduleName, path);
    return false;
  }
  return true;
}

bool FsHelpers::openFileForWrite(const char* moduleName, const std::string& path, File& file) {
  return openFileForWrite(moduleName, path.c_str(), file);
}

bool FsHelpers::openFileForWrite(const char* moduleName, const String& path, File& file) {
  return openFileForWrite(moduleName, path.c_str(), file);
}

bool FsHelpers::removeDir(const char* path) {
  // 1. Open the directory
  File dir = SD.open(path);
  if (!dir) {
    return false;
  }
  if (!dir.isDirectory()) {
    return false;
  }

  File file = dir.openNextFile();
  while (file) {
    String filePath = path;
    if (!filePath.endsWith("/")) {
      filePath += "/";
    }
    filePath += file.name();

    if (file.isDirectory()) {
      if (!removeDir(filePath.c_str())) {
        return false;
      }
    } else {
      if (!SD.remove(filePath.c_str())) {
        return false;
      }
    }
    file = dir.openNextFile();
  }

  return SD.rmdir(path);
}

std::string FsHelpers::normalisePath(const std::string& path) {
  std::vector<std::string> components;
  std::string component;

  for (const auto c : path) {
    if (c == '/') {
      if (!component.empty()) {
        if (component == "..") {
          if (!components.empty()) {
            components.pop_back();
          }
        } else {
          components.push_back(component);
        }
        component.clear();
      }
    } else {
      component += c;
    }
  }

  if (!component.empty()) {
    components.push_back(component);
  }

  std::string result;
  for (const auto& c : components) {
    if (!result.empty()) {
      result += "/";
    }
    result += c;
  }

  return result;
}
