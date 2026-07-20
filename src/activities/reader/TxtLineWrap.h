#pragma once

#include <Utf8.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace TxtLineWrap {

inline bool isContinuationByte(const char byte) { return (static_cast<uint8_t>(byte) & 0xC0) == 0x80; }

// SD-card fonts measure these codepoints as a sum of unsigned advances. Keeping
// this guard to printable ASCII and CJK avoids bidi shaping, combining marks,
// and font ligatures, making prefix widths monotonic and safe to binary-search.
inline bool isMonotonicLtrText(const std::string& text) {
  const auto* cursor = reinterpret_cast<const unsigned char*>(text.c_str());
  const auto* const end = cursor + text.size();
  bool hasText = false;

  while (*cursor) {
    const uint32_t cp = utf8NextCodepoint(&cursor);
    if (utf8IsCjkBreakable(cp)) {
      hasText = true;
    } else if (cp >= 0x20 && cp <= 0x7E) {
      hasText = true;
    } else {
      return false;
    }
  }

  return hasText && cursor == end;
}

inline size_t nextUtf8Boundary(const std::string& text, const size_t boundary) {
  size_t next = boundary + 1;
  while (next < text.size() && isContinuationByte(text[next])) {
    ++next;
  }
  return next;
}

// Returns the largest UTF-8 prefix accepted by measure(). The full string must
// already be known not to fit, and measured prefix widths must be monotonic.
// A temporary NUL avoids allocating a substring for every probe.
template <typename Measure>
size_t findLargestFittingPrefix(std::string& text, const int maxWidth, Measure measure) {
  size_t fits = 0;
  size_t overflows = text.size();

  while (fits < overflows) {
    size_t probe = fits + (overflows - fits) / 2;
    while (probe > fits && isContinuationByte(text[probe])) {
      --probe;
    }

    if (probe == fits) {
      probe = nextUtf8Boundary(text, fits);
      if (probe >= overflows) {
        break;
      }
    }

    const char saved = text[probe];
    text[probe] = '\0';
    const int width = measure(text.c_str());
    text[probe] = saved;

    if (width <= maxWidth) {
      fits = probe;
    } else {
      overflows = probe;
    }
  }

  return fits;
}

inline size_t preserveWordBreak(const std::string& text, const size_t largestFittingPrefix) {
  const size_t space = text.rfind(' ', largestFittingPrefix);
  return space != std::string::npos && space > 0 ? space : largestFittingPrefix;
}

}  // namespace TxtLineWrap
