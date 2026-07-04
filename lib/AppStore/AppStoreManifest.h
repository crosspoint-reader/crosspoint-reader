#pragma once

#include "AppCatalogEntry.h"
#include "AppStoreManifestResolve.h"
#include "AppStoreManifestTypes.h"

#include <cstdint>
#include <string>
#include <vector>

// Loads the Discover catalog: remote HTTPS manifest, SD cache, then builtin JSON.
class AppStoreManifest {
 public:
  static AppStoreManifest& getInstance();

  AppStoreManifest(const AppStoreManifest&) = delete;
  AppStoreManifest& operator=(const AppStoreManifest&) = delete;

  bool load();
  // Loads catalog from SD cache, then builtin fallback. Never hits the remote API.
  bool loadFromCache();
  AppCatalogSource getSource() const { return source_; }
  const std::vector<AppCatalogEntry>& getEntries() const { return entries_; }

  static bool resolveAndParse(const AppCatalogResolveInput& input, std::vector<AppCatalogEntry>& outEntries,
                              uint8_t& outVersion, AppCatalogSource& outSource) {
    return appStoreManifestResolveAndParse(input, outEntries, outVersion, outSource);
  }

 private:
  AppStoreManifest() = default;

  bool tryLoadRemote(std::string& outJson);
  bool tryLoadCache(std::string& outJson);
  bool writeCache(const std::string& json) const;
  bool resolveCatalog(bool allowRemote, const char* callerLabel);

  std::vector<AppCatalogEntry> entries_;
  AppCatalogSource source_ = AppCatalogSource::None;
  static AppStoreManifest instance_;
};

#define APP_STORE_CATALOG AppStoreManifest::getInstance()
