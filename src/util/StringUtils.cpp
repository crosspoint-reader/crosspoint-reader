#include "StringUtils.h"

#include <Utf8.h>

#include <cstring>

namespace StringUtils {

std::string sanitizeFilename(const std::string& name, size_t maxBytes) {
  std::string result;
  result.reserve(std::min(name.size(), maxBytes));

  const auto* text = reinterpret_cast<const unsigned char*>(name.c_str());

  // Skip leading spaces and dots so they don't consume the byte budget
  while (*text == ' ' || *text == '.') {
    text++;
  }

  // Process full UTF-8 codepoints to avoid trimming in the middle of a multibyte sequence
  while (*text != 0) {
    const auto* cpStart = text;
    uint32_t cp = utf8NextCodepoint(&text);

    if (cp == '/' || cp == '\\' || cp == ':' || cp == '*' || cp == '?' || cp == '"' || cp == '<' || cp == '>' ||
        cp == '|') {
      // Replace illegal and control characters with '_'
      if (result.length() + 1 > maxBytes) break;
      result += '_';
    } else if (cp >= 128 || (cp >= 32 && cp < 127)) {
      const size_t cpBytes = text - cpStart;
      if (result.length() + cpBytes > maxBytes) break;
      result.append(reinterpret_cast<const char*>(cpStart), cpBytes);
    }
  }

  // Trim trailing spaces and dots
  size_t end = result.find_last_not_of(" .");
  if (end != std::string::npos) {
    result.resize(end + 1);
  } else {
    result.clear();
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

}  // namespace StringUtils
