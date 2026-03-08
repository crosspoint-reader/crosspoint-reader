#include "FsHelpers.h"

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

const char* getMimeType(std::string_view fileName) {
  if (hasEpubExtension(fileName)) return "application/epub+zip";
  if (checkFileExtension(fileName, ".pdf")) return "application/pdf";
  if (hasTxtExtension(fileName) || checkFileExtension(fileName, ".log")) return "text/plain";
  if (hasMarkdownExtension(fileName)) return "text/markdown";
  if (checkFileExtension(fileName, ".html") || checkFileExtension(fileName, ".htm")) return "text/html";
  if (checkFileExtension(fileName, ".css")) return "text/css";
  if (checkFileExtension(fileName, ".js")) return "application/javascript";
  if (checkFileExtension(fileName, ".json")) return "application/json";
  if (checkFileExtension(fileName, ".xml")) return "application/xml";
  if (hasJpgExtension(fileName)) return "image/jpeg";
  if (hasPngExtension(fileName)) return "image/png";
  if (checkFileExtension(fileName, ".gif")) return "image/gif";
  if (checkFileExtension(fileName, ".svg")) return "image/svg+xml";
  if (checkFileExtension(fileName, ".zip")) return "application/zip";
  if (checkFileExtension(fileName, ".gz")) return "application/gzip";
  return "application/octet-stream";
}

}  // namespace FsHelpers
