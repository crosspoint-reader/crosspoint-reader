#pragma once

#include <string>
#include <vector>

namespace Marginalia {

constexpr const char* PACKAGE_ROOT = "/.marginalia/packages";
constexpr const char* PACKAGE_INBOX_ROOT = "/.marginalia/inbox";
constexpr const char* PACKAGE_STAGING_ROOT = "/.marginalia/staging";
constexpr const char* PACKAGE_STATE_ROOT = "/.marginalia/package-state";
constexpr size_t MAX_MANIFEST_BYTES = 16384;
constexpr int PACKAGE_API_LEVEL = 1;

enum class PackageSettingType {
  Boolean,
  Enum,
};

struct PackageSettingDefinition {
  std::string id;
  std::string label;
  PackageSettingType type = PackageSettingType::Boolean;
  bool defaultBool = false;
  std::string defaultString;
  std::vector<std::string> options;
};

struct PackageManifest {
  std::string id;
  std::string directoryName;
  std::string name;
  std::string version;
  std::string kind;
  std::string execution;
  std::string summary;
  std::string author;
  std::string manifestPath;
  std::vector<std::string> permissions;
  std::vector<PackageSettingDefinition> settings;
  bool enabled = true;
  bool compatible = true;
  std::string compatibilityError;
  bool valid = false;
  std::string error;
};

class PackageStore {
 public:
  void scan();
  void scanInbox();
  const std::vector<PackageManifest>& packages() const { return packages_; }
  bool hadScanError() const { return hadScanError_; }
  PackageManifest readManifest(const std::string& packageDir, const std::string& packageDirName) const;

 private:
  std::vector<PackageManifest> packages_;
  bool hadScanError_ = false;

  void scanRoot(const char* rootPath);
};

bool isSafePackageId(const std::string& value);
bool isSafePackageRelativePath(const std::string& value);
bool ensurePackageBaseDirectories();
bool readPackageEnabled(const std::string& packageId);
bool setPackageEnabled(const std::string& packageId, bool enabled);
bool readPackageSettingBool(const std::string& packageId, const std::string& settingId, bool defaultValue);
std::string readPackageSettingString(const std::string& packageId, const std::string& settingId,
                                     const std::string& defaultValue);
bool writePackageSettingBool(const std::string& packageId, const std::string& settingId, bool value);
bool writePackageSettingString(const std::string& packageId, const std::string& settingId, const std::string& value);
bool uninstallPackage(const std::string& packageId);

}  // namespace Marginalia
