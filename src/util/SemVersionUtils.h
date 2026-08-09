#pragma once

#include <string>

struct SemVersion {
  int major = 0;
  int minor = 1;
  int patch = 0;
  bool prerelease = false;
};

bool parse(const std::string& str, SemVersion& version);