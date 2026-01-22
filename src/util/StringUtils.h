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

/**
 * Check if the given filename ends with an extension we can open.
 */
bool readableFileExtension(const std::string& fileName);

/**
 * Split a filename into base name and extension.
 * If there is no extension, the second element of the pair will be an empty string.
 */
std::pair<std::string, std::string> splitFileName(const std::string& name);

// UTF-8 safe string truncation - removes one character from the end
// Returns the new size after removing one UTF-8 character
size_t utf8RemoveLastChar(std::string& str);

// Truncate string by removing N UTF-8 characters from the end
void utf8TruncateChars(std::string& str, size_t numChars);
}  // namespace StringUtils
