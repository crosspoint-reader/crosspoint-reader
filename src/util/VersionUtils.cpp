#include "VersionUtils.h"

constexpr char RC_TAG_PREFIX[] = "-rc";

bool parse(const std::string& str, Version& version) {
    const char* p = str.c_str();

    // In version 1.5.0, we switched to using a "v" prefix for the version string.
    if (*p == 'v' || *p == 'V') p++;

    // The %n specified stores how many characters sscanf read.
    int offset = 0;
    if (sscanf(p, "%d.%d.%d%n", &version.major, &version.minor, &version.patch, &offset) != 3) {
        return false;
    }

    p += offset;

    if (*p == '\0') {
        return true;
    }
    if (std::strncmp(p, RC_TAG_PREFIX, sizeof(RC_TAG_PREFIX) - 1) != 0) {
        return false;
    }
    
    version.prerelease = true;
    
    return true;
}