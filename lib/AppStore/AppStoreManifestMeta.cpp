#include "AppStoreManifestMeta.h"

#include <HalStorage.h>
#include <Logging.h>

#include "AppStoreManifestMetaJson.h"
#include "AppStorePaths.h"

namespace AppStoreManifestMeta {

bool parse(const char* json, AppStoreManifestMetaData& out) {
  return AppStoreManifestMetaJson::parse(json, out);
}

std::string serialize(const AppStoreManifestMetaData& meta) {
  return AppStoreManifestMetaJson::serialize(meta);
}

bool readMeta(AppStoreManifestMetaData& out) {
  out = {};
  if (!Storage.exists(AppStorePaths::kManifestMetaFile)) {
    return false;
  }

  const String cached = Storage.readFile(AppStorePaths::kManifestMetaFile);
  if (cached.isEmpty()) {
    return false;
  }

  return parse(cached.c_str(), out);
}

bool writeMeta(const AppStoreManifestMetaData& meta) {
  if (meta.fetchedAt.empty()) {
    return false;
  }

  Storage.ensureDirectoryExists(AppStorePaths::kCacheDir);

  static constexpr char kMetaTmp[] = "/.crosspoint/apps/_cache/manifest_meta.json.tmp";
  const std::string json = serialize(meta);
  if (!Storage.writeFile(kMetaTmp, String(json.c_str()))) {
    LOG_ERR("APPS", "Failed to write manifest meta temp file");
    return false;
  }

  if (Storage.exists(AppStorePaths::kManifestMetaFile)) {
    Storage.remove(AppStorePaths::kManifestMetaFile);
  }

  if (!Storage.rename(kMetaTmp, AppStorePaths::kManifestMetaFile)) {
    LOG_ERR("APPS", "Failed to rename manifest meta into place");
    Storage.remove(kMetaTmp);
    return false;
  }

  return true;
}

}  // namespace AppStoreManifestMeta
