#include "BookCacheManager.h"
#include "StringUtils.h"
#include <HardwareSerial.h>
#include "../RecentBooksStore.h"
#include "../CrossPointState.h"

bool BookCacheManager::migrateCache(const String& oldPath, const String& newPath) {
  if (oldPath == newPath) return true;

  // Update Recent Books list
  RECENT_BOOKS.updatePath(oldPath.c_str(), newPath.c_str());

  // Update last opened book state if matches
  if (CrossPointState::getInstance().openEpubPath == oldPath.c_str()) {
    CrossPointState::getInstance().openEpubPath = newPath.c_str();
    CrossPointState::getInstance().saveToFile();
  }

  if (!SdMan.exists(oldPath.c_str())) {
    return false;
  }

  FsFile item = SdMan.open(oldPath.c_str());
  if (!item) return false;

  bool isDir = item.isDirectory();
  item.close();

  if (isDir) {
    // Recursively migrate contents of the directory
    FsFile dir = SdMan.open(oldPath.c_str());
    FsFile entry = dir.openNextFile();
    char nameBuf[512];
    bool success = true;

    while (entry) {
      entry.getName(nameBuf, sizeof(nameBuf));
      String fileName = String(nameBuf);
      entry.close();

      String subOldPath = oldPath + "/" + fileName;
      String subNewPath = newPath + "/" + fileName;
      
      if (!migrateCache(subOldPath, subNewPath)) {
        success = false;
      }
      entry = dir.openNextFile();
    }
    dir.close();
    return success;
  }

  // It's a file. check if it's a supported book type
  if (!isSupportedFile(oldPath)) {
    return true; // Not a book, nothing to migrate
  }

  String oldCache = getCachePath(oldPath);
  String newCache = getCachePath(newPath);

  if (oldCache.isEmpty() || newCache.isEmpty() || oldCache == newCache) {
    return true;
  }

  if (SdMan.exists(oldCache.c_str())) {
    if (SdMan.exists(newCache.c_str())) {
      Serial.printf("[%lu] [BCM] New cache already exists for %s, removing old cache\n", millis(), newPath.c_str());
      SdMan.removeDir(oldCache.c_str());
      return true;
    }

    Serial.printf("[%lu] [BCM] Migrating cache: %s -> %s\n", millis(), oldCache.c_str(), newCache.c_str());
    if (SdMan.rename(oldCache.c_str(), newCache.c_str())) {
      return true;
    } else {
      Serial.printf("[%lu] [BCM] Failed to rename cache directory\n", millis());
      return false;
    }
  }

  return true; // No old cache to migrate
}

String BookCacheManager::getCachePath(const String& path) {
  if (!isSupportedFile(path)) return "";

  auto hash = std::hash<std::string>{}(path.c_str());
  return String("/.crosspoint/") + getCachePrefix(path) + "_" + String(hash);
}

bool BookCacheManager::isSupportedFile(const String& path) {
  return StringUtils::checkFileExtension(path, ".epub") ||
         StringUtils::checkFileExtension(path, ".txt") ||
         StringUtils::checkFileExtension(path, ".xtc") ||
         StringUtils::checkFileExtension(path, ".xtg") ||
         StringUtils::checkFileExtension(path, ".xth");
}

String BookCacheManager::getCachePrefix(const String& path) {
  if (StringUtils::checkFileExtension(path, ".epub")) return "epub";
  if (StringUtils::checkFileExtension(path, ".txt")) return "txt";
  return "xtc"; // .xtc, .xtg, .xth
}
