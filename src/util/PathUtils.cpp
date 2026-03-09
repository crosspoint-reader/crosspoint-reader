#include "PathUtils.h"

#include <Logging.h>

#include <cctype>
#include <cstring>

#include "InputValidation.h"

namespace PathUtils {
namespace {

int hexValue(const char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
  if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
  return -1;
}

bool equalsIgnoreCase(const char* lhs, const char* rhs) {
  if (lhs == nullptr || rhs == nullptr) {
    return false;
  }

  while (*lhs != '\0' && *rhs != '\0') {
    if (std::tolower(static_cast<unsigned char>(*lhs)) != std::tolower(static_cast<unsigned char>(*rhs))) {
      return false;
    }
    lhs++;
    rhs++;
  }

  return *lhs == '\0' && *rhs == '\0';
}

bool equalsIgnoreCaseN(const char* lhs, const size_t lhsLen, const char* rhs) {
  if (lhs == nullptr || rhs == nullptr) {
    return false;
  }

  const size_t rhsLen = std::strlen(rhs);
  if (lhsLen != rhsLen) {
    return false;
  }

  for (size_t i = 0; i < lhsLen; i++) {
    if (std::tolower(static_cast<unsigned char>(lhs[i])) != std::tolower(static_cast<unsigned char>(rhs[i]))) {
      return false;
    }
  }

  return true;
}

bool containsIgnoreCase(const char* haystack, const char* needle) {
  if (haystack == nullptr || needle == nullptr || needle[0] == '\0') {
    return false;
  }

  const size_t needleLen = std::strlen(needle);
  const size_t haystackLen = std::strlen(haystack);
  if (needleLen > haystackLen) {
    return false;
  }

  for (size_t start = 0; start + needleLen <= haystackLen; start++) {
    size_t matched = 0;
    while (matched < needleLen && std::tolower(static_cast<unsigned char>(haystack[start + matched])) ==
                                      std::tolower(static_cast<unsigned char>(needle[matched]))) {
      matched++;
    }
    if (matched == needleLen) {
      return true;
    }
  }

  return false;
}

// Folders and files to hide from the web interface file browser.
// Items starting with "." are handled separately in isProtectedWebComponent.
constexpr const char* kHiddenWebItems[] = {"System Volume Information", "XTCache"};
constexpr size_t kHiddenWebItemsCount = sizeof(kHiddenWebItems) / sizeof(kHiddenWebItems[0]);

}  // namespace

bool containsTraversal(const String& path) { return containsTraversal(path.c_str()); }

bool containsTraversal(const char* path) {
  if (path == nullptr) {
    LOG_WRN("PATH", "traversal: null");
    return true;
  }

  const size_t length = std::strlen(path);
  if (std::strstr(path, "/../") != nullptr) {
    LOG_WRN("PATH", "traversal: /../");
    return true;
  }
  if (length == 2 && path[0] == '.' && path[1] == '.') {
    LOG_WRN("PATH", "traversal: equals ..");
    return true;
  }
  if (length >= 3 && path[0] == '.' && path[1] == '.' && path[2] == '/') {
    LOG_WRN("PATH", "traversal: starts ../");
    return true;
  }
  if (length >= 3 && std::strcmp(path + length - 3, "/..") == 0) {
    LOG_WRN("PATH", "traversal: ends /..");
    return true;
  }

  if (containsIgnoreCase(path, "%2e%2e%2f")) {
    LOG_WRN("PATH", "traversal: %%2e%%2e%%2f");
    return true;
  }
  if (containsIgnoreCase(path, "%2f%2e%2e")) {
    LOG_WRN("PATH", "traversal: %%2f%%2e%%2e");
    return true;
  }
  if (containsIgnoreCase(path, "..%2f")) {
    LOG_WRN("PATH", "traversal: ..%%2f");
    return true;
  }
  if (containsIgnoreCase(path, "%2f..")) {
    LOG_WRN("PATH", "traversal: %%2f..");
    return true;
  }

  return false;
}

bool isValidSdPath(const String& path) { return isValidSdPath(path.c_str()); }

bool isValidSdPath(const char* path) {
  if (path == nullptr || path[0] == '\0') {
    LOG_WRN("PATH", "REJECT: empty path");
    return false;
  }

  const size_t length = std::strlen(path);
  if (length > 255) {
    LOG_WRN("PATH", "REJECT: path too long (%d chars)", static_cast<int>(length));
    return false;
  }

  if (containsTraversal(path)) {
    LOG_WRN("PATH", "REJECT: traversal in '%s'", path);
    return false;
  }

  size_t invalidPos = 0;
  if (InputValidation::findAsciiControlChar(path, length, invalidPos)) {
    LOG_WRN("PATH", "REJECT: control char at %d", static_cast<int>(invalidPos));
    return false;
  }

  for (size_t i = 0; i < length; i++) {
    if (path[i] == '\\') {
      LOG_WRN("PATH", "REJECT: backslash at %d", static_cast<int>(i));
      return false;
    }
  }

  return true;
}

String normalizePath(const String& path) {
  if (path.isEmpty()) {
    return "/";
  }

  String result = path;

  if (!result.startsWith("/")) {
    result = "/" + result;
  }

  while (result.indexOf("//") >= 0) {
    result.replace("//", "/");
  }

  if (result.length() > 1 && result.endsWith("/")) {
    result = result.substring(0, result.length() - 1);
  }

  return result;
}

bool normalizePathInPlace(char* path, const size_t pathSize) {
  if (path == nullptr || pathSize < 2) {
    return false;
  }

  size_t length = std::strlen(path);
  if (length == 0) {
    path[0] = '/';
    path[1] = '\0';
    return true;
  }

  if (path[0] != '/') {
    if (length + 1 >= pathSize) {
      return false;
    }
    std::memmove(path + 1, path, length + 1);
    path[0] = '/';
    length++;
  }

  size_t write = 0;
  bool previousWasSlash = false;
  for (size_t read = 0; read < length; read++) {
    const char c = path[read];
    if (c == '/') {
      if (previousWasSlash) {
        continue;
      }
      previousWasSlash = true;
    } else {
      previousWasSlash = false;
    }
    path[write++] = c;
  }

  if (write == 0) {
    path[0] = '/';
    path[1] = '\0';
    return true;
  }

  if (write > 1 && path[write - 1] == '/') {
    write--;
  }
  path[write] = '\0';
  return true;
}

String urlDecode(const String& path) {
  String result;
  result.reserve(path.length());

  for (size_t i = 0; i < path.length(); i++) {
    const char c = path[i];
    if (c == '%' && i + 2 < path.length()) {
      const int hi = hexValue(path[i + 1]);
      const int lo = hexValue(path[i + 2]);
      if (hi >= 0 && lo >= 0) {
        result += static_cast<char>((hi << 4) | lo);
        i += 2;
        continue;
      }
    }
    if (c == '+') {
      result += ' ';
    } else {
      result += c;
    }
  }

  return result;
}

bool urlDecode(const char* path, char* out, const size_t outSize) {
  if (path == nullptr || out == nullptr || outSize == 0) {
    return false;
  }

  size_t write = 0;
  for (size_t read = 0; path[read] != '\0'; read++) {
    char decoded = path[read];
    if (decoded == '%' && path[read + 1] != '\0' && path[read + 2] != '\0') {
      const int hi = hexValue(path[read + 1]);
      const int lo = hexValue(path[read + 2]);
      if (hi >= 0 && lo >= 0) {
        decoded = static_cast<char>((hi << 4) | lo);
        read += 2;
      }
    } else if (decoded == '+') {
      decoded = ' ';
    }

    if (write + 1 >= outSize) {
      out[0] = '\0';
      return false;
    }
    out[write++] = decoded;
  }

  out[write] = '\0';
  return true;
}

bool isValidFilename(const String& filename) { return isValidFilename(filename.c_str()); }

bool isValidFilename(const char* filename) {
  if (filename == nullptr || filename[0] == '\0') {
    return false;
  }

  const size_t length = std::strlen(filename);
  if (length > 255) {
    return false;
  }

  if (std::strchr(filename, '/') != nullptr || std::strchr(filename, '\\') != nullptr) {
    return false;
  }

  size_t invalidPos = 0;
  if (InputValidation::findAsciiControlChar(filename, length, invalidPos)) {
    return false;
  }

  for (size_t i = 0; i < length; i++) {
    const char c = filename[i];
    if (c == '"' || c == '*' || c == ':' || c == '<' || c == '>' || c == '?' || c == '|') {
      return false;
    }
  }

  if (std::strcmp(filename, ".") == 0 || std::strcmp(filename, "..") == 0) {
    return false;
  }

  return !containsTraversal(filename);
}

bool isProtectedWebComponent(const String& component) { return isProtectedWebComponent(component.c_str()); }

bool isProtectedWebComponent(const char* component) {
  if (component == nullptr || component[0] == '\0') {
    return false;
  }
  if (component[0] == '.') {
    return true;
  }
  for (size_t i = 0; i < kHiddenWebItemsCount; i++) {
    if (equalsIgnoreCase(component, kHiddenWebItems[i])) {
      return true;
    }
  }
  return false;
}

bool pathContainsProtectedItem(const String& path) { return pathContainsProtectedItem(path.c_str()); }

bool pathContainsProtectedItem(const char* path) {
  if (path == nullptr || path[0] == '\0' || (path[0] == '/' && path[1] == '\0')) {
    return false;
  }

  const char* cursor = path;
  while (*cursor != '\0') {
    while (*cursor == '/') {
      cursor++;
    }
    if (*cursor == '\0') {
      break;
    }

    const char* component = cursor;
    while (*cursor != '\0' && *cursor != '/') {
      cursor++;
    }
    const size_t componentLen = static_cast<size_t>(cursor - component);
    if (componentLen == 0) {
      continue;
    }

    if (component[0] == '.') {
      return true;
    }
    for (size_t i = 0; i < kHiddenWebItemsCount; i++) {
      if (equalsIgnoreCaseN(component, componentLen, kHiddenWebItems[i])) {
        return true;
      }
    }
  }

  return false;
}

}  // namespace PathUtils
