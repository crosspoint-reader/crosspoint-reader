#pragma once

#include <WString.h>

#include <string>

namespace StringUtils {

/**
 * Sanitize a string for use as a filename.
 * Replaces invalid characters with underscores, trims spaces/dots,
 * and limits length to maxLength characters.
 */
std::string sanitizeFilename(const std::string& name, size_t maxLength = 100);

/**
 * Check if the given filename ends with the specified extension (case-insensitive).
 */
bool checkFileExtension(const std::string& fileName, const char* extension);
bool checkFileExtension(const String& fileName, const char* extension);

}  // namespace StringUtils
