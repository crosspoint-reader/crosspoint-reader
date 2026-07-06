#pragma once

namespace AppStoreManifestData {

constexpr char kRemoteUrl[] = "https://crosspoint-app-api.vercel.app/v1/manifest.json";

#if !__has_include("AppStoreManifestBuiltin.generated.h")
// Flash-resident catalog used when remote fetch and SD cache are unavailable.
constexpr char kBuiltinJson[] = R"({
  "version": 1,
  "apps": [
    {
      "id": "hello",
      "name": "Hello App",
      "version": "1.0.0",
      "description": "Smoke-test sample for the Lua runtime",
      "min_api_version": "1.0"
    }
  ]
})";
#endif

}  // namespace AppStoreManifestData

#if __has_include("AppStoreManifestBuiltin.generated.h")
#include "AppStoreManifestBuiltin.generated.h"
#endif
