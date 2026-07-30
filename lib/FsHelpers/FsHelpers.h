#pragma once
#include <WString.h>

#include <cstdint>

class HalFile;

#include <string>
#include <string_view>
#include <vector>

namespace FsHelpers {

std::string decodeUriEscapes(const std::string& path);

std::string normalisePath(const std::string& path);

// Numeric-aware, case-insensitive comparison ("2" < "10"). Returns true when str1 orders
// before str2. Same ordering sortFileList applies within the file/directory groups.
bool naturalLess(const std::string& str1, const std::string& str2);

void sortFileList(std::vector<std::string>& strs);

/**
 * Check if the given filename ends with the specified extension (case-insensitive).
 */
bool checkFileExtension(std::string_view fileName, const char* extension);
inline bool checkFileExtension(const String& fileName, const char* extension) {
  return checkFileExtension(std::string_view{fileName.c_str(), fileName.length()}, extension);
}

// Check for either .jpg or .jpeg extension (case-insensitive)
bool hasJpgExtension(std::string_view fileName);
inline bool hasJpgExtension(const String& fileName) {
  return hasJpgExtension(std::string_view{fileName.c_str(), fileName.length()});
}

// Check for .png extension (case-insensitive)
bool hasPngExtension(std::string_view fileName);
inline bool hasPngExtension(const String& fileName) {
  return hasPngExtension(std::string_view{fileName.c_str(), fileName.length()});
}

// Check for .bmp extension (case-insensitive)
bool hasBmpExtension(std::string_view fileName);

// Check for .gif extension (case-insensitive)
bool hasGifExtension(std::string_view fileName);
inline bool hasGifExtension(const String& fileName) {
  return hasGifExtension(std::string_view{fileName.c_str(), fileName.length()});
}

// Check for .epub extension (case-insensitive)
bool hasEpubExtension(std::string_view fileName);
inline bool hasEpubExtension(const String& fileName) {
  return hasEpubExtension(std::string_view{fileName.c_str(), fileName.length()});
}

// Check for either .xtc or .xtch extension (case-insensitive)
bool hasXtcExtension(std::string_view fileName);

// Check for .txt extension (case-insensitive)
bool hasTxtExtension(std::string_view fileName);
inline bool hasTxtExtension(const String& fileName) {
  return hasTxtExtension(std::string_view{fileName.c_str(), fileName.length()});
}

// Check for .md extension (case-insensitive)
bool hasMarkdownExtension(std::string_view fileName);

// Check for a playable audio extension: .mp3 .wav .flac .m4a .aac .ogg .opus (case-insensitive)
bool hasAudioExtension(std::string_view fileName);
inline bool hasAudioExtension(const String& fileName) {
  return hasAudioExtension(std::string_view{fileName.c_str(), fileName.length()});
}

// Check for .mobi or .azw/.azw3 extension (case-insensitive)
bool hasMobiExtension(std::string_view fileName);
inline bool hasMobiExtension(const String& fileName) {
  return hasMobiExtension(std::string_view{fileName.c_str(), fileName.length()});
}

// Check for .pdf extension (case-insensitive)
bool hasPdfExtension(std::string_view fileName);
inline bool hasPdfExtension(const String& fileName) {
  return hasPdfExtension(std::string_view{fileName.c_str(), fileName.length()});
}

// Check for .css extension (case-insensitive)
bool hasCssExtension(std::string_view fileName);
inline bool hasCssExtension(const String& fileName) {
  return hasCssExtension(std::string_view{fileName.c_str(), fileName.length()});
}
std::string extractFolderPath(const std::string& filePath);

/**
 * Cheap content identity for validating a converted-book cache.
 *
 * CRC32 over 32 evenly-spaced 8 KB windows plus the size, so the cost is
 * constant (at most ~256 KB read) no matter how large the source is -- reading a
 * 50 MB PDF in full on every cached open would cost seconds on SD.
 *
 * This is a cache-invalidation heuristic, not a checksum. It reliably separates
 * one book from another (two different files differ in far more than the sampled
 * bytes, and files under ~256 KB are covered end to end), but a change confined
 * to unsampled bytes of a large file can be missed. Callers pair it with the
 * file size and the converter version.
 */
uint32_t sourceFingerprint(HalFile& file, uint32_t size);

/**
 * Sanitize a filename/path component for FAT32 in a caller-provided buffer.
 * Replaces invalid path characters, spaces, and control characters with '-'.
 */
void sanitizePathComponentForFat32(const char* input, char* output, size_t maxLen);

}  // namespace FsHelpers
