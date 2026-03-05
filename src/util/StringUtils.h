#pragma once

#include <WString.h>
#include <string>

namespace StringUtils {

/**
 * Sanitize a string for use as a filename.
 * Replaces invalid characters with underscores, trims spaces/dots,
 * and limits length to maxBytes bytes.
 */
std::string sanitizeFilename(const std::string& name, size_t maxBytes = 100);

/**
 * Format a series label from series name and index.
 * Returns empty string when series is empty.
 * Trims trailing ".0" from index (e.g. "1.0" → "1").
 * Returns "Series Name" or "Series Name #1".
 */
std::string formatSeriesLabel(const std::string& series, const std::string& seriesIndex);

/**
 * Check if the given filename ends with the specified extension (case-insensitive).
 */
bool checkFileExtension(const std::string& fileName, const char* extension);
bool checkFileExtension(const String& fileName, const char* extension);
}  // namespace StringUtils
