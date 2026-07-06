#include "AppBootstrap.h"

#include <HalStorage.h>
#include <Logging.h>

#include <string>
#include <vector>

#include "AppRegistry.h"
#include "AppStoreManifest.h"
#include "AppStorePaths.h"
#include "BundledAppSeeder.h"

void AppBootstrap::cleanupTmpDir() {
  if (!Storage.exists(AppStorePaths::kTmpDir)) {
    return;
  }

  const std::vector<String> files = Storage.listFiles(AppStorePaths::kTmpDir, 64);
  for (const String& name : files) {
    if (name == "." || name == "..") {
      continue;
    }
    std::string path = std::string(AppStorePaths::kTmpDir) + "/" + name.c_str();
    Storage.remove(path.c_str());
    LOG_DBG("APPS", "Removed stale tmp file: %s", path.c_str());
  }
}

void AppBootstrap::onBoot() {
  Storage.ensureDirectoryExists(AppStorePaths::kAppsRoot);
  Storage.ensureDirectoryExists(AppStorePaths::kTmpDir);
  Storage.ensureDirectoryExists(AppStorePaths::kCacheDir);
  cleanupTmpDir();

  BundledAppSeeder::seedMissingApps();

  if (!APP_REGISTRY.loadFromFile()) {
    LOG_ERR("APPS", "Registry load failed; starting with empty installed list");
  }

  if (!APP_STORE_CATALOG.loadFromCache()) {
    LOG_ERR("APPS", "Discover catalog load failed; Discover list will be empty");
  }
}
