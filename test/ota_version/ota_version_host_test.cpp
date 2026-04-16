#include <cstdlib>
#include <iostream>

#include "network/OtaVersion.h"

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << "\n";
    std::exit(1);
  }
}

}  // namespace

int main() {
  require(
      !ota_version::isNewer("v2026.04.16-reduce-pdf-object-buffer-usage", "v2026.04.16-reduce-pdf-object-buffer-usage"),
      "matching date tags are not newer");
  require(ota_version::isNewer("v2026.04.16-reduce-pdf-object-buffer-usage", "1.1.1"),
          "date tag is newer than installed semver build");
  require(ota_version::isNewer("v2026.04.17-next", "v2026.04.16-reduce-pdf-object-buffer-usage"),
          "later date tag is newer");
  require(ota_version::isNewer("v2026.04.16-actions", "v2026.04.16-reduce-pdf-object-buffer-usage"),
          "different same-day date tag is newer");

  require(ota_version::isNewer("1.2.0", "1.1.1"), "higher semver is newer");
  require(!ota_version::isNewer("1.1.0", "1.1.1"), "lower semver is not newer");
  require(ota_version::isNewer("1.1.1", "1.1.1-rc+abc1234"), "release is newer than matching rc");
  require(!ota_version::isNewer("1.1.1", "1.1.1"), "matching semver is not newer");

  return 0;
}
