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

// Check for either .jpg or .jpeg extension (case-insensitive)
bool hasJpgExtension(std::string_view fileName);

// Check for .png extension (case-insensitive)
bool hasPngExtension(std::string_view fileName);

// Check for .bmp extension (case-insensitive)
bool hasBmpExtension(std::string_view fileName);

// Check for .gif extension (case-insensitive)
bool hasGifExtension(std::string_view fileName);

}  // namespace FsHelpers
