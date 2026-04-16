#pragma once

#include <cstdlib>
#include <string>

namespace ota_version {

struct Semver {
  int major = 0;
  int minor = 0;
  int patch = 0;
};

inline bool parseSemverPrefix(const char* version, Semver& out) {
  if (version == nullptr) {
    return false;
  }
  if (*version == 'v') {
    ++version;
  }

  char* end = nullptr;
  const long major = std::strtol(version, &end, 10);
  if (end == version || *end != '.') {
    return false;
  }
  version = end + 1;

  const long minor = std::strtol(version, &end, 10);
  if (end == version || *end != '.') {
    return false;
  }
  version = end + 1;

  const long patch = std::strtol(version, &end, 10);
  if (end == version) {
    return false;
  }

  out.major = static_cast<int>(major);
  out.minor = static_cast<int>(minor);
  out.patch = static_cast<int>(patch);
  return true;
}

inline bool isNewer(const std::string& latestVersion, const char* currentVersion) {
  if (latestVersion.empty() || currentVersion == nullptr || latestVersion == currentVersion) {
    return false;
  }

  Semver latest;
  Semver current;
  const bool latestParsed = parseSemverPrefix(latestVersion.c_str(), latest);
  const bool currentParsed = parseSemverPrefix(currentVersion, current);
  if (!latestParsed || !currentParsed) {
    return true;
  }

  if (latest.major != current.major) return latest.major > current.major;
  if (latest.minor != current.minor) return latest.minor > current.minor;
  if (latest.patch != current.patch) return latest.patch > current.patch;

  if (std::string(currentVersion).find("-rc") != std::string::npos) {
    return true;
  }

  return latestVersion != currentVersion && !latestVersion.empty() && latestVersion[0] == 'v';
}

}  // namespace ota_version
