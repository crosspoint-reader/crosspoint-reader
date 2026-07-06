#pragma once

#include <string>

struct AppInstallRequest {
  std::string id;
  std::string name;
  std::string version;
  // Empty uses current UTC time via AppDateTimeFormat::formatNowIso8601Utc().
  std::string installedAt;
};

// Extracts a .cpapp zip on SD into /.crosspoint/apps/<id>/ and updates registry.json.
class AppBundleInstaller {
 public:
  static bool installFromZipFile(const char* zipPath, const AppInstallRequest& request);
};
