#pragma once

#include <string>

namespace ota_test {
inline std::string currentVersion = "1.6.0";
inline std::string releaseJson;
inline unsigned flashBegins = 0;
inline size_t chunkSize = 7;
}  // namespace ota_test

#define CROSSPOINT_VERSION ota_test::currentVersion.c_str()
