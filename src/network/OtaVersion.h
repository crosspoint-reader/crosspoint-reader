#pragma once

#include <string_view>

namespace ota_version {

// Compare SemVer versions with an optional leading 'v', without allocation.
// The current firmware may also use git_branch.py's -dev-<branch>-<sha>
// suffix; its opaque branch/hash suffix is treated as the prerelease "dev".
// Invalid versions never authorize an update.
bool isNewer(std::string_view latest, std::string_view current);

}  // namespace ota_version
