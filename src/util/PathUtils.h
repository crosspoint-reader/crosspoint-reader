#pragma once

#include <Arduino.h>

#include <cstddef>

/**
 * Path validation utilities for SD card file operations.
 * Protects against path traversal attacks and normalizes paths.
 */
namespace PathUtils {

/**
 * Check if a path contains directory traversal attempts.
 * Detects patterns like "..", "/..", "../", URL-encoded variants, etc.
 *
 * @param path The path to check
 * @return true if path contains traversal attempts (UNSAFE)
 */
bool containsTraversal(const String& path);
bool containsTraversal(const char* path);

/**
 * Validate that a path is safe for SD card operations.
 * - No traversal attempts
 * - No null bytes
 * - Reasonable length
 *
 * @param path The path to validate
 * @return true if path is valid and safe
 */
bool isValidSdPath(const String& path);
bool isValidSdPath(const char* path);

/**
 * Normalize a path for consistent handling.
 * - Ensures leading /
 * - Removes trailing / (except for root)
 * - Collapses multiple consecutive slashes
 *
 * @param path The path to normalize
 * @return Normalized path (empty input becomes "/")
 */
String normalizePath(const String& path);
bool normalizePathInPlace(char* path, size_t pathSize);

/**
 * Decode URL-encoded path fragments (e.g. %2F, %20).
 * Converts '+' to space.
 *
 * @param path The URL-encoded path to decode
 * @return Decoded path
 */
String urlDecode(const String& path);
bool urlDecode(const char* path, char* out, size_t outSize);

/**
 * Validate a filename (no path separators or traversal).
 * Used for uploaded filenames before combining with destination path.
 *
 * @param filename The filename to validate
 * @return true if filename is valid
 */
bool isValidFilename(const String& filename);
bool isValidFilename(const char* filename);

/**
 * Check if a single path component (filename or directory name) is a
 * protected/hidden web item. Protects dotfiles and known system folders
 * (System Volume Information, XTCache).
 *
 * @param component A single path component (no slashes)
 * @return true if the component should be blocked from web access
 */
bool isProtectedWebComponent(const String& component);
bool isProtectedWebComponent(const char* component);

/**
 * Check if any component of a path is a protected web item.
 * Splits the normalized path on '/' and tests each component.
 * Used by web handlers to gate access to hidden/system items.
 *
 * @param path An absolute SD path (will be normalized internally)
 * @return true if any component is protected
 */
bool pathContainsProtectedItem(const String& path);
bool pathContainsProtectedItem(const char* path);

}  // namespace PathUtils
