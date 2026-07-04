#pragma once

namespace AppStorePaths {

constexpr char kAppsRoot[] = "/.crosspoint/apps";
constexpr char kRegistryFile[] = "/.crosspoint/apps/registry.json";
constexpr char kRegistryTmpFile[] = "/.crosspoint/apps/registry.json.tmp";
constexpr char kTmpDir[] = "/.crosspoint/apps/_tmp";
constexpr char kCacheDir[] = "/.crosspoint/apps/_cache";
constexpr char kManifestCacheFile[] = "/.crosspoint/apps/_cache/manifest.json";
constexpr char kManifestMetaFile[] = "/.crosspoint/apps/_cache/manifest_meta.json";
constexpr char kManifestEtagFile[] = "/.crosspoint/apps/_cache/manifest_etag.txt";

inline constexpr char kDataSubdir[] = "data";

}  // namespace AppStorePaths
