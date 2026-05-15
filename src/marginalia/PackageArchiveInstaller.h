#pragma once

#include <string>

namespace Marginalia {

struct PackageArchiveInstallResult {
  bool ok = false;
  std::string packageId;
  std::string packageName;
  std::string error;
};

PackageArchiveInstallResult extractPackageArchiveToInbox(const std::string& archivePath);

}  // namespace Marginalia
