#pragma once

#include <string>

namespace StringUtils {

/**
 * Sanitize a string for use as a filename.
 * Replaces invalid characters with underscores, trims spaces/dots,
 * and limits length to maxBytes bytes.
 */
std::string sanitizeFilename(const std::string& name, size_t maxBytes = 100);

/**
 * Normalize an SD folder path to the OPDS download-folder invariant.
 * Trims whitespace, strips trailing slashes; maps ""/"/" to "" (root sentinel);
 * otherwise guarantees a single leading '/' and no trailing '/'.
 */
std::string normalizeFolderPath(const std::string& raw);

}  // namespace StringUtils
