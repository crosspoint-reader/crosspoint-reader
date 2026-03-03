#pragma once
#include <WString.h>

#include <string>
#include <string_view>

namespace FsHelpers {

std::string normalisePath(const std::string& path);

/**
 * Check if the given filename ends with the specified extension (case-insensitive).
 */
bool checkFileExtension(std::string_view fileName, const char* extension);
bool checkFileExtension(const String& fileName, const char* extension);

}  // namespace FsHelpers
