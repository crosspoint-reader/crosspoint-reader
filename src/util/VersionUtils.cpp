#include "VersionUtils.h"

#include <string.h>

constexpr char PRERELEASE_TAG_PREFIX[] = "-rc";

bool parse(const std::string& str, Version& version) {
  const char* p = str.c_str();

  // Around version 1.5.0, we switched to using a "v" prefix for the version string. Ideally,
  // we should follow the semantic versioning spec and not use a prefix, but we have to support
  // both formats for now.
  if (*p == 'v' || *p == 'V') p++;
  if (!isdigit(*p)) {
    return false;
  }

  int offset = 0;
  if (sscanf(p, "%d.%d.%d%n", &version.major, &version.minor, &version.patch, &offset) != 3) {
    return false;
  }
  if (version.major < 0 || version.minor < 0 || version.patch < 0) {
    return false;
  }
  p += offset;

  if (*p == '\0') {
    return true;
  }

  if (strncmp(p, PRERELEASE_TAG_PREFIX, sizeof(PRERELEASE_TAG_PREFIX) - 1) != 0) {
    return false;
  }
  version.prerelease = true;

  return true;
}~