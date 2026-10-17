#pragma once

#include <string>
#include <string_view>

namespace FsHelpers {
inline bool checkFileExtension(std::string_view path, const char* extension) { return path.ends_with(extension); }
inline bool hasEpubExtension(const std::string& path) { return checkFileExtension(path, ".epub"); }
}  // namespace FsHelpers
