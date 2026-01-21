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

size_t utf8RemoveLastChar(std::string& str) {
  if (str.empty()) return 0;
  size_t pos = str.size() - 1;
  // Walk back to find the start of the last UTF-8 character
  // UTF-8 continuation bytes start with 10xxxxxx (0x80-0xBF)
  while (pos > 0 && (static_cast<unsigned char>(str[pos]) & 0xC0) == 0x80) {
    --pos;
  }
  str.resize(pos);
  return pos;
}

// Truncate string by removing N UTF-8 characters from the end
void utf8TruncateChars(std::string& str, const size_t numChars) {
  for (size_t i = 0; i < numChars && !str.empty(); ++i) {
    utf8RemoveLastChar(str);
  }
}

}  // namespace StringUtils
