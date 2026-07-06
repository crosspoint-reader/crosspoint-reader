#include "BundledAppSeeder.h"

#include <AppDateTimeFormat.h>
#include <AppRegistry.h>
#include <AppStorePaths.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstring>

namespace {

bool isAppInstalled(const std::string& appId) {
  for (const AppRegistryEntry& entry : APP_REGISTRY.getEntries()) {
    if (entry.id == appId) {
      return true;
    }
  }
  return false;
}

bool writeProgmemBlobToFile(const uint8_t* data, size_t size, const char* destPath) {
  HalFile outFile;
  if (!Storage.openFileForWrite("APPS", destPath, outFile)) {
    LOG_ERR("APPS", "Seed failed: could not open %s for write", destPath);
    return false;
  }

  uint8_t buffer[512];
  size_t offset = 0;
  while (offset < size) {
    const size_t chunk = (size - offset) > sizeof(buffer) ? sizeof(buffer) : (size - offset);
    memcpy(buffer, data + offset, chunk);
    if (outFile.write(buffer, chunk) != chunk) {
      LOG_ERR("APPS", "Seed failed: short write to %s", destPath);
      outFile.close();
      Storage.remove(destPath);
      return false;
    }
    offset += chunk;
  }

  outFile.close();
  return true;
}

}  // namespace

void BundledAppSeeder::seedMissingApps() {
  if (BundledApps::kAppCount == 0) {
    return;
  }

  APP_REGISTRY.loadFromFile();

  for (size_t index = 0; index < BundledApps::kAppCount; index++) {
    const BundledAppBlob& app = BundledApps::kApps[index];
    if (app.id == nullptr || app.id[0] == '\0') {
      continue;
    }
    if (isAppInstalled(app.id)) {
      continue;
    }

    char tmpZipPath[96];
    snprintf(tmpZipPath, sizeof(tmpZipPath), "%s/seed-%s.cpapp", AppStorePaths::kTmpDir, app.id);
    if (!writeProgmemBlobToFile(app.data, app.size, tmpZipPath)) {
      continue;
    }

    AppInstallRequest request;
    request.id = app.id;
    request.name = app.name != nullptr ? app.name : app.id;
    request.version = app.version != nullptr ? app.version : "0.0.0";
    request.installedAt = AppDateTimeFormat::formatNowIso8601UtcMinusSeconds(static_cast<uint32_t>(index));

    if (AppBundleInstaller::installFromZipFile(tmpZipPath, request)) {
      LOG_INF("APPS", "Seeded bundled app %s", app.id);
    } else {
      LOG_ERR("APPS", "Seed failed for bundled app %s", app.id);
    }

    Storage.remove(tmpZipPath);
  }
}
