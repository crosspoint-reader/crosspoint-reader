#include "SemVersionUtils.h"

#include <string.h>

constexpr char RC_TAG_PREFIX[] = "-rc";

bool parse(const std::string& str, SemVersion& version) {
  const char* p = str.c_str();

  int offset = 0;
  if (sscanf(p, "%d.%d.%d%n", &version.major, &version.minor, &version.patch, &offset) != 3) {
    return false;
  }
  p += offset;

  if (*p == '\0') {
    return true;
  }

  if (strncmp(p, RC_TAG_PREFIX, sizeof(RC_TAG_PREFIX) - 1) != 0) {
    return false;
  }
  version.prerelease = true;

  return true;
}