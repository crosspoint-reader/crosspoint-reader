#pragma once

#include "AppStoreManifestMeta.h"

namespace AppStoreManifestMetaJson {

bool parse(const char* json, AppStoreManifestMetaData& out);
std::string serialize(const AppStoreManifestMetaData& meta);

}  // namespace AppStoreManifestMetaJson
