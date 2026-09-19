#pragma once

#include <algorithm>
#include <cstddef>
#include <string>

namespace clippingPreview {
constexpr size_t MAX_BYTES = 256;
constexpr size_t ELLIPSIS_BYTES = 3;

inline size_t utf8SpaceLength(const char* text, const size_t size) {
  if (size >= 2 && static_cast<unsigned char>(text[0]) == 0xC2 && static_cast<unsigned char>(text[1]) == 0xA0) return 2;
  if (size >= 3 && static_cast<unsigned char>(text[0]) == 0xE2 && static_cast<unsigned char>(text[1]) == 0x80 &&
      (static_cast<unsigned char>(text[2]) == 0x83 || static_cast<unsigned char>(text[2]) == 0xAF))
    return 3;
  return 0;
}

// Reader is a HalFile in firmware, or a byte reader in host tests. A small
// buffer bounds stack use and SD reads; no read can cross the stored text length.
template <typename Reader>
bool read(Reader& reader, size_t remaining, std::string& out) {
  out.clear();
  out.reserve(MAX_BYTES + ELLIPSIS_BYTES);
  char buffer[64];
  size_t pos = 0;
  size_t size = 0;
  const auto nextByte = [&]() -> int {
    if (pos == size) {
      if (remaining == 0) return -1;
      size = std::min(remaining, sizeof(buffer));
      if (reader.read(buffer, size) != static_cast<int>(size)) return -1;
      remaining -= size;
      pos = 0;
    }
    return static_cast<unsigned char>(buffer[pos++]);
  };

  bool spacePending = false;
  size_t prefixBytes = 0;
  while (remaining > 0 || pos < size) {
    const int first = nextByte();
    if (first < 0) {
      out.clear();
      return false;
    }
    const size_t length = first < 0x80                     ? 1
                          : first >= 0xC2 && first <= 0xDF ? 2
                          : first >= 0xE0 && first <= 0xEF ? 3
                          : first >= 0xF0 && first <= 0xF4 ? 4
                                                           : 0;
    char codepoint[4] = {static_cast<char>(first)};
    if (length == 0) {
      out.clear();
      return false;
    }
    for (size_t i = 1; i < length; ++i) {
      const int byte = nextByte();
      if (byte < 0 || (byte & 0xC0) != 0x80) {
        out.clear();
        return false;
      }
      codepoint[i] = static_cast<char>(byte);
    }
    const bool space = (length == 1 && (first == ' ' || first == '\r' || first == '\n' || first == '\t')) ||
                       utf8SpaceLength(codepoint, length) != 0;
    // Leading whitespace is exempt from the preview budget. Whitespace inside
    // the preview still counts so a long run cannot force a full-file scan.
    if (!space || !out.empty()) {
      if (prefixBytes + length > MAX_BYTES) {
        out += "\xe2\x80\xa6";
        return true;
      }
      prefixBytes += length;
    }
    if (space) {
      spacePending = !out.empty();
      continue;
    }
    if (spacePending) out += ' ';
    out.append(codepoint, length);
    spacePending = false;
  }
  return true;
}
}  // namespace clippingPreview
