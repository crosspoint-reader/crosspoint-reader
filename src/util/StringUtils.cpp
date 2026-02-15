#include "StringUtils.h"

#include <cstring>

namespace StringUtils {

std::string sanitizeFilename(const std::string& name, size_t maxLength) {
  std::string result;
  result.reserve(name.size());

  for (char c : name) {
    // Replace invalid filename characters with underscore
    if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
      result += '_';
    } else if (c >= 32 && c < 127) {
      // Keep printable ASCII characters
      result += c;
    }
    // Skip non-printable characters
  }

  // Trim leading/trailing spaces and dots
  size_t start = result.find_first_not_of(" .");
  if (start == std::string::npos) {
    return "book";  // Fallback if name is all invalid characters
  }
  size_t end = result.find_last_not_of(" .");
  result = result.substr(start, end - start + 1);

  // Limit filename length
  if (result.length() > maxLength) {
    result.resize(maxLength);
  }

  return result.empty() ? "book" : result;
}

bool checkFileExtension(const std::string& fileName, const char* extension) {
  if (fileName.length() < strlen(extension)) {
    return false;
  }

  const std::string fileExt = fileName.substr(fileName.length() - strlen(extension));
  for (size_t i = 0; i < fileExt.length(); i++) {
    if (tolower(fileExt[i]) != tolower(extension[i])) {
      return false;
    }
  }
  return true;
}

bool checkFileExtension(const String& fileName, const char* extension) {
  if (fileName.length() < strlen(extension)) {
    return false;
  }

  String localFile(fileName);
  String localExtension(extension);
  localFile.toLowerCase();
  localExtension.toLowerCase();
  return localFile.endsWith(localExtension);
}

std::string urlDecode(const std::string& encoded) {
  std::string decoded;
  decoded.reserve(encoded.size());
  for (size_t i = 0; i < encoded.size(); i++) {
    if (encoded[i] == '%' && i + 2 < encoded.size()) {
      auto hexVal = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
      };
      int hi = hexVal(encoded[i + 1]);
      int lo = hexVal(encoded[i + 2]);
      if (hi >= 0 && lo >= 0) {
        decoded += static_cast<char>((hi << 4) | lo);
        i += 2;
        continue;
      }
    }
    decoded += (encoded[i] == '+') ? ' ' : encoded[i];
  }
  return decoded;
}

}  // namespace StringUtils
