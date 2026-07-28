#pragma once
#include <cstdint>

// Host-test stub for lib/EpdFont/EpdFontFamily.h. The real header pulls in
// EpdFont.h and the built-in font tables, none of which TextBlock's layout
// logic needs -- it only consumes the Style bitmask. Keep the enum values in
// lockstep with the real header: they are serialized into the page cache.
class EpdFontFamily {
 public:
  enum Style : uint8_t {
    REGULAR = 0,
    BOLD = 1,
    ITALIC = 2,
    BOLD_ITALIC = 3,
    UNDERLINE = 4,
    STRIKETHROUGH = 8,
    SUP = 16,
    SUB = 32,
    RUBY_CONTINUE = 64,
  };
  static constexpr uint8_t TEXT_DECORATION_MASK = static_cast<uint8_t>(UNDERLINE | STRIKETHROUGH);

  static constexpr bool hasTextDecoration(const Style style) {
    return (static_cast<uint8_t>(style) & TEXT_DECORATION_MASK) != 0;
  }
};
