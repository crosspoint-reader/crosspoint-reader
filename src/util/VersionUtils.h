#pragma once

#include <string>

struct Version {
    int major = 0;
    int minor = 1;
    int patch = 0;
    bool prerelease = false;
};

bool parse(const std::string& str, Version& version);