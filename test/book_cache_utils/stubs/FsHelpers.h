#pragma once

#include <string>

namespace FsHelpers {
inline bool hasEpubExtension(const std::string&) { return false; }
inline bool hasTxtExtension(const std::string&) { return false; }
inline bool hasXtcExtension(const std::string&) { return false; }
}  // namespace FsHelpers
