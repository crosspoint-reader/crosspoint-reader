#include "FsHelpers.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <vector>

namespace FsHelpers {

std::string normalisePath(const std::string& path) {
  std::vector<std::string> components;
  std::string component;

  for (const auto c : path) {
    if (c == '/') {
      if (!component.empty()) {
        if (component == "..") {
          if (!components.empty()) {
            components.pop_back();
          }
        } else {
          components.push_back(component);
        }
        component.clear();
      }
    } else {
      component += c;
    }
  }

  if (!component.empty()) {
    components.push_back(component);
  }

  std::string result;
  for (const auto& c : components) {
    if (!result.empty()) {
      result += "/";
    }
    result += c;
  }

  return result;
}

void sortFileList(std::vector<std::string>& strs) {
  std::sort(begin(strs), end(strs), [](const std::string& str1, const std::string& str2) {
    // Directories first
    bool isDir1 = str1.back() == '/';
    bool isDir2 = str2.back() == '/';
    if (isDir1 != isDir2) return isDir1;

    // Start naive natural sort
    const char* s1 = str1.c_str();
    const char* s2 = str2.c_str();

    // Iterate while both strings have characters
    while (*s1 && *s2) {
      // Check if both are at the start of a number
      if (isdigit(*s1) && isdigit(*s2)) {
        // Skip leading zeros and track them
        const char* start1 = s1;
        const char* start2 = s2;
        while (*s1 == '0') s1++;
        while (*s2 == '0') s2++;

        // Count digits to compare lengths first
        int len1 = 0, len2 = 0;
        while (isdigit(s1[len1])) len1++;
        while (isdigit(s2[len2])) len2++;

        // Different length so return smaller integer value
        if (len1 != len2) return len1 < len2;

        // Same length so compare digit by digit
        for (int i = 0; i < len1; i++) {
          if (s1[i] != s2[i]) return s1[i] < s2[i];
        }

        // Numbers equal so advance pointers
        s1 += len1;
        s2 += len2;
      } else {
        // Regular case-insensitive character comparison
        char c1 = tolower(*s1);
        char c2 = tolower(*s2);
        if (c1 != c2) return c1 < c2;
        s1++;
        s2++;
      }
    }

    // One string is prefix of other
    return *s1 == '\0' && *s2 != '\0';
  });
}

bool checkFileExtension(std::string_view fileName, const char* extension) {
  const size_t extLen = strlen(extension);
  if (fileName.length() < extLen) {
    return false;
  }

  const size_t offset = fileName.length() - extLen;
  for (size_t i = 0; i < extLen; i++) {
    if (tolower(static_cast<unsigned char>(fileName[offset + i])) !=
        tolower(static_cast<unsigned char>(extension[i]))) {
      return false;
    }
  }
  return true;
}

bool hasJpgExtension(std::string_view fileName) {
  return checkFileExtension(fileName, ".jpg") || checkFileExtension(fileName, ".jpeg");
}

bool hasPngExtension(std::string_view fileName) { return checkFileExtension(fileName, ".png"); }

bool hasBmpExtension(std::string_view fileName) { return checkFileExtension(fileName, ".bmp"); }

bool hasGifExtension(std::string_view fileName) { return checkFileExtension(fileName, ".gif"); }

bool hasEpubExtension(std::string_view fileName) { return checkFileExtension(fileName, ".epub"); }

bool hasXtcExtension(std::string_view fileName) {
  return checkFileExtension(fileName, ".xtc") || checkFileExtension(fileName, ".xtch");
}

bool hasTxtExtension(std::string_view fileName) { return checkFileExtension(fileName, ".txt"); }

bool hasMarkdownExtension(std::string_view fileName) { return checkFileExtension(fileName, ".md"); }

std::string extractFolderPath(const std::string& filePath) {
  const auto lastSlash = filePath.find_last_of('/');
  if (lastSlash == std::string::npos || lastSlash == 0) {
    return "/";
  }
  return filePath.substr(0, lastSlash);
}

void sanitizePathComponentForFat32(const char* input, char* output, size_t maxLen) {
  if (maxLen == 0) {
    return;
  }

  size_t i = 0;
  for (; i < maxLen - 1 && input[i] != '\0'; i++) {
    const char c = input[i];
    if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' ||
        c == ' ' || (c > 0x00 && c <= 0x1f)) {
      output[i] = '-';
    } else {
      output[i] = c;
    }
  }
  output[i] = '\0';
}

}  // namespace FsHelpers
