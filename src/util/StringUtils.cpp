#include "StringUtils.h"

#include <Utf8.h>

namespace StringUtils {

namespace {
constexpr size_t MAX_PRESERVED_EXTENSION_BYTES = 16;

bool hasConventionalExtension(const std::string& name, const size_t dot) {
  const size_t extensionBytes = name.size() - dot;
  if (dot == 0 || extensionBytes < 2 || extensionBytes > MAX_PRESERVED_EXTENSION_BYTES) return false;
  for (size_t i = dot + 1; i < name.size(); i++) {
    if (!std::isalnum(static_cast<unsigned char>(name[i]))) return false;
  }
  return true;
}

std::string sanitizeFilenameRange(const std::string& name, const size_t endOffset, const size_t contentMaxBytes,
                                  const size_t reserveBytes) {
  std::string result;
  result.reserve(std::min(name.size(), reserveBytes));

  const auto* text = reinterpret_cast<const unsigned char*>(name.c_str());
  const auto* rangeEnd = text + std::min(name.size(), endOffset);

  // Skip leading spaces and dots so they don't consume the byte budget
  while (text < rangeEnd && (*text == ' ' || *text == '.')) {
    text++;
  }

  // Process full UTF-8 codepoints to avoid trimming in the middle of a multibyte sequence
  while (text < rangeEnd) {
    const auto* cpStart = text;
    uint32_t cp = utf8NextCodepoint(&text);

    if (cp == '/' || cp == '\\' || cp == ':' || cp == '*' || cp == '?' || cp == '"' || cp == '<' || cp == '>' ||
        cp == '|') {
      // Replace illegal and control characters with '_'
      if (result.length() + 1 > contentMaxBytes) break;
      result += '_';
    } else if (cp >= 128 || (cp >= 32 && cp < 127)) {
      const size_t cpBytes = text - cpStart;
      if (result.length() + cpBytes > contentMaxBytes) break;
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
}  // namespace

std::string sanitizeFilename(const std::string& name, size_t maxBytes) {
  return sanitizeFilenameRange(name, name.size(), maxBytes, maxBytes);
}

std::string sanitizeFilenamePreservingExtension(const std::string& name, const size_t maxBytes) {
  const size_t dot = name.find_last_of('.');
  if (dot == std::string::npos || !hasConventionalExtension(name, dot)) return sanitizeFilename(name, maxBytes);

  const size_t extensionBytes = name.size() - dot;
  if (extensionBytes >= maxBytes) return sanitizeFilename(name, maxBytes);

  std::string stem = sanitizeFilenameRange(name, dot, maxBytes - extensionBytes, maxBytes);
  stem.append(name, dot, extensionBytes);
  return stem;
}

}  // namespace StringUtils
