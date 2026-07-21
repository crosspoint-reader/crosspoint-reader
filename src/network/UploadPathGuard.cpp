#include "UploadPathGuard.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>

#include "util/BookPathMoveUtils.h"

namespace UploadPathGuard {
namespace {

bool validUtf8(const std::string_view text) {
  for (size_t i = 0; i < text.size();) {
    const uint8_t first = static_cast<uint8_t>(text[i]);
    if (first <= 0x7F) {
      if (first == 0) return false;
      ++i;
      continue;
    }

    size_t continuationCount = 0;
    uint32_t codepoint = 0;
    uint32_t minimum = 0;
    if (first >= 0xC2 && first <= 0xDF) {
      continuationCount = 1;
      codepoint = first & 0x1FU;
      minimum = 0x80;
    } else if (first >= 0xE0 && first <= 0xEF) {
      continuationCount = 2;
      codepoint = first & 0x0FU;
      minimum = 0x800;
    } else if (first >= 0xF0 && first <= 0xF4) {
      continuationCount = 3;
      codepoint = first & 0x07U;
      minimum = 0x10000;
    } else {
      return false;
    }
    if (continuationCount > text.size() - i - 1) return false;
    for (size_t j = 1; j <= continuationCount; ++j) {
      const uint8_t continuation = static_cast<uint8_t>(text[i + j]);
      if ((continuation & 0xC0U) != 0x80U) return false;
      codepoint = (codepoint << 6) | (continuation & 0x3FU);
    }
    if (codepoint < minimum || codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF)) return false;
    i += continuationCount + 1;
  }
  return true;
}

bool asciiEqualIgnoreCase(const std::string_view lhs, const std::string_view rhs) {
  if (lhs.size() != rhs.size()) return false;
  for (size_t i = 0; i < lhs.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(lhs[i])) != std::tolower(static_cast<unsigned char>(rhs[i]))) {
      return false;
    }
  }
  return true;
}

bool isTransactionArtifact(const std::string_view segment) {
  std::string owned(segment);
  if (isBookFileTransactionArtifact(owned.c_str())) return true;
  std::transform(owned.begin(), owned.end(), owned.begin(),
                 [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return isBookFileTransactionArtifact(owned.c_str());
}

bool isSafeSegment(const std::string_view segment, const size_t maxBytes) {
  if (segment.empty() || segment.size() > maxBytes || segment.front() == '.' || segment.back() == '.' ||
      segment.back() == ' ') {
    return false;
  }
  if (asciiEqualIgnoreCase(segment, "System Volume Information") || asciiEqualIgnoreCase(segment, "XTCache") ||
      isTransactionArtifact(segment)) {
    return false;
  }
  const bool bytesSafe = std::all_of(segment.begin(), segment.end(), [](const unsigned char byte) {
    return byte >= 0x20 && byte != 0x7F && byte != '/' && byte != '\\' && byte != ':' && byte != '*' && byte != '?' &&
           byte != '"' && byte != '<' && byte != '>' && byte != '|';
  });
  if (!bytesSafe) return false;
  return validUtf8(segment);
}

}  // namespace

bool isSafeLeafName(const char* name) { return name && isSafeSegment(name, MAX_LEAF_BYTES); }

bool isSafeAbsolutePath(const char* path, const bool allowRoot) {
  if (!path) return false;
  const std::string_view value(path);
  if (value == "/") return allowRoot;
  if (value.size() < 2 || value.front() != '/' || !validUtf8(value)) return false;

  size_t start = 1;
  while (start < value.size()) {
    const size_t slash = value.find('/', start);
    const size_t end = slash == std::string_view::npos ? value.size() : slash;
    if (!isSafeSegment(value.substr(start, end - start), 255)) return false;
    if (slash == std::string_view::npos) return true;
    start = slash + 1;
  }
  return value.back() == '/';
}

bool parseSize(const char* token, size_t& value) {
  if (!token || *token == '\0') return false;
  size_t parsed = 0;
  for (const unsigned char byte : std::string_view(token)) {
    if (byte < '0' || byte > '9') return false;
    const size_t digit = byte - '0';
    if (parsed > (std::numeric_limits<size_t>::max() - digit) / 10) return false;
    parsed = parsed * 10 + digit;
  }
  value = parsed;
  return true;
}

}  // namespace UploadPathGuard
