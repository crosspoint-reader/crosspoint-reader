#pragma once

#include <cstddef>
#include <string>

namespace Marginalia {

struct PackageDownloadInstallResult {
  bool ok = false;
  std::string packageId;
  std::string packageName;
  std::string error;
};

PackageDownloadInstallResult downloadPackageArchiveToInbox(const std::string& url, const std::string& expectedSha256,
                                                           size_t expectedSize);

}  // namespace Marginalia
