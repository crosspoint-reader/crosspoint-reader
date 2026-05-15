#pragma once

#include <string>
#include <vector>

namespace Marginalia {

constexpr const char* PACKAGE_ROOT = "/.marginalia/packages";
constexpr size_t MAX_MANIFEST_BYTES = 16384;

struct PackageManifest {
  std::string id;
  std::string name;
  std::string version;
  std::string kind;
  std::string execution;
  std::string summary;
  std::string author;
  std::string manifestPath;
  bool valid = false;
  std::string error;
};

class PackageStore {
 public:
  void scan();
  const std::vector<PackageManifest>& packages() const { return packages_; }
  bool hadScanError() const { return hadScanError_; }

 private:
  std::vector<PackageManifest> packages_;
  bool hadScanError_ = false;

  PackageManifest readManifest(const std::string& packageDir, const std::string& packageDirName) const;
};

}  // namespace Marginalia
