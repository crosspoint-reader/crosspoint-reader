#include "PathUtils.h"

#include <Logging.h>

#include "InputValidation.h"

namespace PathUtils {

bool containsTraversal(const String& path) {
  // Note: null byte check removed - indexOf('\0') finds the internal C-string terminator
  // Null bytes are checked separately in isValidSdPath via character iteration

  // Check for ".." as a path component (actual traversal), not just any ".." in filename
  // Patterns that indicate traversal:
  // - "/../" anywhere in path
  // - "/.." at end of path
  // - "../" at start of path
  // - path equals ".."
  if (path.indexOf("/../") >= 0) {
    LOG_WRN("PATH", "traversal: /../");
    return true;
  }
  if (path.endsWith("/..")) {
    LOG_WRN("PATH", "traversal: ends /..");
    return true;
  }
  if (path.startsWith("../")) {
    LOG_WRN("PATH", "traversal: starts ../");
    return true;
  }
  if (path == "..") {
    LOG_WRN("PATH", "traversal: equals ..");
    return true;
  }

  // Check for URL-encoded traversal variants (in case called before decoding)
  // %2e = '.', %2f = '/'
  String lower = path;
  lower.toLowerCase();
  if (lower.indexOf("%2e%2e%2f") >= 0) {
    LOG_WRN("PATH", "traversal: %%2e%%2e%%2f");
    return true;
  }
  if (lower.indexOf("%2f%2e%2e") >= 0) {
    LOG_WRN("PATH", "traversal: %%2f%%2e%%2e");
    return true;
  }
  if (lower.indexOf("..%2f") >= 0) {
    LOG_WRN("PATH", "traversal: ..%%2f");
    return true;
  }
  if (lower.indexOf("%2f..") >= 0) {
    LOG_WRN("PATH", "traversal: %%2f..");
    return true;
  }

  return false;
}

bool isValidSdPath(const String& path) {
  // Empty paths are invalid
  if (path.isEmpty()) {
    LOG_WRN("PATH", "REJECT: empty path");
    return false;
  }

  // Check length (FAT32 path limit is 255 chars)
  if (path.length() > 255) {
    LOG_WRN("PATH", "REJECT: path too long (%d chars)", path.length());
    return false;
  }

  // Must not contain traversal attempts
  if (containsTraversal(path)) {
    LOG_WRN("PATH", "REJECT: traversal in '%s'", path.c_str());
    return false;
  }

  size_t invalidPos = 0;
  if (InputValidation::findAsciiControlChar(path.c_str(), path.length(), invalidPos)) {
    LOG_WRN("PATH", "REJECT: control char at %d", static_cast<int>(invalidPos));
    return false;
  }

  for (size_t i = 0; i < path.length(); i++) {
    if (path[i] == '\\') {
      LOG_WRN("PATH", "REJECT: backslash at %d", i);
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

  // Ensure leading slash
  if (!result.startsWith("/")) {
    result = "/" + result;
  }

  // Collapse multiple consecutive slashes
  while (result.indexOf("//") >= 0) {
    result.replace("//", "/");
  }

  // Remove trailing slash unless it's the root
  if (result.length() > 1 && result.endsWith("/")) {
    result = result.substring(0, result.length() - 1);
  }

  return result;
}

namespace {
int hexValue(const char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
  if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
  return -1;
}
}  // namespace

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

bool isValidFilename(const String& filename) {
  // Empty filenames are invalid
  if (filename.isEmpty()) {
    return false;
  }

  // Check length (FAT32 filename limit)
  if (filename.length() > 255) {
    return false;
  }

  // Must not contain path separators
  if (filename.indexOf('/') >= 0 || filename.indexOf('\\') >= 0) {
    return false;
  }

  size_t invalidPos = 0;
  if (InputValidation::findAsciiControlChar(filename.c_str(), filename.length(), invalidPos)) {
    return false;
  }

  for (size_t i = 0; i < filename.length(); i++) {
    const char c = filename[i];
    if (c == '"' || c == '*' || c == ':' || c == '<' || c == '>' || c == '?' || c == '|') {
      return false;
    }
  }

  // Must not be a traversal attempt
  if (filename == "." || filename == "..") {
    return false;
  }

  // Check for traversal patterns
  if (containsTraversal(filename)) {
    return false;
  }

  // Check for null bytes
  for (size_t i = 0; i < filename.length(); i++) {
    if (filename[i] == '\0') {
      return false;
    }
  }

  return true;
}

namespace {
// Folders and files to hide from the web interface file browser.
// Items starting with "." are handled separately in isProtectedWebComponent.
constexpr const char* kHiddenWebItems[] = {"System Volume Information", "XTCache"};
constexpr size_t kHiddenWebItemsCount = sizeof(kHiddenWebItems) / sizeof(kHiddenWebItems[0]);
}  // namespace

bool isProtectedWebComponent(const String& component) {
  if (component.isEmpty()) {
    return false;
  }
  if (component.startsWith(".")) {
    return true;
  }
  for (size_t i = 0; i < kHiddenWebItemsCount; i++) {
    if (component.equalsIgnoreCase(kHiddenWebItems[i])) {
      return true;
    }
  }
  return false;
}

bool pathContainsProtectedItem(const String& path) {
  const String normalized = normalizePath(path);
  if (normalized == "/") {
    return false;
  }
  int start = normalized.startsWith("/") ? 1 : 0;
  while (start < static_cast<int>(normalized.length())) {
    int slash = normalized.indexOf('/', start);
    if (slash < 0) {
      slash = normalized.length();
    }
    const String component = normalized.substring(start, slash);
    if (isProtectedWebComponent(component)) {
      return true;
    }
    start = slash + 1;
  }
  return false;
}

}  // namespace PathUtils
