#pragma once

#include <SDCardManager.h>
#include <WString.h>
#include <string>

class BookCacheManager {
 public:
  /**
   * Migrate cache data for a file or directory.
   * If path is a directory, it recursively migrates all files within.
   * 
   * @param oldPath Original absolute path
   * @param newPath New absolute path
   * @return true if migration was successful or no cache existed
   */
  static bool migrateCache(const String& oldPath, const String& newPath);

  /**
   * Get the cache directory path for a given book file.
   * 
   * @param path Absolute path to the book file
   * @return Full path to the cache directory, or empty string if not a supported book type
   */
  static String getCachePath(const String& path);

 private:
  static bool isSupportedFile(const String& path);
  static String getCachePrefix(const String& path);
};
