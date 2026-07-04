#pragma once

#include <cstdint>
#include <string>

struct AppStoreManifestMetaData {
  std::string fetchedAt;
  uint32_t appCount = 0;
};

namespace AppStoreManifestMeta {

bool parse(const char* json, AppStoreManifestMetaData& out);
std::string serialize(const AppStoreManifestMetaData& meta);

bool readMeta(AppStoreManifestMetaData& out);
bool writeMeta(const AppStoreManifestMetaData& meta);

}  // namespace AppStoreManifestMeta
